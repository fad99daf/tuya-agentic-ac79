/* 涂鸦上行 opus 编码封装 —— libopus 1.4 定点(见 libopus/)的最小包装。
 * mic 管线保持 PCM 不动(audio_input.c 不改),编码发生在 demo 上行发送路径:
 *   _device_get_voice_data(1280B PCM,含 AEC/VAD 后)→ 本封装 → 80B opus 包 → TAI。
 * 这样 VAD/能量门/barge-in/onset 补发仍工作在 PCM 上,只换"发出去的字节"。
 * ★ 工具链风险(记录在案):clang 4.0.1 + LTO 曾对本地 libopus 浮点【解码】
 *   产生 axi_wr_inv 崩溃(2026-08-28 回退)。本封装走定点编码;若上电/说话
 *   再现崩溃,注释 app_config.h 的 TUYA_UPLINK_OPUS_ENABLE 即回 PCM 上行。*/
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "app_config.h"            /* 拿 TUYA_UPLINK_OPUS_ENABLE:开关关闭时本文件整体空编译 */

#ifdef TUYA_UPLINK_OPUS_ENABLE

#include "tuya_opus_enc.h"

/* 符号重命名映射:libopus 以预编译库 libopus/libopus_tuya.a 提供(见其
 * build_tuya_libopus.sh),全部全局符号统一加 topus_ 前缀——杰理闭源
 * lib_opus_enc/stenc/dec.a 也是 libopus 改的,同名符号直编混链必冲突
 * (2026-08-28 实测 multiple definition 141/74/137 个)。
 * ★ 必须放在 #include "opus.h" 【之前】:宏在头文件解析时生效,把 opus.h 里的
 *   函数原型一并改成 topus_ 名(否则原型是原名、调用是前缀名→隐式声明)。 */
#define opus_encoder_create    topus_opus_encoder_create
#define opus_encoder_destroy   topus_opus_encoder_destroy
#define opus_encoder_ctl       topus_opus_encoder_ctl
#define opus_encode            topus_opus_encode
#define opus_strerror          topus_opus_strerror

#include "opus.h"

static OpusEncoder *s_enc;

int tuya_opus_enc_init(void)
{
    int err = 0;
    if (s_enc) {
        return 0;
    }
    s_enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (!s_enc || err != OPUS_OK) {
        printf("[OPUS-ENC] create fail err=%d (%s)\r\n", err, opus_strerror(err));
        s_enc = NULL;
        return -1;
    }
    opus_encoder_ctl(s_enc, OPUS_SET_VBR(0));            /* CBR:涂鸦按定长分帧 */
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(16000));    /* 16kbps */
    opus_encoder_ctl(s_enc, OPUS_SET_DTX(0));            /* 关 DTX:不发静音包 */
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));     /* 最快,语音质量够用 */
    opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(0));     /* 无 FEC,网络层已可靠(TCP) */
    opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(0));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    printf("[OPUS-ENC] inited (libopus 1.4 fixed-point, 16k/mono/CBR16k/40ms)\r\n");
    return 0;
}

int tuya_opus_enc_frame(const short *pcm, unsigned char *out, int out_max)
{
    if (!s_enc || !pcm || !out || out_max < TUYA_OPUS_PKT_MAX) {
        return -1;
    }
    /* pi32v2 上 16bit 访问奇地址 = 硬件 misalign_err 崩溃(2026-08-31 实测,根因与修复见
     * tuya_agentic_demo.c abuf 的 aligned(4) 注释)。调用方缓冲若漏了对齐,这里丢帧+日志,
     * 绝不把奇指针送进 libopus——丢 40ms 一帧,好过硬重启。*/
    if ((unsigned long)(uintptr_t)pcm & 1ul) {
        printf("[OPUS-ENC] misaligned pcm=0x%08x, frame dropped\r\n",
               (unsigned)(uintptr_t)pcm);
        return -1;
    }
    /* OPUS_RESET_STATE 每帧调用会让编码器失去跨帧预测,音质劣化——不重置,
     * 长会话连续编码即可;turn 间断不影响(编码器对静音段有自适应)。*/
    int n = opus_encode(s_enc, pcm, TUYA_OPUS_SAMPLES, out, out_max);
    if (n < 0) {
        printf("[OPUS-ENC] encode fail %d (%s)\r\n", n, opus_strerror(n));
        return -1;
    }
    return n;   /* CBR 下应为 80 */
}

void tuya_opus_enc_deinit(void)
{
    if (s_enc) {
        opus_encoder_destroy(s_enc);
        s_enc = NULL;
    }
}

int tuya_opus_enc_ok(void)
{
    return s_enc != NULL;
}

#endif /* TUYA_UPLINK_OPUS_ENABLE */
