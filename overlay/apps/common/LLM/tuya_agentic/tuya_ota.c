/*
 * tuya_ota.c -- 涂鸦云 OTA 编排层:把涂鸦的云协议和杰理的下载烧写串起来。
 *
 * 设计依据:
 *   - 涂鸦 OTA 的"查升级"是 pull 模型,走 ATOP over HTTPS。云端 API:
 *       tuya_iot_ota_check_upgrade         -> tuya.device.upgrade.get         拉当前待升级任务(APP 确认后)
 *       tuya_iot_ota_check_upgrade_silent  -> tuya.device.upgrade.silent.get  设备自检,只返回静默升级任务
 *       tuya_iot_ota_report_status         -> tuya.device.upgrade.status.update  上报升级状态
 *       (report_version 由 iot_client_init 自动调用)
 *     升级方式(提醒/静默)的区分在云端按【接口】分流,不在响应字段里(对齐
 *     TuyaOpen matop_service.c:auto_upgrade_info_get=静默自检,upgrade_info_get=
 *     确认后拉包)。2026-09-29 教训:开机自检误用 upgrade.get,平台"APP提醒
 *     升级"任务被直接拉下来刷,"提醒"变"强制"。
 *     协议实现见 agentic-kit/modules/iot-client/src/iot_ota.c / atop.c。
 *   - 下载烧写链路(2026-09-29 起自持):照搬 apps/common/update/http_update.c 的
 *       get_update_data 下载循环到本文件 -> net_fopen("update-ota.ufw") ->
 *       dual_bank_update_write -> net_fclose -> 烧 boot info -> 2s 后 system_reset,
 *       并在 read->write 之间逐块喂涂鸦摘要校验器 tuya_iot_ota_verify_*(移植自
 *       上游 agentic-kit e09d502):烧 boot info 之前比对云端摘要,不匹配按失败
 *       处理(不烧 boot info 不重启,继续跑旧备份区,下次开机重试)。
 *     http_ops 内部根据 URL 的 https:// 前缀自动走 TLS(mbedtls VERIFY_OPTIONAL,
 *     不配 CA 也能握手),涂鸦给的 httpsUrl / cdnUrl 都能下。
 *
 * 流程:
 *   1. check_upgrade  -> 无升级:返回 0(继续连 AI)
 *   2. report_status(UPGRADING) + 进度 0%
 *   3. 播报"正在升级"提示音
 *   3.5 写 USER 区版本记录(云端任务版本,防旧宏包死循环——必须在下载前,
 *       net_fclose 后有 2s reset 倒计时再写必撞车;见文件尾"版本号管理")
 *   4. 下载+烧写(自持循环):尺寸比对 file_size(-4 不等即止,一块不写) ->
 *      逐块喂摘要校验 + 进度上报(协议 16,>5% 节流) -> 全部落盘后烧 boot info
 *      前比对云端摘要(-3 不匹配不烧不重启) -> 成功 net_fclose 安排 2s 后重启
 *   5. 成功:进度 100% + report_status(COMPLETE);失败:report_status(ERROR)
 *
 * 两个入口、两条查询接口(2026-09-29 定稿):
 *   - 开机:tuya_ota_check_and_upgrade(client) —— 走 silent.get,云端只返回
 *     静默升级任务;平台配"APP提醒升级"的任务开机【不】拉,等用户确认;
 *   - APP 确认升级:tuya_ota_check_and_upgrade_channel(client, ch) —— 云端
 *     MQTT protocol 15 推送(App 里点了"确认升级")后,worker 线程走
 *     upgrade.get 拉包(上游 agentic-kit eb19466 同款流程,demo 侧
 *     ota_confirm_callback 只置标志)。
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
#include "http/http_cli.h"      /* httpcli_ctx/http_ops:HTTP(S) 下载操作集(https 自动走 TLS) */
#include "update/net_update.h"  /* net_fopen/net_fwrite/net_fclose + CONFIG_UPGRADE_OTA_FILE_NAME */
#include "tuya_auth_region.h"   /* USER区版本记录:tuya_save_upgraded_sw_ver 的持久化载体 */

