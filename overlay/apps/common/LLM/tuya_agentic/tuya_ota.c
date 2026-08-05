/*
 * tuya_ota.c -- 涂鸦云 OTA 编排层:把涂鸦的云协议和杰理的下载烧写串起来。
 *
 * 设计依据:
 *   - 涂鸦 OTA 是 pull 模型,走 ATOP over HTTPS(不走 MQTT 推送)。三个云端 API:
 *       tuya_iot_ota_check_upgrade  -> tuya.device.upgrade.get      查是否有升级
 *       tuya_iot_ota_report_status  -> tuya.device.upgrade.status.update  上报升级状态
 *       (report_version 由 iot_client_init 自动调用)
 *     协议实现见 agentic-kit/modules/iot-client/src/iot_ota.c / atop.c。
 *   - 下载烧写复用杰理现成链路:apps/common/update/http_update.c 的 get_update_data(url)
 *       -> net_fopen("update-ota.ufw") -> dual_bank_update_write -> net_fclose
 *       -> 烧 boot info -> 2s 后 system_reset(自动重启)。
 *     http_ops 内部根据 URL 的 https:// 前缀自动走 TLS(mbedtls VERIFY_OPTIONAL,
 *     不配 CA 也能握手),涂鸦给的 httpsUrl / cdnUrl 都能下。
 *
 * 流程(对齐 xiaozhi-esp32 的 Ota::CheckTuyaVersion):
 *   1. check_upgrade  -> 无升级:返回 0(继续连 AI)
 *   2. report_status(UPGRADING)
 *   3. 播报"正在升级"提示音
 *   4. get_update_data(url)  下载+烧写(成功则内部自动重启,函数不返回)
 *
 * ⚠️ 延迟保护(开机体验):check_upgrade 走 ATOP HTTPS,涂鸦 SDK 内部超时硬编码 5s
 *   (IOT_HTTP_TIMEOUT_MS_DEFAULT)。若网络/TLS 异常导致握手失败,会卡满 5s 才返回,
 *   严重拖慢开机("说了几次你好才有反应")。故把 check_upgrade 放独立线程,主线程用
 *   信号量限时等待(TUYA_OTA_CHECK_TIMEOUT_MS):超时就放弃 OTA 直接连 AI,不让查询
 *   阻塞开机。仅当查询【快速成功且有升级】时,才进入下载烧写(那一步值得等)。
 *
 * 时序约束:本函数复用调用方的 iot_client(已 init、已建 MQTT),必须在
 *   iot_client_deinit 之前调用。典型调用点 = 开机连 AI 之前。
 */
#include "app_config.h"          /* TUYA_FIRMWARE_VERSION 等(纯宏,不引入 SDK FILE) */

#ifdef TUYA_OTA_ENABLE

#include <stdlib.h>
#include <string.h>

#include "system/includes.h"     /* printf / os_time_dly / thread_fork / OS_SEM 等 */
#include "system/os/os_api.h"

#include "iot_client.h"
#include "iot_ota.h"
#include "tuya_ota.h"

/* check_upgrade 阶段的最大等待时间。涂鸦 SDK 内部 HTTP 超时硬编码 5s
 * (IOT_HTTP_TIMEOUT_MS_DEFAULT)。设 7s 给足裕量:正常情况查询会在 5s 内(成功或失败
 * 如 -7)自行结束并 post 信号量,os_sem_pend 提前成功返回(wait==0),不会走超时分支。
 * 超时分支(8s)只在 SDK 异常卡死时才触发——此时等下去也无意义,但为避免后台线程仍
 * 在用 client(主流程随后会 iot_client_deinit)导致踩释放,超时分支继续等到线程退出。*/
#define TUYA_OTA_CHECK_TIMEOUT_MS   7000

/* 杰理 HTTP/HTTPS 下载+烧写入口(下载 https URL 时 http_ops 自动走 TLS)。
 * 声明见 include_lib/update/http_update.h。*/
extern int get_update_data(const char *url);

/* 提示音播报(实现见 app_music.c,照搬 app_music_play_netcfg_prompt 的导出模式)。
 * type: 0=正在升级(OtaInUpdate.mp3) 1=升级成功(OtaSuccess.mp3) 2=升级失败(OtaFailed.mp3) */
extern void app_music_play_ota_prompt(int type);

/* check_upgrade 独立线程参数 + 结果(主线程<->ota_check 线程) */
typedef struct {
    iot_client_t *client;
    tuya_iot_ota_upgrade_info_t info;   /* 线程填,主线程读 */
    int rt;                              /* check_upgrade 返回码 */
    OS_SEM sem;                          /* 通知主线程:查询结束 */
    volatile int done;                   /* 线程置 1 表示已填好结果 */
} ota_check_ctx_t;

