// le_net_cfg_tuya.h — 涂鸦 BLE 配网的 GATT profile_data
// 128-bit UUID 编码规则取自 le_net_cfg_matter.h(那个文件本身就是从涂鸦 profile 改的):
//   特征值声明 size=0x1b = 头8 + properties1 + value_handle2 + uuid128_16
//   特征值属性 size=0x16 = 头6 + uuid128_16(DYNAMIC 无静态 value)
//   靠 size 区分 16/128 位 UUID,不需要特殊标志位。
// 涂鸦 UUID(小端,与 ESP32 tuya_ble_nimble.c 的 BLE_UUID128 一致):
//   write  ...0001 / notify ...0002 / read ...0003,base = 0000xxxx-0010-0108-0000-5F9B07D0
//   小端 16 字节:D0 07 9B 5F 80 00 01 80 01 10 00 00 [XX] 00 00 00
#ifndef _LE_NET_CFG_TUYA_H
#define _LE_NET_CFG_TUYA_H

#include <stdint.h>

// 涂鸦三个 128-bit 特征 UUID(小端 16 字节)
#define TUYA_UUID_WRITE  0xD0,0x07,0x9B,0x5F,0x80,0x00,0x01,0x80,0x01,0x10,0x00,0x00,0x01,0x00,0x00,0x00
#define TUYA_UUID_NOTIFY 0xD0,0x07,0x9B,0x5F,0x80,0x00,0x01,0x80,0x01,0x10,0x00,0x00,0x02,0x00,0x00,0x00
#define TUYA_UUID_READ   0xD0,0x07,0x9B,0x5F,0x80,0x00,0x01,0x80,0x01,0x10,0x00,0x00,0x03,0x00,0x00,0x00

static const uint8_t profile_data[] = {
    // 0x0001 PRIMARY_SERVICE 1800 (GAP)
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    // 0x0002 CHARACTERISTIC 2a00 (Device Name) READ | DYNAMIC
    0x0d, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2a,
    // 0x0003 VALUE 2a00 READ | DYNAMIC
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2a,

    // 0x0004 PRIMARY_SERVICE FD50 (涂鸦配网服务)
    0x0a, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28, 0x50, 0xFD,

    // 0x0005 CHARACTERISTIC ...0001 (write) WRITE | DYNAMIC —— App 写配网数据到这里
    0x1b, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28, 0x08, 0x06, 0x00, TUYA_UUID_WRITE,
    // 0x0006 VALUE ...0001 WRITE | DYNAMIC
    0x16, 0x00, 0x08, 0x01, 0x06, 0x00, TUYA_UUID_WRITE,

    // 0x0007 CHARACTERISTIC ...0002 (notify) NOTIFY —— 设备回传给 App
    0x1b, 0x00, 0x02, 0x00, 0x07, 0x00, 0x03, 0x28, 0x10, 0x08, 0x00, TUYA_UUID_NOTIFY,
    // 0x0008 VALUE ...0002 NOTIFY
    0x16, 0x00, 0x10, 0x00, 0x08, 0x00, TUYA_UUID_NOTIFY,
    // 0x0009 CLIENT_CHARACTERISTIC_CONFIGURATION(notify 使能)
    0x0a, 0x00, 0x0a, 0x01, 0x09, 0x00, 0x02, 0x29, 0x00, 0x00,

    // 0x000a CHARACTERISTIC ...0003 (read) READ | DYNAMIC —— App 读设备信息
    0x1b, 0x00, 0x02, 0x00, 0x0a, 0x00, 0x03, 0x28, 0x02, 0x0b, 0x00, TUYA_UUID_READ,
    // 0x000b VALUE ...0003 READ | DYNAMIC
    0x16, 0x00, 0x02, 0x01, 0x0b, 0x00, TUYA_UUID_READ,

    // END
    0x00, 0x00,
};

// 特征值 handle(供 att 回调 / notify 用)
#define TUYA_HANDLE_DEV_NAME        0x0003
#define TUYA_HANDLE_WRITE_VAL       0x0006
#define TUYA_HANDLE_NOTIFY_VAL      0x0008
#define TUYA_HANDLE_NOTIFY_CCC      0x0009
#define TUYA_HANDLE_READ_VAL        0x000b

#ifdef __cplusplus
extern "C" {
#endif

/* 配网结果回调:解出 {ssid, password, token} 后调用 */
struct tuya_ble_wifi_creds; // 定义在 agentic-kit tuya_ble_prov.h
typedef void (*tuya_prov_result_cb_t)(const struct tuya_ble_wifi_creds *creds);

/* 启动涂鸦 BLE 配网(无 devid 时调用)。阻塞广告直到配网完成或超时。
 *   device_name : 广播名("TUYA")
 *   product_key : 产品 PID
 *   uuid/auth_key : 设备授权码(产品三件套)
 *   cb : 解出 {ssid,password,token} 后回调
 * 返回 0=配网成功(cb 已带 creds),<0=失败/超时。*/
int tuya_ble_netcfg_start(const char *device_name,
                          const char *product_key,
                          const char *uuid,
                          const char *auth_key,
                          tuya_prov_result_cb_t cb);

/* 停止本配网 profile 的广播和连接。
 * 注意：本模块没有调用官方 bt_ble_init()，因此不能调用 bt_ble_exit()。*/
int tuya_ble_netcfg_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* _LE_NET_CFG_TUYA_H */
