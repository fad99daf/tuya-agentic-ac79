/* ============================================================================
 * tuya_kws.c — AC79 唤醒词("你好涂鸦"+"嘿涂鸦")薄封装实现 v7 (官方 token)
 * 接口与行为说明见 tuya_kws.h；引擎 API 见 keyword_tflite.h(逆向重建 v3)。
 *
 * v7 (2026-09-07): 换声学团队 v2 算法包(jieli_lib_v2), create(NULL) 默认
 *  模型 g_fsmn_v8_0515_avg_int8_model_data, 官方提供两词 token 序列+阈值
 *  0.7, 取代 v5/v6 探针自标定。主唤醒词切"你好涂鸦"{23,4,27,9,22,5,38,1},
 *  兼容保留"嘿涂鸦"{27,8,22,5,38,1}。引擎侧常量(IR 核实)与 v1 一致:
 *  arena CHECK≤49152、TFLite 束堆 50336, 5 项标定配置全部继续有效。
 *
 * v5/v5.1 探针标定(适用于旧 heytuya 单词模型, 结论已被官方序列取代):
 *  1. forward_pcm 是本引擎唯一的流式推理入口(内部完成 AcceptWaveform→
 *     帧队列→5×80=400 滑窗堆叠→Forward→归一化→解码搜索→命中写 result)。
 *     detect 把 80 宽单帧直接喂 Forward, 撞 CHECK(80≠400)→exit(-1) 杀任务,
 *     对本引擎永久弃用。
 *  2. normalize_skip 必须 0: 默认 1 时 greedy 跳过 log→概率域归一化,
 *     FSMN 模型输出 log_softmax(全≤0) → 命中得分恒负、正数阈值永不满足。
 *  3. frame_threshold 必须 0: 真实 token 段多为单窗(30ms), 默认 3 的
 *     时长门会拦掉大部分真实段。
 * ==========================================================================*/
#include "keyword_tflite.h"
#include "tuya_kws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned int timer_get_ms(void);   /* system/timer.h, 单调 ms */
extern void tuya_agentic_on_wake(void);   /* tuya_agentic_demo.c: 唤醒应答提示音 */

/* ---------------------------------------------------------------------------
 * 官方双唤醒词(声学团队 jieli_lib_v2, 2026-09-07):
 *   "你好涂鸦" → {23,4,27,9,22,5,38,1} (主唤醒词)
 *   "嘿涂鸦"   → {27,8,22,5,38,1}     (兼容保留, 同样可唤醒)
 * token 序列与阈值 0.7 均为随 v2 包官方提供, 取代 v6.1 的探针自标定序列
 * ({27,8,22}/{27,8,22,5}), 标定探针仅留作调优观测。
 * v2 包 IR 核实: create(NULL) 默认模型已换为 g_fsmn_v8_0515_avg_int8
 * _model_data(v1 是 heytuya 专用模型; 包内其余 nhty_hty/anna/tino 等模型
 * 未被引擎引用, 只是随包带出); 引擎侧常量与 v1 完全一致 —— arena CHECK
 * 上限 49152、TFLite 束堆分配 50336, 下方 5 项标定配置继续有效。 */
static const int TUYA_KWS_NIHAOTUYA_TOKENS[] = { 23, 4, 27, 9, 22, 5, 38, 1 };
static const int TUYA_KWS_HEYTUYA_TOKENS[]  = { 27, 8, 22, 5, 38, 1 };
#define TUYA_KWS_TOKENS_READY 1
#define TUYA_KWS_PROBE_LOG    1  /* 正式门控下仍注册 k00-k39 探针(只打印
                                  * 不唤醒): 注册在正式关键词之后, 库按
                                  * 注册序返回每窗首个命中 → 正式词优先,
                                  * 探针只兜没被正式命中的窗; 漏唤醒时
                                  * 仍留有引擎实际输出 token 的调优数据 */

#define TUYA_KWS_NAME        "nihaotuya"   /* 注册名用 ASCII: result.name 走
                                            * strncpy/strcmp, 串口打印也稳 */