/* check_upgrade 阶段的最大等待时间。涂鸦 SDK 内部 HTTP 超时硬编码 5s
 * (IOT_HTTP_TIMEOUT_MS_DEFAULT)。设 7s 给足裕量:正常情况查询会在 5s 内(成功或失败
 * 如 -7)自行结束并 post 信号量,os_sem_pend 提前成功返回(wait==0),不会走超时分支。
 * 超时分支(8s)只在 SDK 异常卡死时才触发——此时等下去也无意义,但为避免后台线程仍
 * 在用 client(主流程随后会 iot_client_deinit)导致踩释放,超时分支继续等到线程退出。*/
#define TUYA_OTA_CHECK_TIMEOUT_MS   7000

/* 提示音播报(实现见 app_music.c,照搬 app_music_play_netcfg_prompt 的导出模式)。
 * type: 0=正在升级(OtaInUpdate.mp3) 1=升级成功(OtaSuccess.mp3) 2=升级失败(OtaFailed.mp3) */
extern void app_music_play_ota_prompt(int type);

/* check_upgrade 独立线程参数 + 结果(主线程<->ota_check 线程) */
typedef struct {
    iot_client_t *client;
    int channel;                         /* 固件通道(0=主固件;APP 确认升级非 0) */
    int silent;                          /* 1=设备自检走 silent.get(云端过滤提醒任务);
                                            0=APP 确认后拉包走 upgrade.get */
    tuya_iot_ota_upgrade_info_t info;   /* 线程填,主线程读 */
    int rt;                              /* check_upgrade 返回码 */
    OS_SEM sem;                          /* 通知主线程:查询结束 */
    volatile int done;                   /* 线程置 1 表示已填好结果 */
} ota_check_ctx_t;

static void tuya_ota_check_task(void *arg)
{
    ota_check_ctx_t *c = (ota_check_ctx_t *)arg;
    /* silent=1:开机自检,云端只下发静默升级任务(对齐 TuyaOpen
     * auto_upgrade_info_get);silent=0:APP 确认后的拉包(upgrade.get)。*/
    c->rt = c->silent ? tuya_iot_ota_check_upgrade_silent(c->client, &c->info)
                      : tuya_iot_ota_check_upgrade(c->client, c->channel, &c->info);
    c->done = 1;
    os_sem_post(&c->sem);
    /* 线程到此结束;info 的释放由主线程负责(主线程拿走结果后 free) */
}

/* 单次下载块大小,与 http_update.c 的 get_update_data 保持一致。*/
#define TUYA_OTA_PER_RECV_SIZE  (8 * 1024)

static int __tuya_ota_http_cb(void *ctx, void *buf, unsigned int size, void *priv, httpin_status status)
{
    return 0;
}

/* 下载+烧写+摘要校验+进度上报(2026-09-29:照搬 apps/common/update/http_update.c 的
 * get_update_data 下载循环自持到本文件,read->write 之间逐块喂给涂鸦摘要
 * 校验器;全部落盘后、net_fclose 烧 boot info 之前 finish 比对云端摘要)。
 *
 * 尺寸比对(客户方案 §8):HTTP Content-Length 必须与云端升级详情的 file_size
 * 完全相等,不等直接失败(error=-4,一块不写、不烧 boot info);云端没给
 * file_size(异常)则跳过比对仅告警,摘要校验仍兜底。
 *
 * 进度上报(MQTT 协议 16,上游无此 API、按 TuyaOpen 协议补齐):每块写盘后算
 * 百分比,较上次超过 5% 才报(TuyaOpen 官方节奏);最后一块到 100% 自然上报,
 * 成功路径 do_upgrade 再强制补一次 100(幂等)。进度是体验数据:发布失败只打
 * 日志,绝不中断下载。
 *
 * verify 的生命周期由本函数全权管理:任何退出路径上 ctx 都已被 finish/abort
 * 释放(NULL 安全),调用方移交后不得再碰。
 *
 * 返回:0 = 烧写完成且摘要通过(net_fclose 内部已烧 boot info 并安排 2s 后
 *       system_reset);非 0 = 失败,其中 -3 = 摘要不匹配(固件损坏/被篡改,
 *       boot info 未烧、不重启,设备继续跑当前备份区,下次开机重试);
 *       -4 = 尺寸与云端 file_size 不符。*/
