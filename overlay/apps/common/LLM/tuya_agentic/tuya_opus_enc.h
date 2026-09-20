#ifndef TUYA_OPUS_ENC_H
#define TUYA_OPUS_ENC_H

/* 涂鸦上行 opus 编码封装(杰理闭源 lib_opus_enc/stenc.a,经 audio_server virtual
 * 源通道驱动,实现见 tuya_opus_enc.c)。
 * 参数对齐 agentic-kit audio_chat_demo(涂鸦云端验证过的组合):
 *   16kHz / mono / format_mode=0(裸 CBR) / 16kbps / 40ms 帧 = 640 采样。
 * CBR 16kbps × 40ms ⇒ 每包恰 80 字节(frame() 实际返回值为准,不符有日志)。*/

#define TUYA_OPUS_PKT_MAX   128     /* 单包上限:80B 预期,留余量 */
#define TUYA_OPUS_SAMPLES   640     /* 16k × 40ms */
#define TUYA_OPUS_PCM_BYTES (TUYA_OPUS_SAMPLES * 2)   /* 1280B,恰与 mic 帧等长 */

int  tuya_opus_enc_init(void);      /* 0=ok;编码器常驻堆内存(单例),失败返回 -1 */
int  tuya_opus_enc_frame(const short *pcm, unsigned char *out, int out_max);  /* >0=包长 */
void tuya_opus_enc_deinit(void);
int  tuya_opus_enc_ok(void);        /* 编码器是否可用(初始化成功过) */

#endif
