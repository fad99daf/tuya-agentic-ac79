/* ============================================================================
 * wekws_nn.c — v9d FSMN int16 流式内核(逐行镜像 PC 仿真器 fsmn_quant.
 * Sim16.step, 定案配置 exam 校准 pad=0.05 pct=99.9, 18/18 worst=0.046):
 *   CMVN(400d 图内, 浮点)+量化 x -> in_linear1(400->64, 无 relu, q_bias64)
 *   -> b0=relu(in_linear2) -> 4x[ fsmn_i0(96->64, 无偏置无 relu) ->
 *   conv_left(10 拍)+conv_right(2 拍)+残差(cat[9]) 三臂各自 req -> y ->
 *   relu(fsmn_i.2) ] -> out_linear1(96->64, 无 relu) -> out_linear2(64->9)
 *   -> 反量化 float logits(softmax 归 CTC, 与 ONNX 图尾 softmax 同数学)。
 * 算术约定(全部与 Sim16 对齐):
 *   - CMVN 量化 = llrint(银行家) = np.round; 仿射 clip [-32768,32767]
 *   - requant = round(acc*q / 2^n) + zp(TFLM 乘子语义, round-half-up);
 *     fused RELU = 下限钳到 zp; 非 relu 张量(h1/l/y/out1/logits)不钳 zp
 *   - conv 两臂各自独立 qm(qml/qmr: l_s*wls/y_s 与 l_s*wrs/y_s), 残差臂
 *     单独 qm(l_s/y_s); 三项各 req_raw 后相加 + zp_y, int16 饱和
 *   - 缓存量化域真零 = l_zp(复位填充 zp, 非零编码)
 *   - GEMV 内积 int32 累加: 最长 400 项 x 2^15 x 2^7 = 2^30 < 2^31, 与
 *     int64 全程累加位级一致; 偏置相加/requant 走 int64
 *   - 状态: 4 层 x 11 帧 x 64 int16 = 5.6KB(最旧在前, 每帧 memmove 滑动)
 * 接口契约不变(wekws_fbank 现产 400d 堆叠帧: 5x80 上下文窗, 30ms 步长):
 *   push() 入队, flush() 批算; fbank 每 640 样本片最多产 2 个堆叠帧,
 *   队列容量 WK_NN_QH 为其数倍余量。
 * ==========================================================================*/
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "wekws_nn.h"
#include "wekws_model_data.h"

/* 拆段计时位(心跳读走; FSMN 单帧量小, 仅保留接口) */
#ifndef WK_HOST_BUILD
#include "asm/WL82.h"
#ifndef BIT
#define BIT(n) (1u << (n))
#endif
#define WK_NN_TICK() (JL_TIMER4->CNT)
#else
#define WK_NN_TICK() (0u)
#endif
static unsigned int s_t_nn_all;

void wekws_nn_timers(unsigned int *stage, unsigned int *pw)
{
    if (stage) *stage = 0;
    if (pw)    *pw    = s_t_nn_all;
    s_t_nn_all = 0;
}

/* ---- 内部 RAM 常驻(WK_RAM_BSS -> ram0 BSS) ----
 * 缓存 5.6KB + 队列 12.8KB + 帧缓冲 1.6KB */
static int16_t s_cache[4][11][64] WK_RAM_BSS;   /* 最旧在 [0], 最新在 [10] */
static float   s_q[WK_NN_QH][400] WK_RAM_BSS;   /* 堆叠帧入队暂存 */
static int     s_qn;               /* 队列内待算帧数 */
static int     s_qdrop;            /* 队满丢弃计数(接线正确应恒 0) */

void wekws_nn_reset(void)
{
    /* 缓存量化域真零 = l_zp(Sim16: np.full(zp)) */
    for (int i = 0; i < 4; i++) {
        for (int f = 0; f < 11; f++) {
            for (int c = 0; c < 64; c++) s_cache[i][f][c] = (int16_t)wk_l_zp[i];
        }
    }
    s_qn = 0;
}

