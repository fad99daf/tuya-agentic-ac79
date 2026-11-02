/* ============================================================================
 * wekws_fbank.c — 流式 kaldi fbank, torchaudio.compliance.kaldi.fbank 的
 * C 镜像(v6 40mel PC 端逐位验证 16 卷 max|Δf40| = 6.8e-5; v9d 80mel 烘焙
 * 表同管线, emit 门逐帧 max|Δf80| = 5.5e-5)。FFT/窗/帧机构不变, 仅 mel
 * 表 40->80(v9d 部署口径)。
 *
 * 语义(与 torchaudio _get_window 严格一致):
 *   帧: snip_edges=True → 仅输出完整帧, 起点 = t*160, 长 400 (25ms@16k)
 *   1) remove_dc_offset: 减帧均值
 *   2) preemph 0.97: x[j] -= 0.97*x[j-1] (j>0, 用去直流后的帧内样本);
 *      j=0 为 replicate pad → x[0] = 0.03*x[0] (torchaudio 与 kaldi 同)
 *   3) povey 加窗 (hann(periodic=false)^0.85, 烘焙常量)
 *   4) 补零至 512, 基-2 FFT, 功率谱 |X(k)|^2, k=0..256
 *   5) mel 80x257 (末列 0), floor = float32 eps, ln
 *   无 dither/无 energy 追加(全局 CMVN 在模型图内, 内核做)。
 *
 * v9d 堆叠+跳帧(部署口径, PC 端 fsmn_ref.py 对拍定案):
 *   - 上下文窗: stacked(t) = [f(t-2)..f(t+2)], 边缘复制(f<0 复制 f0),
 *     流式即 f(t+2) 到达时产出(滞后 2 帧, 10ms 帧距)
 *   - 跳帧: 仅 stacked(t), t%3==0 产出(head 相位 [::3], 30ms 步长)
 *   - 到达 k=2 → stacked_0=[f0,f0,f0,f1,f2]; k=3 → stacked_1=[f0,f0,f1,
 *     f2,f3]; k>=4 → 环形最后 5 帧顺序; 产出条件 (k-2)%3==0
 * ==========================================================================*/
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "wekws_fbank.h"
#include "wekws_model_data.h"

#define WKFB_RING   1280     /* ≥ 400+640: 640 样本分片内不丢样 */
#define WKFB_FRAME  400
#define WKFB_SHIFT  160
#define WKFB_EPS    1.1920928955078125e-07f   /* torch.finfo(float32).eps */

/* 逐帧热缓冲上岸内部 RAM(v6 二刀): s_ring 帧拷 1.6KB/hop + FFT 原地蝶形
 * 跨步触 ~36KB/hop, SDRAM 未缓存取数是板测⑦ fb 2.6-3ms/hop 的大头 */
static float        s_ring[WKFB_RING] WK_RAM_BSS;
static uint32_t     s_w;      /* 绝对写指针(样本数) */
static uint32_t     s_fstart; /* 下一帧绝对起点 */
static float        s_x[WKFB_FRAME] WK_RAM_BSS;
static float        s_fr[512] WK_RAM_BSS, s_fi[512] WK_RAM_BSS;

/* v9d 堆叠+跳帧状态: 最近 5 个 80mel 帧环形(到达序) + 到达计数 */
static float        s_stk[5][WKFB_BINS] WK_RAM_BSS;
static int          s_stk_w;   /* 下一写槽(模 5) */
static uint32_t     s_stk_k;   /* 已到达帧数(下一帧 = k) */

/* FFT 旋转因子与位反转表: 由 emit_fsmn_c.py 烘焙(声明见 wekws_model_data.h) */

void wekws_fbank_reset(void)
{
    s_w = 0;
    s_fstart = 0;
    s_stk_w = 0;
    s_stk_k = 0;
}