static void tuya_ota_check_task(void *arg)
{
    ota_check_ctx_t *c = (ota_check_ctx_t *)arg;
    c->rt = tuya_iot_ota_check_upgrade(c->client, 0, &c->info);
    c->done = 1;
    os_sem_post(&c->sem);
    /* 线程到此结束;info 的释放由主线程负责(主线程拿走结果后 free) */
}

/* 实际执行下载+烧写的核心(查询已成功,这一步值得花时间)。返回 1=升级成功待重启。 */
static int tuya_ota_do_upgrade(iot_client_t *client, tuya_iot_ota_upgrade_info_t *info)
{
    printf("[TUYA-OTA] found upgrade: ver=%s size=%ld url=%s md5=%s\r\n",
           info->version ? info->version : "(null)",
           info->file_size,
           info->url,
           info->md5 ? info->md5 : "(null)");

    /* 上报"升级中" */
    int rt = tuya_iot_ota_report_status(client, 0, OTA_STATUS_UPGRADING);
    if (rt != 0) {
        printf("[TUYA-OTA] warn: report UPGRADING err=%d (continue anyway)\r\n", rt);
    }

    /* 播报"正在升级,请勿断电"。同步播完再下载,避免下载太快打断提示音。
     *   get_update_data 内部首块数据到达后还有 os_time_dly(500) 的额外窗口。*/
    printf("[TUYA-OTA] playing upgrade prompt, do NOT power off...\r\n");
    app_music_play_ota_prompt(0);
    os_time_dly(200);   /* ~2s:让提示音播完再开始下载 */

    /* url 和 version 都拷一份:info 由 client 的 PAL 分配,本函数内会
     *   tuya_iot_ota_upgrade_info_free 释放掉,之后不能再碰 info->*。
     *   url 给 get_update_data 用;version 给存 USER flash 用(让重启后新固件
     *   上报这个版本,停掉升级循环)。两者一起拷,free 后即无依赖。*/
    char *url_copy     = info->url     ? strdup(info->url)     : NULL;
    char *ver_copy     = info->version ? strdup(info->version) : NULL;
    tuya_iot_ota_upgrade_info_free(client, info);   /* 尽早释放 info,降低下载期内存占用 */
    if (!url_copy) {
        printf("[TUYA-OTA] strdup url OOM\r\n");
        free(ver_copy);
        tuya_iot_ota_report_status(client, 0, OTA_STATUS_UPGRD_EXEC);
        return 0;
    }

    /* ★★ 关键时序:必须在 get_update_data 之前把版本号写进 USER flash ★★
     * get_update_data 内部 net_fclose 会安排 "system_reset after 2s",返回后 reset
     * 倒计时已在跑。若在 get_update_data 返回后才写,写 flash 操作会和 reset 撞车,
     * 导致数据损坏(VM 方案已实证:重启后读 hdl=192/181 均 CRC 失败)。
     * 在下载之前写则完全安全:下载+烧写耗时十几秒,数据早已稳定落盘。
     *
     * 边界处理:若下载/烧写失败,必须清掉刚写的版本号——否则设备实际还是旧固件,
     * 但 USER flash 里已写新版本号,下次开机会上报新版本→平台不再推升级→设备永久卡在旧版本。*/
    tuya_save_upgraded_sw_ver(ver_copy);

    printf("[TUYA-OTA] downloading & flashing...\r\n");
    int dl_ret = get_update_data(url_copy);
    free(url_copy);

    if (dl_ret == 0) {
        /* 成功:get_update_data 内部 net_fclose 已烧 boot info 并安排 2s 后 system_reset。
         * 抢在这 2s 窗口内上报 FINI(对齐 xiaozhi-esp32:reboot 前上报成功)。
         * 版本号已在下载前写入 VM,这里不再碰 VM(避免和 reset 撞车损坏数据)。*/
        free(ver_copy);
        printf("[TUYA-OTA] flash done, report FINI & wait reboot...\r\n");
        tuya_iot_ota_report_status(client, 0, OTA_STATUS_UPGRAD_FINI);
        app_music_play_ota_prompt(1);
        os_time_dly(150);   /* 给提示音/上报一点时间,再让 net_fclose 的 reset 生效 */
        return 1;
    }

    /* 失败:清掉刚写的版本号(防止"固件没换成但VM版本号已更新"导致以后升不上去),
     * 上报失败状态 + 播报,返回 0 让调用方继续连 AI(下轮开机再重试)。*/
    tuya_clear_upgraded_sw_ver();
    free(ver_copy);

    /* 失败:上报失败状态 + 播报,返回 0 让调用方继续连 AI(下轮开机再重试) */
    printf("[TUYA-OTA] download/flash FAILED ret=%d, report EXEC & continue\r\n", dl_ret);
    tuya_iot_ota_report_status(client, 0, OTA_STATUS_UPGRD_EXEC);
    app_music_play_ota_prompt(2);
    return 0;
}