#define TUYA_KWS_NAME2       "heytuya"
#define TUYA_KWS_ARENA       49152   /* 构造器 CHECK 上限(1..49152)取满 */
#define TUYA_KWS_THRESHOLD   0.70f   /* 命中阈值。v2 官方值(两词同值);
                                      * greedy 实际触发路径仍是"词长-容差"
                                      * 前缀窗, 得分被 (1-1/词长) 缩水,
                                      * 漏唤醒时先看探针日志再考虑下调 */
#define TUYA_KWS_AWAKE_MS    15000u  /* 唤醒窗: 窗内允许 VAD 起轮 */
#define TUYA_KWS_CLASSES     40      /* 探针覆盖类别数(v1 模型逆向=40; v2 模型
                                      * 实际 vocab 以开机引擎日志 "vocab=%d"
                                      * 为准, 探针只少不多无碍) */

/* 引擎工作内存: search_mode=1 需 30144B。静态块 4 对齐(create 要求)。 */
static char s_kws_mem[30144] __attribute__((aligned(4)));
static void                 *s_hdl;
static volatile int          s_ready;
static int                   s_tried;     /* 只初始化一次, 失败不反复重试 */
static volatile unsigned int s_awake_until;

/* 同句双命中拦截窗口:两次正式命中的起点帧差小于此值=同一句话(50 帧=2s)。
 * 两词共享"涂鸦"尾段,同句可能先后命中两个关键词,只认第一次。*/
#define TUYA_KWS_DUP_START_FRAMES 50
static int s_last_wake_start = -1000000;  /* 上次正式命中的起点帧(很负=尚无) */

/* 探针日志(k00-k39 命中打印, 不唤醒): 最近命中的滚动记录 */
#if TUYA_KWS_PROBE_LOG
static char s_probe_tail[64];             /* 最近若干次命中名, 逗号分隔 */
static unsigned s_probe_fed;              /* 已喂 40ms 帧数(存活心跳用)  */
static unsigned s_probe_hits;             /* 累计命中数                   */
static unsigned s_probe_err;              /* forward_pcm 返回-1次数(ABI监测)*/
#endif

