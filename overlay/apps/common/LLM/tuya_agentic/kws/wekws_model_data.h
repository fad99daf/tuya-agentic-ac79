#ifndef __WEKWS_MODEL_DATA_H__
#define __WEKWS_MODEL_DATA_H__
/* ============================================================================
 * wekws_model_data.h — v9d FSMN "小涂小涂" 唤醒词模型常量(生成物, 见
 * kws_convert/emit_fsmn_c.py)。数值与 PC 仿真器 fsmn_quant.Sim16 位级一致
 * (定案配置: exam 校准 pad=0.05 pct=99.9, 18/18 worst=0.046):
 *  - 激活 int16 非对称 / 权重 int8 逐输出通道对称 / 偏置 int64
 *  - requant = round(acc*q / 2^n) + zp (TFLM 语义); fused RELU 下限钳到 zp
 *  - 9 类输出: <blk>0 小1 涂2 图3 土4 兔5 突6 秃7 <filler>8, ONNX 图尾
 *    自带 softmax -> 内核出 logits 反量化为 float, softmax 归 CTC(同数学)
 *  - CMVN 400d 在图内: 前端产 [f(t-2)..f(t+2)] 5x80 堆叠帧, 内核浮点
 *    CMVN 后量化(引擎蓝图 = Sim16.step)
 * 布局约定: 线性权重 [out][in]; conv_left [tap][ch] / conv_right [tap][ch];
 * conv 两臂各乘各自 qm(qml/qmr), 残差臂 qmres(l 尺度->y 尺度)。
 * fsmn_i0 无偏置; 残差在 64 维 l 域(96 维 b 无跨层直连)。
 * ★ 全部常量 ir_* 开机一次拷入内部 RAM(WK_RAM_BSS -> ram0 BSS, 零初始化
 *   不占镜像); FSMN 每帧 ~90K MAC, 无 SDRAM 级。宿主验证置空。
 * ==========================================================================*/
#include <stdint.h>

#ifdef WK_HOST_BUILD
#define WK_RAM_BSS
#else
#define WK_RAM_BSS __attribute__((section(".sram"), aligned(4)))
#endif

#define WK_NCLASS 9

/* 开机把全部推理常量拷入内部 RAM(幂等, 可重复调)。首次推理前必须调用。 */
void wk_model_cache_init(void);

/* ---- kaldi fbank 常量 (torchaudio.compliance.kaldi 逐位烘焙) ---- */
extern const float  *wk_win400;         /* [400] povey 窗 */
extern const float (*wk_mel)[257];      /* [80][257] mel 滤波器组, 末列恒 0 */
extern const double *wk_cmvn_mean;      /* [400] 图内 CMVN mean */
extern const double *wk_cmvn_istd;      /* [400] */
extern const float  *wk_fft_twr;        /* [256] 512 点 FFT 旋转因子(实部) */
extern const float  *wk_fft_twi;        /* [256] */
extern const uint16_t *wk_bitrev;       /* [512] 位反转置换 */
extern const float  *wk_exp2_tab;       /* [257] 2^(i/256) 快速 exp LUT */

/* ---- 激活量化参数(尺度已折进 qm 的只发 zp) ---- */
extern const double wk_x_scale;         /* x_cmvn (CMVN 后 400d) */
extern const int    wk_x_zp;
extern const int    wk_h1_zp;           /* in_linear1 出(线性, 无 relu) */
extern const int32_t *wk_b_zp;          /* [5] b0..b4 (relu: out_zp=下限) */
extern const int32_t *wk_l_zp;          /* [4] fsmn l 域(缓存初值=zp) */
extern const int32_t *wk_y_zp;          /* [4] fsmn y 域 */
extern const int    wk_out1_zp;         /* out_linear1 出(线性) */
extern const double wk_lg_scale;        /* logits 反量化(float 给 CTC) */
extern const int    wk_lg_zp;

/* ---- 输入段 ---- */
extern const int8_t  (*wk_w1)[400];     /* [64][400] in_linear1 */
extern const int64_t *wk_b1q;           /* [64] q_bias64(x 带 zp 补偿) */
extern const int32_t *wk_qm1_q;         /* [64] */
extern const int8_t  *wk_qm1_n;
extern const int8_t  (*wk_w2)[64];      /* [96][64] in_linear2 */
extern const int64_t *wk_b2q;           /* [96] nozp(输入已减 zp) */
extern const int32_t *wk_qm2_q;
extern const int8_t  *wk_qm2_n;

/* ---- 4 x FSMN 块 ----
 * 每块: l = b @ W0 (96->64, 无偏置无 relu)
 *       cat = [cache(11 帧), l];  convL = sum_k wlq[k]*cat[k] (k=0..9)
 *       convR = wrq[0]*cat[10] + wrq[1]*cat[11]
 *       y = cat[9] + convL + convR  (残差臂 l 尺度, 各臂独立 req)
 *       b = relu(y @ Wl2 + bl2);  cache = cat[1:] */
extern const int8_t  (*wk_w0)[64][96];   /* [4][64][96] */
extern const int32_t (*wk_qm0_q)[64];    /* [4][64] */
extern const int8_t  (*wk_qm0_n)[64];
extern const int8_t  (*wk_wlq)[10][64];  /* [4][10][64] conv_left [tap][ch] */
extern const int8_t  (*wk_wrq)[2][64];   /* [4][2][64]  conv_right [tap][ch] */
extern const int32_t (*wk_qml_q)[64];    /* [4][64] convL 换算(l_s*wls/y_s) */
extern const int8_t  (*wk_qml_n)[64];
extern const int32_t (*wk_qmr_q)[64];    /* [4][64] convR 换算(l_s*wrs/y_s) */
extern const int8_t  (*wk_qmr_n)[64];
extern const int32_t *wk_qmres_q;        /* [4] 残差臂换算(l_s/y_s) */
extern const int8_t  *wk_qmres_n;
extern const int8_t  (*wk_wl2)[96][64];  /* [4][96][64] fsmn_i.2 */
extern const int64_t (*wk_bl2q)[96];     /* [4][96] */
extern const int32_t (*wk_qml2_q)[96];   /* [4][96] */
extern const int8_t  (*wk_qml2_n)[96];

/* ---- 输出段 ---- */
extern const int8_t  (*wk_wo1)[96];      /* [64][96] out_linear1 (无 relu) */
extern const int64_t *wk_bo1q;           /* [64] */
extern const int32_t *wk_qmo1_q;
extern const int8_t  *wk_qmo1_n;
extern const int8_t  (*wk_wo2)[64];      /* [9][64] out_linear2 */
extern const int64_t *wk_bo2q;           /* [9] */
extern const int32_t *wk_qmo2_q;
extern const int8_t  *wk_qmo2_n;

#endif /* __WEKWS_MODEL_DATA_H__ */