int tuya_ota_check_and_upgrade(iot_client_t *client)
{
    if (client == NULL) {
        printf("[TUYA-OTA] client NULL, skip\r\n");
        return 0;
    }

    /* 把 check_upgrade 放独立线程,主线程限时等待——防止 ATOP HTTPS 握手失败时
     * 卡满 SDK 内部 5s 超时,拖慢开机("说了几次你好才有反应"的根因之一)。*/
    ota_check_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.client = client;
    os_sem_create(&ctx.sem, 0);

    thread_fork("tuya_ota_chk", 4, 8 * 1024, 0, 0, tuya_ota_check_task, &ctx);

    int wait = os_sem_pend(&ctx.sem, TUYA_OTA_CHECK_TIMEOUT_MS);

    if (wait != 0) {
        /* 超时:查询线程仍在卡(SDK 异常,正常应在 5s 内退出)。不能直接 return——
         * 主流程随后会 iot_client_deinit,后台线程若仍持有 client 会踩已释放内存。
         * 这里继续阻塞等到线程退出(最坏再等 SDK 内部 5s 超时)。正常不会走到这里。*/
        printf("[TUYA-OTA] check_upgrade slow(>%dms), wait thread exit...\r\n",
               TUYA_OTA_CHECK_TIMEOUT_MS);
        os_sem_pend(&ctx.sem, 0);   /* 0 = 无限等,确保线程退出后再 return */
        printf("[TUYA-OTA] check_upgrade thread exited, skip\r\n");
        os_sem_del(&ctx.sem, 0);
        return 0;
    }

    os_sem_del(&ctx.sem, 0);   /* 正常返回:信号量用完释放 */

    /* 查询已返回 */
    if (ctx.rt != 0) {
        printf("[TUYA-OTA] check_upgrade err=%d, skip\r\n", ctx.rt);
        return 0;
    }
    if (!ctx.info.has_upgrade || !ctx.info.url) {
        printf("[TUYA-OTA] no upgrade (current=%s)\r\n", TUYA_FIRMWARE_VERSION);
        tuya_iot_ota_upgrade_info_free(client, &ctx.info);
        return 0;
    }

    /* 有升级:进入下载烧写(这一步值得花时间,主线程继续等)。*/
    return tuya_ota_do_upgrade(client, &ctx.info);
}

/* ===== 版本号管理(手动配置方案)=====
 * 每次发版前在 app_config.h 里改 TUYA_FIRMWARE_VERSION,然后编译+出 OTA 包+传平台。
 * 版本号必须和涂鸦平台上传固件时填的版本号一致。
 * 不再尝试跨 OTA 自动持久化(VM/USER/BTIF/RTC 均实测不可靠)。*/
const char *tuya_get_effective_sw_ver(void)
{
    return TUYA_FIRMWARE_VERSION;
}

void tuya_save_upgraded_sw_ver(const char *ver)
{
    /* 手动方案:不需要运行时存储,此函数保留为空(调用方仍调用,不报错) */
    (void)ver;
}

void tuya_clear_upgraded_sw_ver(void)
{
    /* 手动方案:不需要运行时存储 */
}

#else  /* !TUYA_OTA_ENABLE */

/* OTA 关闭时提供空实现,调用方无需 #ifdef 包裹(虽然 tuya_agentic_demo.c
 * 本身也用 TUYA_OTA_ENABLE 门控了,这里双保险)。*/
#include "iot_client.h"
#include "tuya_ota.h"
int tuya_ota_check_and_upgrade(iot_client_t *client)
{
    (void)client;
    return 0;
}

/* OTA 关闭时版本号直接用源码基线(不读 VM,因为没有写 VM 的机会)。*/
const char *tuya_get_effective_sw_ver(void)
{
    return TUYA_FIRMWARE_VERSION;
}

void tuya_save_upgraded_sw_ver(const char *ver)
{
    (void)ver;
}

void tuya_clear_upgraded_sw_ver(void)
{
}

#endif /* TUYA_OTA_ENABLE */