int tuya_kws_init(void)
{
    if (s_tried) return s_ready ? 0 : -1;
    s_tried = 1;

    keyword_tflite_config_t cfg;
    keyword_tflite_config_init(&cfg);
    cfg.tensor_arena = TUYA_KWS_ARENA;   /* ★ 默认 98304 必触发库 CHECK 崩溃 */
    cfg.search_mode  = 1;                /* 仅 greedy: 单关键词 + 内存最省 */
    cfg.normalize_skip = 0;              /* ★默认1: greedy 下跳过后验归一化,
                                          * heytuya 输出 log_softmax(≤0) →
                                          * 命中得分恒负、正数阈值永不满足
                                          * (v2/v3 零命中根因)。0=自动探测
                                          * 输出域并归一化 */
    cfg.frame_threshold = 0;             /* min_frames=0: 真实 token 段多为
                                          * 单窗(30ms), 默认 3 的时长门会拦掉
                                          * (v5 首轮 12 段只放行 1 段); 多
                                          * token 关键词跨度天然 ≥3 个单位
                                          * 不受影响, 误触约束交给距离+阈值 */
    cfg.gap_threshold = 100;             /* max_frames=命中最长跨度, 默认 250(2.5s)
                                          * 会把不同语句的 token 拼成唤醒词:
                                          * 2026-09-06 故事实测, 4.6s 前一句真人
                                          * "嘿tuya"的 k27 与新命令话音的 k08/k22
                                          * 被拼装成假唤醒(score 0.447), 另有一次
                                          * 命中跨 246 帧。真词实测 [27,8] 间隔恒
                                          * 3 帧、k22 尾 ≤40 帧, 100 帧(1s)余量足;
                                          * 超长尾(heytuya 全词窗)本就只是 dup 副本,
                                          * 拦掉无损失 */

    int need = keyword_tflite_get_mem_size(&cfg);
    if (need <= 0 || need > (int)sizeof(s_kws_mem)) {
        printf("[TUYA-KWS] mem need=%d buf=%d, abort\r\n",
               need, (int)sizeof(s_kws_mem));
        return -1;
    }

    void *hdl = NULL;
    /* create 内部 puts("use default model") 后加载内嵌 heytuya 模型;
     * 另在系统堆 operator new(50336B) —— 堆不够时构造器拿到空指针直接崩,
     * 先用同尺寸 malloc 探一遍堆余量(free 后引擎立刻再取同一块)。 */
    {
        void *heap_probe = malloc(50336);
        if (!heap_probe) {
            printf("[TUYA-KWS] heap low for engine(need 50336B), abort\r\n");
            return -1;
        }
        free(heap_probe);
    }
    if (keyword_tflite_create(NULL, &cfg, s_kws_mem, sizeof(s_kws_mem), &hdl) != 0
        || !hdl) {
        printf("[TUYA-KWS] create fail, fallback always-listen\r\n");
        return -1;
    }
    s_hdl = hdl;

#if TUYA_KWS_TOKENS_READY
    if (keyword_tflite_add_keyword(hdl, TUYA_KWS_NAME, TUYA_KWS_NIHAOTUYA_TOKENS,
                                   (int)(sizeof(TUYA_KWS_NIHAOTUYA_TOKENS) /
                                         sizeof(TUYA_KWS_NIHAOTUYA_TOKENS[0])),
                                   TUYA_KWS_THRESHOLD) != 0
        || keyword_tflite_add_keyword(hdl, TUYA_KWS_NAME2, TUYA_KWS_HEYTUYA_TOKENS,
                                      (int)(sizeof(TUYA_KWS_HEYTUYA_TOKENS) /
                                            sizeof(TUYA_KWS_HEYTUYA_TOKENS[0])),
                                      TUYA_KWS_THRESHOLD) != 0) {
        printf("[TUYA-KWS] add_keyword fail, destroy+fallback\r\n");
        keyword_tflite_destroy(hdl);
        s_hdl = NULL;
        return -1;
    }
    printf("[TUYA-KWS] ready: gating ON nihaotuya={23,4,27,9,22,5,38,1} "
           "heytuya={27,8,22,5,38,1} (greedy thr=%.3f window=%ums)\r\n",
           TUYA_KWS_THRESHOLD, (unsigned)TUYA_KWS_AWAKE_MS);
#endif
#if TUYA_KWS_PROBE_LOG
    /* 探针: 每个类别一个单 token 关键词(阈值放最低), 命中只打印
     * [KWS-PROBE] 不参与唤醒 —— greedy 逐关键词独立匹配/容差/冷却,
     * 与正式关键词互不影响。留着它, 漏唤醒/误唤醒都能从日志看到
     * 引擎当时到底输出了什么 token。 */
    {
        int bad = 0;
        for (int id = 0; id < TUYA_KWS_CLASSES; id++) {
            char name[8];
            int  tok = id;
            name[0] = 'k';
            name[1] = (char)('0' + (id / 10) % 10);
            name[2] = (char)('0' + id % 10);
            name[3] = '\0';
            if (keyword_tflite_add_keyword(hdl, name, &tok, 1, 0.0001f) != 0) {
                bad++;
            }
        }
        if (bad) {
            printf("[TUYA-KWS] probe add_keyword fail %d/%d!\r\n",
                   bad, TUYA_KWS_CLASSES);
        }
    }
    s_probe_tail[0] = '\0';
#if TUYA_KWS_TOKENS_READY
    printf("[TUYA-KWS] probe log ON (k00-k39 print-only)\r\n");
#else
    printf("[TUYA-KWS] PROBE mode: say the wake word a few times, "
           "watch [KWS-PROBE] hits (device will not talk back)\r\n");
#endif
#endif
    s_ready = 1;
    return 0;
}

int tuya_kws_ready(void)
{
    return s_ready;
}

/* 内部: 喂一块 PCM(样本数) → forward_pcm 一站式推理+解码 → 处理命中。
 * v5: forward_pcm = AcceptWaveform + 滑窗堆叠 + Forward + 归一化 +
 *     解码搜索 + 命中写 result(返回1)。绝不调 detect —— 它把 80 宽
 *     单帧喂 Forward, 对本模型必触发 CHECK(80≠400)→exit(-1) 杀任务。
 *     返回 -1 仅在参数非法(handle/result 空), 心跳里以 err 计数监测
 *     ABI/签名是否仍匹配。 */
