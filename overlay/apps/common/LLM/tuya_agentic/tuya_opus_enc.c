/* 涂鸦上行 opus 编码封装 —— 杰理闭源 opus 编码库版(cpu/wl82/liba/lib_opus_enc.a +
 * lib_opus_stenc.a,经 SDK audio_server 驱动,链接清单本就含这两库)。
 *
 * 架构(2026-09-20 替换本地 libopus 移植版,起因:声学高压测试中本地定点库把
 * CPU 推到 95%):不再直链 libopus_tuya.a/topus_,改开一条 audio_server 的
 * "virtual" 源 opus 编码通道——
 *   tuya_uplink_send_frame(demo 线程)
 *     → 注入 1280B PCM 进本文件的就绪 cbuf + post 信号量
 *     → audio_server 编码线程 read_input 拉走 PCM,杰理库编码
 *     → 编出的 opus 包经 vfs fwrite 写回输出 cbuf + post 信号量
 *     → frame() 等信号量,取走包返回(80B CBR 预期)
 * 编码发生在 audio_server 任务线程,不占 demo 任务;mic PCM 管线(AEC/VAD/能量门/
 * KWS/barge-in)与 tuya_agentic_demo.c 零改动——demo 仍是 mic cbuf 的唯一消费者,
 * 拿到 PCM 后交给这里换"发出去的字节"。
 *
 * 云端契约(同旧版):TCP 协议 opus=111 且上行必带帧参数(tai 协议层从帧长推导:
 * 80B/帧→40ms/16000bps)。format_mode=0(百度无头=裸 CBR 包)+bitrate=16000+
 * frame_ms=40 ⇒ 恰 80B/包(公式 bitrate*frame_ms/8000,twetalk 180B=24k*60ms
 * 定长读帧已验证)。若实测包长≠80,frame() 打日志核对,勿盲发。
 *
 * ★ 回退路径:注释 app_config.h 的 TUYA_UPLINK_OPUS_ENABLE 即回 PCM 上行;
 *   本文件整体空编译,固件不留杰理编码通道。初始化失败也会话内自动回退 PCM。
 * ★ 本文件【不得】include newlib <stdio.h>:audio_server.h 经 fs/fs.h 引入 SDK
 *   的 FILE,与 agentic-kit 侧 newlib FILE 冲突(同 tuya_agentic_demo.c 的前向
 *   声明规避)。printf 用 SDK 头自带声明,只用基本类型做对外 API。*/
#include "app_config.h"            /* 拿 TUYA_UPLINK_OPUS_ENABLE:开关关闭时本文件整体空编译 */

#ifdef TUYA_UPLINK_OPUS_ENABLE

#include "system/includes.h"       /* printf 声明等 */
#include "os/os_api.h"             /* os_sem_create/pend/post */
#include "server/server_core.h"    /* server_open/request/close,事件注册 */
#include "server/audio_server.h"   /* union audio_req/audio_vfs_ops,经 fs/fs.h 带 SDK FILE */
#include "generic/circular_buf.h"  /* cbuffer_t */
#include "tuya_opus_enc.h"

/* ---- 资源(单例,init 成功后常驻,跨会话/重连复用) ---- */
static struct server *s_enc_srv;              /* audio_server "enc" 实例(与 mic pcm 通道各自独立) */
static cbuffer_t      s_inj_cbuf;             /* 待编码 PCM 就绪队列(demo 线程写,编码线程读) */
static cbuffer_t      s_out_cbuf;             /* 编码出的 opus 包(编码线程写,demo 线程读) */
static OS_SEM         s_inj_sem;              /* PCM 就绪信号量 */
static OS_SEM         s_out_sem;              /* opus 包就绪信号量 */
static u8  s_inj_buf[TUYA_OPUS_PCM_BYTES * 3] __attribute__((aligned(4)));  /* 3 帧余量 */
static u8  s_out_buf[TUYA_OPUS_PKT_MAX * 4];  /* 4 包余量(同步协议下常态 ≤1 包) */
static volatile u8 s_closing;                 /* deinit 中:read_input 返回 -1 收尾 */
static volatile u8 s_dead;                    /* 通道异常(END/ERR):frame() 拒绝服务 */
static u8  s_res_ok;                          /* sem/cbuf 只建一次(重试 init 不重建) */
/* 诊断:前 10 包打长度核对 CBR;此后偶发非 80B 打点(限频)。 */
static u8  s_pkt_logged;
static u32 s_pkt_bad_cnt;

