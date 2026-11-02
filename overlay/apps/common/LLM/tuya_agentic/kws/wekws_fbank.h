#ifndef __WEKWS_FBANK_H__
#define __WEKWS_FBANK_H__
/* wekws_fbank.h — 流式 kaldi fbank (16k, 25ms/10ms, 80 mel) + 上下文堆叠:
 * fbank 语义逐位对齐 torchaudio.compliance.kaldi.fbank(v6 端 40mel PC 端
 * 验证 max|Δ|=6.8e-5; v9d 80mel 烘焙表同管线, emit 门 max|Δ|=5.5e-5)。
 * v9d 部署口径: 80mel 帧 -> [f(t-2)..f(t+2)] 5 帧堆叠(边缘复制, 流式滞后
 * 2 帧) -> 每 3 帧取 1(head 相位 [::3], 已对拍定案) -> 400 维堆叠帧回调。
 * 常量在 wekws_model_data.c。 */
#define WKFB_BINS 80

/* 复位到"零历史"状态(开机/唤醒命中后)。 */
void wekws_fbank_reset(void);

/* 喂 PCM(int16, 任意块大小; 内部按 640 样本分片)。新产出的 400 维堆叠帧
 * 逐帧回调 on_frame(ctx, f400)。 */
void wekws_fbank_feed(const short *pcm, int samples,
                      void (*on_frame)(void *ctx, const float *f400),
                      void *ctx);

#endif /* __WEKWS_FBANK_H__ */