/* 堆叠+跳帧: 第 k 个 fbank 帧到达时, 若 (k-2)%3==0 产出 stacked(k-2) */
static void stack_feed(const float *f80,
                       void (*on_frame)(void *, const float *), void *ctx)
{
    uint32_t k = s_stk_k++;
    memcpy(s_stk[s_stk_w], f80, WKFB_BINS * sizeof(float));
    s_stk_w = (s_stk_w + 1) % 5;
    if (k < 2 || (k - 2) % 3 != 0) return;

    float  out[5 * WKFB_BINS];
    const float *p[5];
    int    j, b;
    if (k == 2) {          /* [f0,f0,f0,f1,f2] */
        p[0] = p[1] = p[2] = s_stk[0];
        p[3] = s_stk[1];
        p[4] = s_stk[2];
    } else if (k == 3) {   /* [f0,f0,f1,f2,f3] */
        p[0] = p[1] = s_stk[0];
        p[2] = s_stk[1];
        p[3] = s_stk[2];
        p[4] = s_stk[3];
    } else {               /* k>=4: 环形最后 5 槽 = 帧 k-4..k (旧->新) */
        for (j = 0; j < 5; j++) {
            p[j] = s_stk[(s_stk_w + j) % 5];
        }
    }
    for (j = 0; j < 5; j++) {
        for (b = 0; b < WKFB_BINS; b++) {
            out[j * WKFB_BINS + b] = p[j][b];
        }
    }
    on_frame(ctx, out);
}

static void frame_out(void (*on_frame)(void *, const float *), void *ctx)
{
    float *x = s_x;
    int    i, j, k;
    float  mean = 0.0f;
    float  out[WKFB_BINS];

    for (i = 0; i < WKFB_FRAME; i++) {
        x[i] = s_ring[(s_fstart + i) % WKFB_RING];
    }
    /* 1) 去直流 (均值按 float32 参与减法) */
    {
        double s = 0.0;
        for (i = 0; i < WKFB_FRAME; i++) s += x[i];
        mean = (float)(s / WKFB_FRAME);
    }
    for (i = 0; i < WKFB_FRAME; i++) x[i] -= mean;
    /* 2) preemph: 降序原地 = 引用修改前的 x[j-1] (torchaudio pad-replicate) */
    for (j = WKFB_FRAME - 1; j > 0; j--) {
        x[j] -= 0.97f * x[j - 1];
    }
    x[0] *= 0.03f;
    /* 3) povey 加窗 */
    for (i = 0; i < WKFB_FRAME; i++) x[i] *= wk_win400[i];
    /* 4) 512 点基-2 FFT (实输入, 虚部清零), 迭代蝶形同 PC 镜像 */
    for (i = 0; i < WKFB_FRAME; i++) { s_fr[i] = x[i]; s_fi[i] = 0.0f; }
    for (i = WKFB_FRAME; i < 512; i++) { s_fr[i] = 0.0f; s_fi[i] = 0.0f; }
    for (i = 1; i < 512; i++) {
        j = wk_bitrev[i];
        if (i < j) {
            float t = s_fr[i]; s_fr[i] = s_fr[j]; s_fr[j] = t;
            t = s_fi[i]; s_fi[i] = s_fi[j]; s_fi[j] = t;
        }
    }
    for (int len = 2; len <= 512; len <<= 1) {
        int step = 512 / len;
        int half = len >> 1;
        for (int st = 0; st < 512; st += len) {
            int kk = 0;
            for (int p = st; p < st + half; p++) {
                float wr = wk_fft_twr[kk], wi = wk_fft_twi[kk];
                float qr = s_fr[p + half], qi = s_fi[p + half];
                float tr = wr * qr - wi * qi;
                float ti = wr * qi + wi * qr;
                s_fr[p + half] = s_fr[p] - tr;
                s_fi[p + half] = s_fi[p] - ti;
                s_fr[p] += tr;
                s_fi[p] += ti;
                kk += step;
            }
        }
    }
    /* 5) mel + floor + ln */
    for (int b = 0; b < WKFB_BINS; b++) {
        float e = 0.0f;
        const float *m = wk_mel[b];
        for (k = 0; k < 257; k++) {
            e += m[k] * (s_fr[k] * s_fr[k] + s_fi[k] * s_fi[k]);
        }
        if (e < WKFB_EPS) e = WKFB_EPS;
        out[b] = logf(e);
    }
    stack_feed(out, on_frame, ctx);
}

void wekws_fbank_feed(const short *pcm, int samples,
                      void (*on_frame)(void *, const float *), void *ctx)
{
    if (!pcm || samples <= 0) return;
    while (samples > 0) {
        int n = samples > 640 ? 640 : samples;
        for (int i = 0; i < n; i++) {
            s_ring[s_w % WKFB_RING] = (float)pcm[i];
            s_w++;
        }
        pcm += n;
        samples -= n;
        /* 完整帧即取即算: 环内滞留 ≤ 399+640 < WKFB_RING */
        while (s_fstart + WKFB_FRAME <= s_w) {
            frame_out(on_frame, ctx);
            s_fstart += WKFB_SHIFT;
        }
    }
}