#define OPUS_INJ_PEND_SLICE_MS  20    /* read_input 等数据切片:post 即醒,切片只为查 closing */
#define OPUS_OUT_WAIT_MS        120   /* frame() 等包上限(3 帧时间,正常 <5ms) */

/* ---- 编码线程侧:virtual 源数据入口(audio_server 编码任务上下文) ----
 * 返回 -1 = 输入结束 → 服务回调 AUDIO_SERVER_EVENT_END(仅 deinit 时走)。
 * 空数据时按 OPUS_INJ_PEND_SLICE_MS 切片等待:通道常驻但空闲零编码、零忙转。 */
static int tuya_opus_read_input(u8 *buf, u32 len)
{
    if (len != TUYA_OPUS_PCM_BYTES) {
        /* frame_size=1280 已约定 read_input 的 len;不符=协议层理解偏差,失败可见 */
        printf("[OPUS-ENC] read_input len=%u != %u, refuse\r\n",
               (unsigned)len, (unsigned)TUYA_OPUS_PCM_BYTES);
        return -1;
    }
    for (;;) {
        if (cbuf_get_data_size(&s_inj_cbuf) >= len) {
            cbuf_read(&s_inj_cbuf, buf, len);
            return (int)len;
        }
        if (s_closing || s_dead) {
            return -1;
        }
        os_sem_pend(&s_inj_sem, OPUS_INJ_PEND_SLICE_MS);
    }
}

/* ---- 编码线程侧:编码输出(audio_server 编码任务上下文,禁长阻塞) ---- */
static int tuya_opus_vfs_fwrite(void *file, void *data, u32 len)
{
    (void)file;
    if (len > sizeof(s_out_buf)) {     /* 配置异常保护:丢大包并计数,不炸通道 */
        s_pkt_bad_cnt++;
        return (int)len;
    }
    if (cbuf_write(&s_out_cbuf, data, len) != len) {
        /* 同步协议下常态只压 1 包;堆积=编码线程异常超前,丢旧换新保时序推进 */
        cbuf_clear(&s_out_cbuf);
        cbuf_write(&s_out_cbuf, data, len);
        s_pkt_bad_cnt++;
    }
    os_sem_post(&s_out_sem);
    return (int)len;
}

static const struct audio_vfs_ops tuya_opus_vfs_ops = {
    .fwrite = tuya_opus_vfs_fwrite,
    .fopen  = 0,
    .fread  = 0,
    .fseek  = 0,
    .ftell  = 0,
    .flen   = 0,
    .fclose = 0,
};

static void tuya_opus_srv_event(void *priv, int argc, int *argv)
{
    (void)priv; (void)argc;
    switch (argv[0]) {
    case AUDIO_SERVER_EVENT_ERR:
        printf("[OPUS-ENC] audio_server ERR\r\n");
        s_dead = 1;                    /* 后续 frame() 拒绝;init 不会重入,demo 会话回退日志可见 */
        break;
    case AUDIO_SERVER_EVENT_END:
        if (!s_closing) {              /* deinit 主动收尾的 END 是预期,不算异常 */
            printf("[OPUS-ENC] unexpected END\r\n");
            s_dead = 1;
        }
        break;
    default:
        break;
    }
}

