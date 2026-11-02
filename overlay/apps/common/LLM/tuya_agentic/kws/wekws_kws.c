/* ============================================================================
 * wekws_kws.c — 自训"小涂小涂"唤醒词引擎(tuya_kws.h API, TUYA_KWS_ENGINE==2)
 *
 * v9d 链路: PCM 16k/16bit -> wekws_fbank(kaldi 80 mel -> 5 帧堆叠 400 维,
 * 30ms 步长) -> wekws_nn(FSMN int16 量化图, Sim16 位级镜像; 4 层 x 11 帧
 * 状态缓存, T=1 流式已证与整句一致) -> wekws_ctc(CTC 前缀束 + 变体扫描,
 * 9 类全进束)。PC 端宿主 18 卷对拍 PASS(worst 0.046)。
 *
 * 门控语义(与 v6/tuya_kws v7 一致, demo 侧零改动):
 *   阈值 0.45(v9d 研发线; 涂鸦产品线 0.70 在 tuya_kws.c); 命中 -> 2.5s
 *   冷却 + 全状态复位; 唤醒窗 15s, tuya_kws_window_kick() 续窗。
 *   喂端策略静音复位(WK_SIL_RMS_TH, 板测13定案): 静音 240ms 判句间——
 *   束内有进行中的词仅复位 nn(保束桥接词内停顿), 否则三件套全复位
 *   (清 >=1s 语音前导的模型压制窗 + 环境声束垃圾, 宿主唤醒场景 0->0.62)。
 *
 * 开机自测(WK_SELFTEST, 默认 0): v9d 换模后自测数据未重生, 保持 0。 */
#include "app_config.h"

#if defined(TUYA_KWS_ENABLE) && TUYA_KWS_ENGINE == 2

#include <stdio.h>
#include <string.h>
#include <math.h>          /* sqrtf: 喂端块 RMS */

#include "tuya_kws.h"
#include "wekws_fbank.h"
#include "wekws_nn.h"
#include "wekws_ctc.h"
#include "wekws_model_data.h"
#include "asm/WL82.h"       /* JL_TIMER4: 逐级耗时打点(仅读计数, 见下) */

#ifndef BIT                 /* generic/typedef.h 惯例宏; WL82.h 不带, 本地补 */
#define BIT(n) (1u << (n))
#endif

extern unsigned int timer_get_ms(void);   /* system/timer.h, 单调 ms */
extern void tuya_agentic_on_wake(void);   /* tuya_agentic_demo.c: 唤醒应答 */
extern int get_recoder_state(void);       /* 本地 VAD 开口态(1=说话中, demo 同款声明) */

#define WK_THRESHOLD     0.45f    /* 裁头复标后维持: 决策保持区 0.383~0.773 */
#define WK_COOLDOWN_MS   2500u    /* HANDOFF: 建议设备端 2.5s 唤醒冷却 */
#define WK_AWAKE_MS      15000u   /* 与 v7 引擎一致 */
/* 喂端策略静音复位(板测13定案, ref 逐句口径的流式等价物): 块 RMS 低于
 * 门限连续 WK_SIL_CHUNKS 喂(40ms/喂, ≈240ms)判句间静音 —— 束内有进行中
 * 的词(变体前缀且新鲜)则仅复位 nn(保束桥接词内停顿: 板测①"小图, 小图"
 * 逗号停顿全靠束桥接), 否则 fbank+nn+ctc 三件套全复位(清 >=1s 语音前导
 * 的模型压制窗 + 环境声束垃圾)。门限 350 为初值: 宿主考卷静音 RMS≈8-25,
 * 语音≈1300+; 以心跳 rms= 与 sil-rst 日志为准标定。 */
#define WK_SIL_RMS_TH    350
#define WK_SIL_CHUNKS    6
/* WK_SELFTEST 默认 0, 定义在 wekws_model_data.h(自测与考卷 PCM 整体编出) */