static int tuya_ota_download_and_flash(const char *url,
                                       long expect_size,
                                       tuya_iot_ota_verify_ctx_t *verify,
                                       const char *sw_ver,
                                       iot_client_t *client, int channel)
{
    int error = 0;
    int ret = 0;
    int offset = 0;
    int remain = 0;
    void *update_fd = NULL;
    u8 *buffer = NULL;
    int data_offset = 0;
    int total_len = 0;
    const struct net_download_ops *ops = &http_ops;

    httpcli_ctx *ctx = (httpcli_ctx *)calloc(1, sizeof(httpcli_ctx));
    if (NULL == ctx) {
        tuya_iot_ota_verify_abort(verify);
        return -1;
    }

    ctx->url = (char *)url;
    ctx->connection = "close";
    ctx->timeout_millsec = 10000;
    ctx->cb = __tuya_ota_http_cb;

    error = ops->init(ctx);
    if (error != HERROR_OK) {
        goto __exit;
    }

    error = -1;

    update_fd = net_fopen(CONFIG_UPGRADE_OTA_FILE_NAME, "w");
    if (!update_fd) {
        goto __exit;
    }

    buffer = (u8 *)malloc(TUYA_OTA_PER_RECV_SIZE);
    if (!buffer) {
        goto __exit;
    }

    total_len = ctx->content_length;
    if (total_len <= 0) {
        goto __exit;
    }

    /* 尺寸比对:HTTP 实际长度 vs 云端 file_size(下载开始前就能判,提前失败省流量)。
     * net_fclose(fd, error!=0) 不烧 boot info,提前 goto 无副作用。*/
    if (expect_size > 0) {
        if ((long)total_len != expect_size) {
            printf("[TUYA-OTA] size MISMATCH: http=%ld cloud_file_size=%ld, abort\r\n",
                   (long)total_len, expect_size);
            error = -4;
            goto __exit;
        }
        printf("[TUYA-OTA] size check OK: http=%ld == cloud file_size=%ld\r\n",
               (long)total_len, expect_size);
    } else {
        printf("[TUYA-OTA] warn: cloud file_size absent, size check skipped\r\n");
    }

    int dl_total = total_len;   /* 循环里 total_len 递减到 0,进度分母另存 */
    int last_pct = 0;           /* 上次已上报的百分比(节流基准) */

    while (total_len > 0) {
        if (total_len >= TUYA_OTA_PER_RECV_SIZE) {
            remain = TUYA_OTA_PER_RECV_SIZE;
            total_len -= TUYA_OTA_PER_RECV_SIZE;
        } else {
            remain = total_len;
            total_len = 0;
        }

        do {
            ret = ops->read(ctx, (char *)buffer + offset, remain - offset);
            if (ret < 0) {
                goto __exit;
            }
            offset += ret;
        } while (remain != offset);

        /* 摘要喂入:与写进 flash 的字节严格一致(整块 buffer[0..offset))。
         * 喂入出错按下载失败处理,不再写这一块。*/
        if (verify && tuya_iot_ota_verify_update(verify, buffer, offset) != OPRT_OK) {
            printf("[TUYA-OTA] digest update err, abort download\r\n");
            goto __exit;
        }

        if (data_offset == 0) {
            os_time_dly(500);   /* 首块延时:避免播提示音时刷 flash 卡音(与 get_update_data 一致) */
        }

        ret = net_fwrite(update_fd, buffer, offset, 0);
        if (ret != offset) {
            printf("[TUYA-OTA] upgrade core error : 0x%x\r\n", ret);
            goto __exit;
        }
        data_offset += offset;
        offset = 0;

        /* 进度上报(节流:>5% 或到 100 才发;乘法走 long 防 20MB+ 固件溢出 int)。*/
        if (client) {
            int percent = (int)((long)data_offset * 100 / dl_total);
            if (percent > 100) {
                percent = 100;
            }
            if (percent == 100 || percent - last_pct > 5) {
                int prt = tuya_iot_ota_report_progress(client, channel, percent);
                printf("[TUYA-OTA] progress %d%% (%ld/%ld bytes)%s\r\n",
                       percent, (long)data_offset, (long)dl_total,
                       prt == OPRT_OK ? "" : " [publish fail, ignored]");
                last_pct = percent;
            }
        }
    }

    /* 全部落盘后、烧 boot info 之前,比对云端摘要(不匹配绝不切启动区)。*/
    if (verify) {
        error = tuya_iot_ota_verify_finish(verify);   /* 无论成败,ctx 在内部已释放 */
        verify = NULL;
        if (error != OPRT_OK) {
            printf("[TUYA-OTA] digest MISMATCH (ret=%d), boot info NOT burned\r\n", error);
            error = -3;
            goto __exit;
        }
        printf("[TUYA-OTA] digest verify OK\r\n");
    }

    /* 版本记录写入点(2026-09-29 v2):镜像已全部落盘且摘要校验通过,而
     * net_fclose(net_update.c:256)还没被调用——2s system_reset 定时器在它
     * 内部才拉起,此处写 USER 区无任何复位在倒计时。语义:记录 = "已在本机
     * 烧录完成的任务版本"。写失败不阻断升级(告警后退回宏方案,新固件开机
     * 自报宏)。旧写点:下载前(下载中断电会留下"报新未升新"谎账)/VM 方案
     * 写在 net_fclose 后的 2s 窗内撞复位——均已废弃,沿革见文件尾"版本号管理"。*/
    tuya_save_upgraded_sw_ver(sw_ver);

    error = 0;

__exit:
    if (buffer) {
        free(buffer);
    }
    if (update_fd) {
        /* 非零 error(含 -3 摘要不匹配)→ is_socket_err 语义:net_fclose 不烧
         * boot info、不安排重启,设备继续跑当前备份区。*/
        net_fclose(update_fd, error ? 1 : 0);
    }
    ops->close(ctx);
    free(ctx);
    tuya_iot_ota_verify_abort(verify);   /* 中途失败/未走 finish 路径的兜底释放(NULL 安全) */
    return error;
}

