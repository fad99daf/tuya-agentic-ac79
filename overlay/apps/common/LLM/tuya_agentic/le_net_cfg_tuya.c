// le_net_cfg_tuya.c — AC79 上的涂鸦 BLE 配网传输层
// GATT 服务端范式照抄 apps/common/ble/le_net_cfg.c,协议解析换成 agentic-kit 的 tuya-ble 模块。
// App 经 write 特征(...0001)下发数据 → tuya_ble_prov_on_data 解析 → 回调出 {ssid,password,token}
// 设备经 notify 特征(...0002)回传(send_fn);read 特征(...0003)返回设备信息。
// ⚠️ 初稿:AC79 BLE 控制器/栈的具体 bring-up 顺序、部分宏,需结合 wifi_story_machine 的 BT 初始化
//    实际跑通一轮编译/上电验证调整。
#include "app_config.h"
#include "system/includes.h"
#include "os/os_api.h"

/* AC79 BTstack 头(与 le_net_cfg.c 同套)*/
#include "btstack/btstack_typedef.h"
#include "btstack/le/att.h"
#include "btstack/le/le_common_define.h"
#include "btstack/le/sm.h"
#include "btstack/le/ble_api.h"
#include "btstack/le/gap.h"

#include "le_net_cfg_tuya.h"          /* profile_data + handle 宏 + 对外 API */
#include "tuya_ble_prov.h"            /* agentic-kit 涂鸦配网协议 */

/* AC79 BLE 事件/adv 相关宏(le_net_cfg.c 里用的,这里引用)*/
#ifndef HCI_EVENT_PACKET
#define HCI_EVENT_PACKET 0x04
#endif
extern void le_device_db_init(void);
extern void hci_event_callback_set(void (*cb)(u8, u16, u8 *, u16));
extern void le_l2cap_register_packet_handler(void (*cb)(u8, u16, u8 *, u16));
extern void bt_ble_adv_enable(u8 enable);
/* ble_user_cmd_prepare / att_get_ccc_config / sm_* 用头里(ble_api.h/att.h/sm.h)的声明 */

/* tuya-ble 需要的 HAL:随机数 */
void tuya_ble_hal_random(uint8_t *buf, size_t len)
{
    extern u32 timer_get_ms(void);
    static u32 seed = 0;
    if (seed == 0) { seed = timer_get_ms() ^ 0xA5A5A5A5u; }
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;          /* 简单 LCG;正式可用 AC79 TRNG */
        buf[i] = (uint8_t)(seed >> 16);
    }
}

/* ---------------- 状态 ---------------- */
static tuya_ble_prov_state_t s_prov;          /* tuya-ble 协议状态(含收发缓冲,~2KB)*/
static volatile u16 s_con_handle = 0;
static volatile u8  s_notify_enabled = 0;
static volatile u8  s_prov_done = 0;           /* 已拿到 {ssid,password,token} */
static tuya_prov_result_cb_t s_user_cb = NULL;
static OS_SEM s_prov_sem;                      /* 配网完成信号(阻塞 tuya_ble_netcfg_start)*/

/* ---------------- notify 回传(send_fn)---------------- */
static int tuya_hal_send(const uint8_t *buf, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_con_handle || !s_notify_enabled) {
        printf("[tuya_ble] send SKIP: con=%d notify_en=%d\r\n", s_con_handle, s_notify_enabled);
        return -1;
    }
    int ret = att_server_notify(s_con_handle, TUYA_HANDLE_NOTIFY_VAL, (uint8_t *)buf, len);
    printf("[tuya_ble] notify: con=%d len=%d ret=%d\r\n", s_con_handle, len, ret);
    return ret;
}

/* ---------------- tuya-ble 解出凭据的回调 ---------------- */
static void prov_complete_cb(const tuya_ble_wifi_creds_t *creds)
{
    printf("[tuya_ble] prov done: ssid_len=%u token_len=%u\r\n",
           (unsigned int)strlen(creds->ssid), (unsigned int)strlen(creds->token));
    s_prov_done = 1;
    if (s_user_cb) {
        s_user_cb(creds);
    }
    os_sem_post(&s_prov_sem);     /* 唤醒阻塞的 tuya_ble_netcfg_start */
}