static volatile int          s_ready;
static volatile unsigned int s_awake_until;
static volatile unsigned int s_cooldown_until;
static unsigned int          s_fed;      /* 已喂 40ms 帧数(心跳) */
static unsigned int          s_hits;     /* 累计唤醒 */
static float                 s_max_seen; /* 观测到的最高分(心跳窗口, 报后清零) */
static float                 s_utt_max;  /* 本段开口(VAD start→stop)最高分 */
static int                   s_vad_prev; /* VAD 开口态沿检测(收口=报分时机) */
static unsigned int          s_sil_run;  /* 连续静音喂计数(喂端策略复位) */
static unsigned int          s_rms_max;  /* 心跳窗口内最大块 RMS(标定门限用) */
static unsigned int          s_last_cand_ms; /* 弱候选日志 500ms 节流 */

static float s_lg[WK_NN_QH][WK_NCLASS];  /* flush 批量 logits(逐 hop) */

/* ---- 逐级耗时打点(2026-10-28 板测⑤定二刀; 纯计时, 不碰推理算术) ----
 * TIMER4 空闲(全构建仅休眠的 asr 压测代码引用, 本产品不编其任务), JL 自家
 * 压测同款配置: sys clk 直驱, PRD=-1 自由跑; u32 差分读数天然抗回绕。
 * fb = fbank 净耗时(扣已计的 nn/ctc), nn = DS-TCN 批算(flush), ctc = 束
 * 搜索, 单位 = tick。
 * 校准: 心跳行 win = 本窗口墙钟(64 喂理论 2560ms, 明显超出=主循环仍丢帧);
 * fb+nn+ctc 合计应 ≈ feed_max 同量级, 借 win 锚定 tick→us 比例。 ---- */
static unsigned int s_t_fb, s_t_nn, s_t_ctc;
static unsigned int s_t_hops, s_t_feed_max, s_hb_ms;

/* ---- 帧回调: 只入队(NN/CTC 移到喂音后批算, 板测⑦二刀) ---- */
static void wk_on_frame(void *ctx, const float *f40)
{
    (void)ctx;
    wekws_nn_push(f40);
}

/* ---- 命中门(逐 hop, 批量推理后按次序调用) ---- */
static void wk_gate(float score)
{
    if (score < WK_THRESHOLD) return;
    {
        unsigned int now = timer_get_ms();
        if ((int)(now - s_cooldown_until) < 0) {
            /* 冷却期内: 只复位, 不唤醒 */
            wekws_nn_reset();
            wekws_fbank_reset();
            wkc_reset();
            return;
        }
        s_hits++;
        s_cooldown_until = now + WK_COOLDOWN_MS;
        s_awake_until = now + WK_AWAKE_MS;
        printf("[WEKWS] WAKE 小涂小涂 score=%.3f (hits=%u window=%ums)\r\n",
               score, s_hits, (unsigned)WK_AWAKE_MS);
        /* 全复位: 命中后残响/尾音不再触发 */
        wekws_nn_reset();
        wekws_fbank_reset();
        wkc_reset();
        tuya_agentic_on_wake();
    }
}

static void wk_reset_all(void)
{
    wekws_fbank_reset();
    wekws_nn_reset();
    wkc_reset();
    s_max_seen = 0.0f;
    s_utt_max = 0.0f;
}

#if WK_SELFTEST
/* 自测帧回调: 只入队; 分片喂入后批量推理+打分(不触发唤醒) */
static void wk_probe_frame(void *ctx, const float *f40)
{
    (void)ctx;
    wekws_nn_push(f40);
}