/* 实际执行下载+烧写的核心(查询已成功,这一步值得花时间)。返回 1=升级成功待重启。 */
static int tuya_ota_do_upgrade(iot_client_t *client, tuya_iot_ota_upgrade_info_t *info)
{
    /* 状态上报通道用云端返回的 info->channel(APP 确认升级可能非 0),info 释放
     * 后仍要用,先存局部。云端没带时为 0(check 前 ctx 已清零)。
     * fsize 同理:尺寸比对要传给下载循环,而 info 在下载前已 free。*/
    int chan = info->channel;
    long fsize = info->file_size;
    printf("[TUYA-OTA] found upgrade: ch=%d ver=%s size=%ld url=%s md5=%s\r\n",
           chan,
           info->version ? info->version : "(null)",
           info->file_size,
           info->url,
           info->md5 ? info->md5 : "(null)");

    /* 上报"升级中"(枚举值=2,云端语义 upgrading) */
    int rt = tuya_iot_ota_report_status(client, chan, OTA_STATUS_UPGRADING);
    if (rt != 0) {
        printf("[TUYA-OTA] warn: report UPGRADING err=%d (continue anyway)\r\n", rt);
    }

    /* 进度 0%(任务已接受,App 立即看到进度条起点;节奏表:接受任务→0) */
    tuya_iot_ota_report_progress(client, chan, 0);
    printf("[TUYA-OTA] progress 0%% (task accepted)\r\n");

    /* 播报"正在升级,请勿断电"。同步播完再下载,避免下载太快打断提示音。
     *   下载循环首块数据到达后还有 os_time_dly(500) 的额外窗口。*/
    printf("[TUYA-OTA] playing upgrade prompt, do NOT power off...\r\n");
    app_music_play_ota_prompt(0);
    os_time_dly(200);   /* ~2s:让提示音播完再开始下载 */

    /* url 拷一份:info 由 client 的 PAL 分配,本函数内会
     *   tuya_iot_ota_upgrade_info_free 释放掉,之后不能再碰 info->*。
     *   url 给下载循环用,free 后即无依赖。
     *   ver_copy 传给下载烧写函数:烧录落盘+摘要通过后(net_fclose 前)由它
     *   写 USER 区版本记录,见 tuya_ota_download_and_flash 内注释。*/
    char *url_copy     = info->url     ? strdup(info->url)     : NULL;
    char *ver_copy     = info->version ? strdup(info->version) : NULL;
    if (!url_copy) {
        printf("[TUYA-OTA] strdup url OOM\r\n");
        free(ver_copy);
        tuya_iot_ota_report_status(client, chan, OTA_STATUS_ERROR);
        return 0;
    }

    /* 摘要校验器:必须在 info 释放前建好(ctx 内部拷贝期望摘要/HMAC 密钥)。
     * hmac 优先、md5 回退;云端两者都没下发 → NOT_SUPPORTED,跳过校验照烧
     * (涂鸦正常都会带 md5,此分支只是兜底);其它错误(格式非法/内存不足)
     * 按上游文档必须中止——校验器建不起来就不装这个固件。*/
    tuya_iot_ota_verify_ctx_t *verify = NULL;
    int vrc = tuya_iot_ota_verify_init(client, info, &verify);
    if (vrc != OPRT_OK && vrc != OPRT_NOT_SUPPORTED) {
        printf("[TUYA-OTA] verify_init err=%d, abort (never flash unverified)\r\n", vrc);
        free(url_copy);
        free(ver_copy);
        tuya_iot_ota_report_status(client, chan, OTA_STATUS_ERROR);
        app_music_play_ota_prompt(2);
        return 0;
    }
    if (vrc == OPRT_NOT_SUPPORTED) {
        printf("[TUYA-OTA] cloud sent no digest, download WITHOUT verify\r\n");
    } else {
        printf("[TUYA-OTA] digest verify armed (%s)\r\n",
               (info->hmac && info->hmac[0]) ? "hmac-sha256" : "md5");
    }

    tuya_iot_ota_upgrade_info_free(client, info);   /* 尽早释放 info(ctx 已拷走所需字段) */

    printf("[TUYA-OTA] downloading & flashing...\r\n");
    int dl_ret = tuya_ota_download_and_flash(url_copy, fsize, verify, ver_copy, client, chan);
    verify = NULL;   /* 生命周期已移交 download_and_flash,防御性清空 */
    free(url_copy);

    if (dl_ret == 0) {
        /* 成功:下载循环内部 net_fclose 已烧 boot info 并安排 2s 后 system_reset。
         * 抢在这 2s 窗口内先报进度 100(强制,循环里可能已报过,幂等)再上报
         * COMPLETE(枚举值=3,云端语义 complete;节奏:先 100 后状态 3);
         * 此路径不碰任何持久介质(避免和 reset 撞车)。*/
        free(ver_copy);
        tuya_iot_ota_report_progress(client, chan, 100);
        printf("[TUYA-OTA] progress 100%% (flash done), report COMPLETE & wait reboot...\r\n");
        int rc = tuya_iot_ota_report_status(client, chan, OTA_STATUS_COMPLETE);
        if (rc != OPRT_OK) {
            printf("[TUYA-OTA] warn: report COMPLETE err=%d\r\n", rc);
        }
        app_music_play_ota_prompt(1);
        os_time_dly(150);   /* 给提示音/上报一点时间,再让 net_fclose 的 reset 生效 */
        return 1;
    }

    /* 失败:USER 区版本记录不动(v2 写点在烧录成功之后,失败时记录里是上一次
     * 成功升级的旧值,恰是设备真实状态)。上报 ERROR(枚举值=4,云端语义
     * error;旧代码报 3 会被云端当"成功"错误闭合任务,2026-09-29 已修正)
     * + 播报,返回 0 让调用方继续连 AI(下轮开机再重试)。*/
    free(ver_copy);
    printf("[TUYA-OTA] download/flash FAILED ret=%d, report ERROR & continue\r\n", dl_ret);
    int re = tuya_iot_ota_report_status(client, chan, OTA_STATUS_ERROR);
    if (re != OPRT_OK) {
        printf("[TUYA-OTA] warn: report ERROR err=%d\r\n", re);
    }
    app_music_play_ota_prompt(2);
    return 0;
}