/* ---------------- ATT 读回调 ---------------- */
/* read 特征(...0003):返回 tuya_ble_prov_get_read_payload(adv+rsp) */
static u16 tuya_att_read(u16 con, u16 handle, u16 offset, u8 *buffer, u16 bufsize)
{
    (void)con;
    if (handle == TUYA_HANDLE_DEV_NAME) {
        static const char name[] = "TUYA";
        if (!buffer) return sizeof(name) - 1;
        u16 n = sizeof(name) - 1;
        if (offset >= n) return 0;
        if (offset + bufsize > n) bufsize = n - offset;
        memcpy(buffer, name + offset, bufsize);
        return bufsize;
    }
    if (handle == TUYA_HANDLE_READ_VAL) {
        const u8 *adv = NULL, *rsp = NULL; u8 al = 0, rl = 0;
        tuya_ble_prov_get_read_payload(&s_prov, &adv, &al, &rsp, &rl);
        u16 total = (u16)al + (u16)rl;
        if (!buffer) return total;
        if (offset >= total) return 0;
        /* 简化:adv 与 rsp 拼接返回 */
        u8 tmp[64];
        if (total > sizeof(tmp)) total = sizeof(tmp);
        memcpy(tmp, adv, al);
        if (rl) memcpy(tmp + al, rsp, (total - al > rl) ? rl : total - al);
        if (offset + bufsize > total) bufsize = total - offset;
        memcpy(buffer, tmp + offset, bufsize);
        return bufsize;
    }
    if (handle == TUYA_HANDLE_NOTIFY_CCC) {
        if (buffer) { buffer[0] = att_get_ccc_config(handle); buffer[1] = 0; }
        return 2;
    }
    return 0;
}

/* ---------------- ATT 写回调 ---------------- */
/* write 特征(...0001):App 写的数据喂给 tuya-ble 协议 */
static int tuya_att_write(u16 con, u16 handle, u16 transaction, u16 offset, u8 *buffer, u16 bufsize)
{
    (void)transaction; (void)offset;
    s_con_handle = con;

    if (handle == TUYA_HANDLE_WRITE_VAL) {
        /* 不在 btstack 任务(栈只有 4KB)里直接跑 mbedTLS 加密;
         * 转发到大栈的 worker 任务处理。 */
        extern void tuya_ble_prov_worker(const u8 *data, u16 len);
        tuya_ble_prov_worker(buffer, bufsize);
    }
    if (handle == TUYA_HANDLE_NOTIFY_CCC) {
        s_notify_enabled = (buffer && (buffer[0] & 0x01)) ? 1 : 0;
        att_set_ccc_config(handle, buffer ? (buffer[0] | (buffer[1] << 8)) : 0);
        printf("[tuya_ble] notify %s\r\n", s_notify_enabled ? "enabled" : "disabled");
    }
    return 0;
}

/* ---------------- 广播数据(用 tuya_ble_prov_get_adv_data 给的字节)---------------- */
static void tuya_make_adv(void)
{
    const u8 *adv = NULL, *rsp = NULL; u8 al = 0, rl = 0;
    tuya_ble_prov_get_adv_data(&s_prov, &adv, &al, &rsp, &rl);
    ble_user_cmd_prepare(BLE_CMD_ADV_DATA, 2, al, (void *)adv);
    ble_user_cmd_prepare(BLE_CMD_RSP_DATA, 2, rl, (void *)rsp);
    ble_user_cmd_prepare(BLE_CMD_ADV_PARAM, 3, 0x30 /* interval*0.625ms */, 0 /*ADV_IND*/, 0x07 /*3 channels*/);
}