/* 名字避让: wk_selftest 是考卷数组(wekws_model_data.h), 函数叫 _run */
static void wk_selftest_run(void)
{
    printf("[WEKWS] self-test: %d clips\r\n", wk_selftest_n);
    for (int i = 0; i < wk_selftest_n; i++) {
        const wk_selftest_t *st = &wk_selftest[i];
        float  best = 0.0f;
        unsigned t0, t1;
        int    frames, off, hn, h;

        wk_reset_all();
        t0 = timer_get_ms();
        for (off = 0; off + 640 <= st->samples; off += 640) {
            wekws_fbank_feed(st->pcm + off, 640, wk_probe_frame, 0);
            hn = wekws_nn_flush(s_lg, WK_NN_QH);
            for (h = 0; h < hn; h++) {
                float sc = wkc_step(s_lg[h]);
                if (sc > best) best = sc;
            }
        }
        if (st->samples - off > 0) {
            wekws_fbank_feed(st->pcm + off, st->samples - off,
                             wk_probe_frame, 0);
            hn = wekws_nn_flush(s_lg, WK_NN_QH);
            for (h = 0; h < hn; h++) {
                float sc = wkc_step(s_lg[h]);
                if (sc > best) best = sc;
            }
        }
        t1 = timer_get_ms();
        frames = 1 + (st->samples - 400) / 160;
        printf("[WEKWS] %-8s score=%.3f exp=%.3f %s  (%d frames, %ums, %.2fms/f)\r\n",
               st->name, best, st->expected,
               (best >= WK_THRESHOLD) == (st->expected >= WK_THRESHOLD)
                   ? "OK" : "MISMATCH",
               frames, t1 - t0, frames ? (t1 - t0) * 1.0f / frames : 0.0f);
        wk_reset_all();
    }
}
#endif /* WK_SELFTEST */

int tuya_kws_init(void)
{
    static int s_tried;
    if (s_tried) return s_ready ? 0 : -1;
    s_tried = 1;

    wk_model_cache_init();      /* 模型常量 flash -> 内部 RAM(首次推理前必须) */
    wk_reset_all();
    /* 打点时钟: JL 压测同款(BIT(14) 停钟配置 -> PRD 满幅 -> BIT(0) 启动) */
    JL_TIMER4->CON = BIT(14);
    JL_TIMER4->PRD = -1;
    JL_TIMER4->CON = BIT(0);
    s_hb_ms = timer_get_ms();
#if WK_SELFTEST
    wk_selftest_run();
#endif
    printf("[WEKWS] ready: xiao-tu-xiao-tu v9d-FSMN thr=%.3f cooldown=%ums "
           "window=%ums (int16 graph, 9-class CTC)\r\n",
           WK_THRESHOLD, (unsigned)WK_COOLDOWN_MS, (unsigned)WK_AWAKE_MS);
    s_ready = 1;
    return 0;
}

int tuya_kws_ready(void)
{
    return s_ready;
}