static int tuya_ota_check_and_upgrade_impl(iot_client_t *client, int channel, int silent)
{
    if (client == NULL) {
        printf("[TUYA-OTA] client NULL, skip\r\n");
        return 0;
    }
    printf("[TUYA-OTA] check_upgrade start (channel=%d, %s)\r\n", channel,
           silent ? "silent.get" : "upgrade.get");

    /* 把 check_upgrade 放独立线程,主线程限时等待——防止 ATOP HTTPS 握手失败时
     * 卡满 SDK 内部 5s 超时,拖慢开机("说了几次你好才有反应"的根因之一)。
     * (APP 确认升级路径由 worker 线程进入,同样受益,不会被查询卡死。)*/
    ota_check_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.client = client;
    ctx.channel = channel;
    ctx.silent = silent;
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
        printf("[TUYA-OTA] no upgrade (current=%s)\r\n", tuya_get_effective_sw_ver());
        tuya_iot_ota_upgrade_info_free(client, &ctx.info);
        return 0;
    }

    /* 有升级:进入下载烧写(这一步值得花时间,主线程继续等)。*/
    return tuya_ota_do_upgrade(client, &ctx.info);
}

/* APP 确认升级入口(protocol 15 之后):upgrade.get 拉当前待升级任务。*/
int tuya_ota_check_and_upgrade_channel(iot_client_t *client, int channel)
{
    return tuya_ota_check_and_upgrade_impl(client, channel, 0);
}