int tuya_opus_enc_init(void)
{
    if (s_enc_srv) {
        return s_dead ? -1 : 0;
    }
    if (!s_res_ok) {
        if (os_sem_create(&s_inj_sem, 0) != OS_NO_ERR) {
            return -1;
        }
        if (os_sem_create(&s_out_sem, 0) != OS_NO_ERR) {
            return -1;
        }
        cbuf_init(&s_inj_cbuf, s_inj_buf, sizeof(s_inj_buf));
        cbuf_init(&s_out_cbuf, s_out_buf, sizeof(s_out_buf));
        s_res_ok = 1;
    }
    s_enc_srv = server_open("audio_server", "enc");
    if (!s_enc_srv) {
        printf("[OPUS-ENC] server_open fail\r\n");
        return -1;
    }
    server_register_event_handler_to_task(s_enc_srv, NULL,
                                          tuya_opus_srv_event, "app_core");

    union audio_req req = {0};
    req.enc.cmd            = AUDIO_ENC_OPEN;
    req.enc.channel        = 1;
    req.enc.volume         = 100;
    req.enc.sample_rate    = 16000;
    req.enc.format         = "opus";
    req.enc.format_mode    = 0;         /* 百度无头=裸 CBR 包,涂鸦要的就是无封装 opus */
    req.enc.frame_ms       = 40;        /* 640 采样/帧,与 mic 帧等长 */
    req.enc.bitrate        = 16000;     /* 16kbps × 40ms = 80B/包 */
    req.enc.complexity     = 0;         /* 最快档,语音够用 */
    req.enc.frame_size     = TUYA_OPUS_PCM_BYTES;  /* virtual 源下即 read_input 的 len */
    req.enc.sample_source  = "virtual";
    req.enc.read_input     = tuya_opus_read_input;
    req.enc.vfs_ops        = &tuya_opus_vfs_ops;
    req.enc.file           = (FILE *)&s_out_cbuf;
    req.enc.vir_data_wait  = 1;         /* 等数据齐再编码,不丢帧 */
    req.enc.output_buf_len = TUYA_OPUS_PCM_BYTES * 4;

    if (server_request(s_enc_srv, AUDIO_REQ_ENC, &req) != 0) {
        printf("[OPUS-ENC] AUDIO_ENC_OPEN fail -> uplink fallback PCM\r\n");
        server_close(s_enc_srv);
        s_enc_srv = NULL;
        return -1;
    }
    s_pkt_logged = 0;
    s_pkt_bad_cnt = 0;
    printf("[OPUS-ENC] inited (JL closed lib via audio_server, 16k/mono/CBR16k/40ms)\r\n");
    return 0;
}

int tuya_opus_enc_frame(const short *pcm, unsigned char *out, int out_max)
{
    if (!s_enc_srv || s_dead || !pcm || !out || out_max < TUYA_OPUS_PKT_MAX) {
        return -1;
    }
    /* 注入一帧 PCM(对齐无要求:走 memcpy,无 short* 直入编码器的奇地址崩溃面) */
    if (cbuf_get_data_size(&s_inj_cbuf) >= TUYA_OPUS_PCM_BYTES) {
        /* 同步协议下注入队列应已空:残留=编码线程停滞,丢旧帧保推进(限频打点) */
        cbuf_clear(&s_inj_cbuf);
        if ((s_pkt_bad_cnt & 31u) == 0) {
            printf("[OPUS-ENC] inject backlog, stale pcm dropped (%u)\r\n", s_pkt_bad_cnt);
        }
        s_pkt_bad_cnt++;
    }
    cbuf_write(&s_inj_cbuf, (void *)pcm, TUYA_OPUS_PCM_BYTES);
    os_sem_post(&s_inj_sem);

    /* 等编码包(正常 <5ms:一次线程切换 + 杰理库编码) */
    if (os_sem_pend(&s_out_sem, OPUS_OUT_WAIT_MS) != 0) {
        printf("[OPUS-ENC] encode timeout (%ums)\r\n", (unsigned)OPUS_OUT_WAIT_MS);
        return -1;                      /* 丢本帧保链路:send 侧按 TAI_OK 处理 */
    }
    int n = cbuf_get_data_size(&s_out_cbuf);
    if (n > out_max) {
        n = out_max;                    /* 一并取走本帧全部产出(80B 预期) */
    }
    n = cbuf_read(&s_out_cbuf, out, n);
    if (s_pkt_logged < 10) {
        printf("[OPUS-ENC] pkt #%d len=%d%s\r\n", s_pkt_logged + 1, n,
               n == 80 ? "" : "  (expect 80!)");
        s_pkt_logged++;
    } else if (n != 80 && (s_pkt_bad_cnt & 31u) == 0) {
        printf("[OPUS-ENC] pkt len=%d != 80 (cnt=%u)\r\n", n, s_pkt_bad_cnt);
        s_pkt_bad_cnt++;
    }
    return n;
}

void tuya_opus_enc_deinit(void)
{
    if (!s_enc_srv) {
        return;
    }
    s_closing = 1;
    os_sem_post(&s_inj_sem);            /* 唤醒 read_input → 返回 -1 → END 正常收尾 */
    union audio_req req = {0};
    req.enc.cmd = AUDIO_ENC_CLOSE;
    server_request(s_enc_srv, AUDIO_REQ_ENC, &req);
    server_close(s_enc_srv);
    s_enc_srv = NULL;
    s_closing = 0;
    s_dead = 0;
}

int tuya_opus_enc_ok(void)
{
    return (s_enc_srv && !s_dead) ? 1 : 0;
}

#endif /* TUYA_UPLINK_OPUS_ENABLE */