void tuya_kws_feed(const void *pcm_frame, int bytes)
{
    if (!s_ready || !pcm_frame || bytes < 2) return;

    /* VAD 收口沿(1→0): 报本段开口引擎最高分 —— live 分数离阈值多远, 板测②
     * 缺的就是它(10s 心跳差 0.1s 没打出来)。各状态 feed 都在跑(idle 排空/
     * 上行/TTS/音乐), 40ms 内必被采样; 与喂音同任务, 无竞态。 */
    {
        int vad_now = get_recoder_state();
        if (s_vad_prev && !vad_now) {
            printf("[WEKWS] utt max=%.3f (thr=%.3f) hits=%u\r\n",
                   s_utt_max, WK_THRESHOLD, s_hits);
            s_utt_max = 0.0f;
        }
        s_vad_prev = vad_now;
    }

    s_fed++;
    /* 心跳: 每 64 帧喂入(≈2.6s)报活性+近期最高分, 报后清零(板测②从 256 帧
     * ≈10.2s 收紧 —— 现场证据就差它没打出来) */
    if ((s_fed & 0x3Fu) == 0) {
        unsigned int now = timer_get_ms();
        unsigned int win = now - s_hb_ms;
        unsigned int n = s_t_hops ? s_t_hops : 1;
        unsigned int nst = 0, npw = 0;   /* nn 拆段: pw=MAC 环 st=解包 */
        wekws_nn_timers(&nst, &npw);
        s_hb_ms = now;
        printf("[WEKWS] alive fed=%u hits=%u max=%.3f rms=%u | fb=%u nn=%u"
               "(pw=%u st=%u) ctc=%u kt/hop(h=%u) feed_max=%uk win=%ums\r\n",
               s_fed, s_hits, s_max_seen, s_rms_max,
               s_t_fb / n / 1000, s_t_nn / n / 1000, npw / n / 1000,
               nst / n / 1000, s_t_ctc / n / 1000,
               s_t_hops, s_t_feed_max / 1000, win);
        s_max_seen = 0.0f;
        s_rms_max = 0;
        s_t_fb = s_t_nn = s_t_ctc = 0;
        s_t_hops = 0;
        s_t_feed_max = 0;
    }
    {
        /* 喂端策略静音复位(先于本块喂入判, 复位后新状态从当前块起算):
         * 整块平方均值 + 一次 sqrtf; 触发恰在 s_sil_run==CHUNKS 处(每段
         * 静音只复位一次, 后续静音喂只维持计数) */
        const short *p = (const short *)pcm_frame;
        int    ns = bytes / 2, i;
        unsigned long long e = 0;
        unsigned int rms;
        for (i = 0; i < ns; i++) e += (unsigned long long)p[i] * p[i];
        rms = ns ? (unsigned int)sqrtf((float)(e / ns)) : 0;
        if (rms > s_rms_max) s_rms_max = rms;
        if (rms < WK_SIL_RMS_TH) {
            s_sil_run++;
            if (s_sil_run == WK_SIL_CHUNKS) {
                if (wkc_has_variant_progress()) {
                    /* 词进行中: 仅清 FSMN 记忆(前导压制毒源), 束桥接词内停顿 */
                    wekws_nn_reset();
                    printf("[WEKWS] sil-rst rms=%u nn-only\r\n", rms);
                } else {
                    /* 句间: 三件套全复位(ref 逐句口径) */
                    wekws_fbank_reset();
                    wekws_nn_reset();
                    wkc_reset();
                    printf("[WEKWS] sil-rst rms=%u full\r\n", rms);
                }
            }
        } else {
            s_sil_run = 0;
        }
    }
    {
        /* 二刀批量管线: fbank(只入队) -> flush 批 NN -> 逐 hop CTC/门。
         * fb 净耗时 = 整次喂入 - 已计的 nn/ctc */
        unsigned int nn0 = s_t_nn, ctc0 = s_t_ctc, f0, f1, n0, c0;
        int    hn, h;
        f0 = JL_TIMER4->CNT;
        wekws_fbank_feed((const short *)pcm_frame, bytes / 2, wk_on_frame, 0);
        n0 = JL_TIMER4->CNT;
        hn = wekws_nn_flush(s_lg, WK_NN_QH);
        s_t_nn += JL_TIMER4->CNT - n0;
        c0 = JL_TIMER4->CNT;
        for (h = 0; h < hn; h++) {
            float score = wkc_step(s_lg[h]);
            s_t_hops++;
            if (score > s_max_seen) s_max_seen = score;
            if (score > s_utt_max)  s_utt_max  = score;
            if (score >= 0.05f) {
                /* 弱候选稀疏日志: 现场分数离阈值多远/多久出现一次 */
                unsigned int now = timer_get_ms();
                if ((int)(now - s_last_cand_ms) >= 500) {
                    s_last_cand_ms = now;
                    printf("[WEKWS] cand f=%u s=%.3f\r\n", s_fed, score);
                }
            }
            wk_gate(score);
        }
        s_t_ctc += JL_TIMER4->CNT - c0;
        f1 = JL_TIMER4->CNT;
        s_t_fb += (f1 - f0) - ((s_t_nn - nn0) + (s_t_ctc - ctc0));
        if (f1 - f0 > s_t_feed_max) s_t_feed_max = f1 - f0;
    }
}

int tuya_kws_awake(void)
{
    if (!s_ready) return 1;
    return (int)(timer_get_ms() - s_awake_until) < 0;
}

void tuya_kws_window_kick(void)
{
    if (s_ready) s_awake_until = timer_get_ms() + WK_AWAKE_MS;
}

#endif /* TUYA_KWS_ENABLE && TUYA_KWS_ENGINE == 2 */