/* 开机路径入口:主固件通道 0,走 silent.get 自检——云端只返回静默升级任务,
 * 平台"APP提醒升级"的任务开机不会被拉下来(等用户在 App 点确认再走
 * _channel 入口)。2026-09-29"提醒升级变强制升级"事故的修复点。*/
int tuya_ota_check_and_upgrade(iot_client_t *client)
{
    return tuya_ota_check_and_upgrade_impl(client, 0, 1);
}

/* ===== 版本号管理(手动宏 + USER 区记录双轨,2026-09-29)=====
 * 上报云端的"生效版本" = USER 区版本记录(tuya_ver_region_load,上次 OTA 的
 * 云端任务版本)> 源码宏 TUYA_FIRMWARE_VERSION。
 * 发版纪律不变:出包前宏仍要改成平台版本号;但即使忘了也有记录兜底——升级后
 * 上报记录值,任务照常闭合,不再死循环;开机日志 base=宏/effective=记录 不一致,
 * 一眼暴露旧宏包。
 * 方案沿革(2026-09-29 复核定稿):v0 VM 方案死于"重启读回 CRC 损坏"——当日
 * 复核发现设备 VM 区本就有慢性坏记录(同一 crc 错值跨开机复现),且写入点在
 * net_fclose 之后的 2s 复位窗内,归因"VM 落盘不稳"证据不足;v1 USER 区方案
 * 写点在下载前,下载中断电会留下"报新未升新"谎账;v2(现行)写点=烧录数据
 * 落盘+摘要校验通过之后、net_fclose 之前(复位定时器在 net_fclose 内部才拉起,
 * net_update.c:256),语义干净且窗口安全。USER 区链路 9/17 断电验证收官。*/
const char *tuya_get_effective_sw_ver(void)
{
    static char cached[16];
    if (cached[0] == 0) {
        /* 首次调用读记录;读不到(无记录/区没准备好)不缓存失败,下次调用再试 */
        char rec[16];
        if (tuya_ver_region_load(rec, sizeof(rec)) == 0 && rec[0]) {
            strncpy(cached, rec, sizeof(cached) - 1);
            cached[sizeof(cached) - 1] = 0;
        }
    }
    return cached[0] ? cached : TUYA_FIRMWARE_VERSION;
}

void tuya_save_upgraded_sw_ver(const char *ver)
{
    if (ver == NULL || ver[0] == 0) {
        return;   /* 云端没给版本号:不写记录,行为退回纯宏方案 */
    }
    if (tuya_ver_region_save(ver) == 0) {
        printf("[TUYA-OTA] sw_ver '%s' persisted (next boot reports it even if macro stale)\r\n",
               ver);
    } else {
        printf("[TUYA-OTA] warn: sw_ver persist fail, fallback to macro '%s'\r\n",
               TUYA_FIRMWARE_VERSION);
    }
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

int tuya_ota_check_and_upgrade_channel(iot_client_t *client, int channel)
{
    (void)client;
    (void)channel;
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

#endif /* TUYA_OTA_ENABLE */