void wekws_nn_push(const float *f400)
{
    if (s_qn >= WK_NN_QH) {
        if (s_qdrop++ == 0) {
            printf("[WEKWS] nn queue overflow (wiring bug: flush per feed!)"
                   "\r\n");
        }
        return;
    }
    memcpy(s_q[s_qn], f400, 400 * sizeof(float));
    s_qn++;
}

/* TFLM 乘子舍入: round(x*q / 2^n) (round-half-up, 同 req_raw16/req_arr16) */
static inline int64_t wk_rnd(int64_t prod, int n)
{
    if (n > 0) {
        int64_t mask = ((int64_t)1 << n) - 1;
        return (prod >> n) + (int64_t)((prod & mask) > (mask >> 1));
    }
    return prod << (-n);
}

/* requant + zp, 仿射 clip [-32768, 32767](非 relu 张量) */
static inline int16_t wk_req16(int64_t acc, int32_t q, int8_t n, int32_t zp)
{
    int64_t v = wk_rnd(acc * (int64_t)q, n) + zp;
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    return (int16_t)v;
}

/* requant + zp + fused RELU(下限钳到 zp; b0..b4) */
static inline int16_t wk_req_relu(int64_t acc, int32_t q, int8_t n, int32_t zp)
{
    int64_t v = wk_rnd(acc * (int64_t)q, n) + zp;
    if (v < (int64_t)zp) v = zp;
    if (v > 32767) v = 32767;
    return (int16_t)v;
}

/* ADD 臂换算: round(x * m), 无偏移无饱和(Sim16 req_raw16) */
static inline int64_t wk_req_raw(int64_t x, int32_t q, int8_t n)
{
    return wk_rnd(x * (int64_t)q, n);
}

static inline int16_t wk_sat16(int64_t v)
{
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (int16_t)v;
}