/* ---------------- HCI/ATT 事件回调 ---------------- */
static void tuya_pkt_handler(u8 packet_type, u16 channel, u8 *packet, u16 size)
{
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    switch (packet[0]) {
    case HCI_EVENT_LE_META:
        switch (hci_event_le_meta_get_subevent_code(packet)) {
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
        case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE:
            s_con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            printf("[tuya_ble] connected, handle=%d\r\n", s_con_handle);
            tuya_ble_prov_reset_conn(&s_prov);
            break;
        default:
            break;
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("[tuya_ble] disconnect, restart adv\r\n");
        s_con_handle = 0;
        s_notify_enabled = 0;
        tuya_ble_prov_set_paired(&s_prov, false);
        if (!s_prov_done) {
            tuya_make_adv();
            bt_ble_adv_enable(1);
        }
        break;
    case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
        printf("[tuya_ble] MTU=%d\r\n", att_event_mtu_exchange_complete_get_MTU(packet));
        break;
    case ATT_EVENT_CAN_SEND_NOW:
        break;
    default:
        break;
    }
}

/* ---------------- 初始化 + 启动 ---------------- */
static void tuya_ble_profile_init(void)
{
    /* wifi_story_machine 启动时已做过的(sm_init / le_device_db_init / hci_event_callback_set /
     * le_l2cap_register_packet_handler)不能重复调,否则 ASSERT("sm init again")崩溃。
     * 这里只做:把 GATT profile 换成涂鸦的 + 注册 ATT 事件回调。*/
    att_server_init(profile_data, tuya_att_read, tuya_att_write);
    att_server_register_packet_handler(tuya_pkt_handler);
}

int tuya_ble_netcfg_start(const char *device_name,
                          const char *product_key,
                          const char *uuid,
                          const char *auth_key,
                          tuya_prov_result_cb_t cb)
{
    if (!device_name || !product_key || !uuid || !auth_key || !cb) {
        return -1;
    }
    printf("[tuya_ble] netcfg start: name=%s pid=%s uuid=%s\r\n", device_name, product_key, uuid);
    s_user_cb = cb;
    s_prov_done = 0;
    s_con_handle = 0;
    s_notify_enabled = 0;
    os_sem_create(&s_prov_sem, 0);

    /* 初始化 tuya-ble 协议(产品三件套 + send_fn + 完成回调)*/
    tuya_ble_prov_cfg_ext_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device_name = device_name;
    cfg.product_key = product_key;
    cfg.uuid = uuid;
    cfg.auth_key = auth_key;
    cfg.cb = prov_complete_cb;
    cfg.send_fn = tuya_hal_send;
    cfg.send_ctx = NULL;
    if (tuya_ble_prov_init(&s_prov, &cfg) != 0) {
        printf("[tuya_ble] tuya_ble_prov_init fail\r\n");
        return -2;
    }
    printf("[tuya_ble] prov_init ok\r\n");

    tuya_ble_profile_init();
    printf("[tuya_ble] profile_init done\r\n");

    tuya_make_adv();
    printf("[tuya_ble] make_adv done\r\n");

    /* 直接用底层 BLE 命令开广播,绕过 set_adv_enable 的 adv_ctrl_en 检查 */
    ble_user_cmd_prepare(BLE_CMD_ADV_ENABLE, 1, 1);
    printf("[tuya_ble] adv_enable(1) done, BLE broadcasting now\r\n");

    /* 阻塞等配网完成(prov_complete_cb 里 post 信号量)*/
    /* TODO: 加超时(如 60s),超时返回 -3。os_sem_pend 第 2 参为 tick,0=无限 */
    os_sem_pend(&s_prov_sem, 0);
    return s_prov_done ? 0 : -3;
}

void tuya_ble_netcfg_stop(void)
{
    ble_user_cmd_prepare(BLE_CMD_ADV_ENABLE, 1, 0);
    printf("[tuya_ble] stopped\r\n");
}

/* ---- worker 任务:替 btstack 任务(栈只有 4KB)跑 mbedTLS 加密 ---- */
static u8 s_prov_rx_buf[512];
static u16 s_prov_rx_len;
static volatile u8 s_prov_rx_flag;

static void tuya_ble_prov_worker_task(void *arg)
{
    while (1) {
        os_taskq_pend("taskq", (int []){0}, 1);  /* 等消息 */
        if (s_prov_rx_flag) {
            s_prov_rx_flag = 0;
            printf("[tuya_ble] worker processing %d bytes\r\n", s_prov_rx_len);
            tuya_ble_prov_on_data(&s_prov, s_prov_rx_buf, s_prov_rx_len);
        }
    }
}

void tuya_ble_prov_worker(const u8 *data, u16 len)
{
    if (len > sizeof(s_prov_rx_buf)) len = sizeof(s_prov_rx_buf);
    memcpy(s_prov_rx_buf, data, len);
    s_prov_rx_len = len;
    s_prov_rx_flag = 1;
    os_taskq_post("tuya_prov_w", 0);
}

static int tuya_ble_worker_init(void)
{
    return thread_fork("tuya_prov_w", 5, 2 * 1024, 8, 0, tuya_ble_prov_worker_task, NULL);
}
early_initcall(tuya_ble_worker_init);