static void kws_push(const short *pcm, int samples)
{
    keyword_tflite_result_t r;
    memset(&r, 0, sizeof(r));
    int n = keyword_tflite_forward_pcm(s_hdl, pcm, samples, &r);
    if (n != 1 || !r.name[0]) {
#if TUYA_KWS_PROBE_LOG
        if (n < 0) s_probe_err++;
        /* 存活心跳: 每 256 帧(40ms*256≈10s)报一次喂入量, 证明链路在跑 */
        if ((++s_probe_fed & 0xFFu) == 0) {
            printf("[KWS-PROBE] alive fed=%u hits=%u err=%u\r\n",
                   s_probe_fed, s_probe_hits, s_probe_err);
        }
#endif
        return;
    }

#if TUYA_KWS_TOKENS_READY
    /* 只认正式关键词名; 探针(kXX)命中落到下面的打印分支, 不唤醒 */
    if (strcmp(r.name, TUYA_KWS_NAME) == 0 ||
        strcmp(r.name, TUYA_KWS_NAME2) == 0) {
        s_awake_until = timer_get_ms() + TUYA_KWS_AWAKE_MS;
        /* 同一句 utterance 双命中拦截:两词共享"涂鸦"尾段({...,22,5,38,1},
         * 且 greedy 容差下 9/8 可互替),说"你好涂鸦"可能 nihaotuya、heytuya
         * 相继各中一次,多播一声"我在";同一词的滑窗也可能重复返回(实测
         * 第二次可晚数秒)。按命中起点帧判同句:|Δstart|<50 帧(2s)视为同一
         * 句,只认第一次;真正的重复唤醒起点至少差整句长度。*/
        if (s_last_wake_start > -1000000 &&
            abs(r.start_frame - s_last_wake_start) < TUYA_KWS_DUP_START_FRAMES) {
            printf("[TUYA-KWS] WAKE dup '%s' f=%d (last start %d), suppressed\r\n",
                   r.name, r.start_frame, s_last_wake_start);
            return;
        }
        s_last_wake_start = r.start_frame;
        printf("[TUYA-KWS] WAKE '%s' score=%.3f f=%d..%d (window %ums)\r\n",
               r.name, r.score, r.start_frame, r.end_frame,
               (unsigned)TUYA_KWS_AWAKE_MS);
        tuya_agentic_on_wake();   /* 播唤醒应答提示音"我在"(demo 侧, 同线程) */
        return;
    }
#endif
#if TUYA_KWS_PROBE_LOG
    /* 探针命中: 逐次打印 + 维护滚动序列(截尾防刷屏) */
    s_probe_hits++;
    printf("[KWS-PROBE] hit=%s score=%.3f f=%d..%d t=%u\r\n",
           r.name, r.score, r.start_frame, r.end_frame, timer_get_ms());
    {
        int len = (int)strlen(s_probe_tail);
        if (len > 40) {                  /* 只留尾部 */
            memmove(s_probe_tail, s_probe_tail + len - 40, 41);
            len = 40;
        }
        if (len) s_probe_tail[len++] = ',';
        int nl = (int)strlen(r.name);
        if (len + nl >= (int)sizeof(s_probe_tail)) {
            nl = (int)sizeof(s_probe_tail) - 1 - len;
        }
        memcpy(s_probe_tail + len, r.name, nl);
        s_probe_tail[len + nl] = '\0';
    }
#endif
}

void tuya_kws_feed(const void *pcm_frame, int bytes)
{
    if (!s_ready || !s_hdl || !pcm_frame || bytes < 2) return;
    kws_push((const short *)pcm_frame, bytes / 2);
}

int tuya_kws_awake(void)
{
    if (!s_ready) return 1;   /* 引擎不可用: 恒醒 → 门控失效(常听回退) */
#if TUYA_KWS_TOKENS_READY
    return (int)(timer_get_ms() - s_awake_until) < 0;
#else
    return 0;                 /* 标定模式: 恒关 → 话音全走 idle 排空喂引擎 */
#endif
}

void tuya_kws_window_kick(void)
{
#if TUYA_KWS_TOKENS_READY
    if (s_ready) s_awake_until = timer_get_ms() + TUYA_KWS_AWAKE_MS;
#endif
}