int wekws_nn_flush(float (*logits)[WK_NCLASS], int max_out)
{
    int      i, h, c, k;
    int      n = s_qn;
    int16_t  xq[400];
    int16_t  hv[64];
    int16_t  bv[96];
    int16_t  lv[64];
    int16_t  yv[64];

    s_qn = 0;
    if (n <= 0) return 0;
    if (n > max_out) {   /* 不可达: 调用方 max_out >= 每次喂入产出上限 */
        s_qdrop += n - max_out;
        n = max_out;
    }
    unsigned int t0 = WK_NN_TICK();

    for (h = 0; h < n; h++) {
        /* 1) CMVN(图内, 浮点)+ int16 量化(与 Sim16.step 逐位一致) */
        for (i = 0; i < 400; i++) {
            double x = ((double)s_q[h][i] - wk_cmvn_mean[i]) * wk_cmvn_istd[i];
            long   q = llrint(x / wk_x_scale) + wk_x_zp;
            if (q < -32768) q = -32768;
            if (q > 32767) q = 32767;
            xq[i] = (int16_t)q;
        }
        /* 2) in_linear1 400->64 (b1 为 q_bias64, x 带 zp 不减) */
        for (c = 0; c < 64; c++) {
            const int8_t *w = wk_w1[c];
            int32_t s = 0;
            for (k = 0; k < 400; k += 4) {
                s += (int32_t)xq[k]     * w[k]
                   + (int32_t)xq[k + 1] * w[k + 1]
                   + (int32_t)xq[k + 2] * w[k + 2]
                   + (int32_t)xq[k + 3] * w[k + 3];
            }
            hv[c] = wk_req16(wk_b1q[c] + s, wk_qm1_q[c], wk_qm1_n[c],
                             wk_h1_zp);
        }
        /* 3) in_linear2 64->96 + relu -> b0 */
        for (c = 0; c < 96; c++) {
            const int8_t *w = wk_w2[c];
            int32_t s = 0;
            for (k = 0; k < 64; k++) s += (int32_t)(hv[k] - wk_h1_zp) * w[k];
            bv[c] = wk_req_relu(wk_b2q[c] + s, wk_qm2_q[c], wk_qm2_n[c],
                                wk_b_zp[0]);
        }
        /* 4) 4 x FSMN 块(三段式: l 全量 -> y 全量(conv 读旧缓存) -> b 全量,
         *    缓存滑动在 conv 用完旧值之后) */
        for (i = 0; i < 4; i++) {
            int32_t zpb = wk_b_zp[i];
            int32_t zpl = wk_l_zp[i];
            int32_t zpy = wk_y_zp[i];
            /* l = b @ W0 (无偏置无 relu) */
            for (c = 0; c < 64; c++) {
                const int8_t *w = wk_w0[i][c];
                int32_t s = 0;
                for (k = 0; k < 96; k++) s += (int32_t)(bv[k] - zpb) * w[k];
                lv[c] = wk_req16(s, wk_qm0_q[i][c], wk_qm0_n[i][c], zpl);
            }
            /* y = cat[9] + convL(10 拍) + convR(2 拍), 三臂各自 req 后相加 */
            for (c = 0; c < 64; c++) {
                int64_t aL = 0, aR;
                for (k = 0; k < 10; k++) {
                    aL += (int32_t)(s_cache[i][k][c] - zpl) * wk_wlq[i][k][c];
                }
                aL = wk_req_raw(aL, wk_qml_q[i][c], wk_qml_n[i][c]);
                aR = (int32_t)(s_cache[i][10][c] - zpl) * wk_wrq[i][0][c]
                   + (int32_t)(lv[c] - zpl)          * wk_wrq[i][1][c];
                aR = wk_req_raw(aR, wk_qmr_q[i][c], wk_qmr_n[i][c]);
                {
                    int64_t a2 = wk_req_raw((int64_t)s_cache[i][9][c] - zpl,
                                            wk_qmres_q[i], wk_qmres_n[i]);
                    yv[c] = wk_sat16(aL + aR + a2 + zpy);
                }
            }
            /* b' = relu(y @ Wl2 + bl2)(读全部 yv; 原地覆盖 bv 安全: 旧 bv
             * 只被上面的 l 段消费, 已完毕) */
            for (c = 0; c < 96; c++) {
                const int8_t *w = wk_wl2[i][c];
                int32_t s = 0;
                for (k = 0; k < 64; k++) s += (int32_t)(yv[k] - zpy) * w[k];
                bv[c] = wk_req_relu(wk_bl2q[i][c] + s, wk_qml2_q[i][c],
                                    wk_qml2_n[i][c], wk_b_zp[i + 1]);
            }
            /* 缓存滑动: 丢最旧 1 帧, 当前 l 落尾(= Sim16 cat[1:]) */
            memmove(s_cache[i], s_cache[i] + 1, 10 * 64 * sizeof(int16_t));
            memcpy(s_cache[i][10], lv, 64 * sizeof(int16_t));
        }
        /* 5) out_linear1 96->64 (无 relu) */
        for (c = 0; c < 64; c++) {
            const int8_t *w = wk_wo1[c];
            int32_t s = 0;
            for (k = 0; k < 96; k++) {
                s += (int32_t)(bv[k] - wk_b_zp[4]) * w[k];
            }
            hv[c] = wk_req16(wk_bo1q[c] + s, wk_qmo1_q[c], wk_qmo1_n[c],
                             wk_out1_zp);
        }
        /* 6) out_linear2 64->9 + 反量化(softmax 归 CTC) */
        for (c = 0; c < WK_NCLASS; c++) {
            const int8_t *w = wk_wo2[c];
            int32_t s = 0;
            for (k = 0; k < 64; k++) {
                s += (int32_t)(hv[k] - wk_out1_zp) * w[k];
            }
            logits[h][c] = (float)((double)(wk_req16(wk_bo2q[c] + s,
                                                     wk_qmo2_q[c],
                                                     wk_qmo2_n[c],
                                                     wk_lg_zp) -
                                             wk_lg_zp) * wk_lg_scale);
        }
    }
    s_t_nn_all += WK_NN_TICK() - t0;
    return n;
}

/* 薄包装: 入队一帧立即批算出 logits(自测/兼容保留; 主链路走 push+flush) */
void wekws_nn_step(const float *f400, float *logits)
{
    float lg[WK_NCLASS];
    wekws_nn_push(f400);
    wekws_nn_flush(&lg, 1);
    memcpy(logits, lg, sizeof(lg));
}
