/* ============================================================================
 * tuya_kws.c — AC79 唤醒词("嘿tuya")薄封装实现 v6.1 (正式门控)
 * 接口与行为说明见 tuya_kws.h；引擎 API 见 keyword_tflite.h(逆向重建 v3)。
 *
 * v5/v5.1 探针标定完成, 三轮根因均已闭环:
 *  1. forward_pcm 是本引擎唯一的流式推理入口(内部完成 AcceptWaveform→
 *     帧队列→5×80=400 滑窗堆叠→Forward→归一化→解码搜索→命中写 result)。
 *     detect 把 80 宽单帧直接喂 Forward, 撞 CHECK(80≠400)→exit(-1) 杀任务,
 *     对本模型永久弃用。
 *  2. normalize_skip 必须 0: 默认 1 时 greedy 跳过 log→概率域归一化,
 *     heytuya 输出 log_softmax(全≤0) → 命中得分恒负、正数阈值永不满足
 *     (v2/v3 全天零命中根因)。
 *  3. frame_threshold 必须 0: 真实 token 段多为单窗(30ms), 默认 3 的
 *     时长门把 v5 首轮 12 段里的 11 段直接拦掉。
 *
 * v6 (2026-09-05 17:24 标定回填, 开正式门控) → v6.1 修正:
 *  v6 实测 [27,8] 后验均值 0.955 仍未唤醒 → 复核 IR 发现 greedy 扫描是
 *  "前缀窗 + 得分不足即跳过"规则(详见下方 token 块注释): 每个起点只从
 *  m=len-容差 的前缀窗开始试, 得分不足直接 start+=m 跳过该起点 → 更长
 *  的整词精确窗基本不可达 → len=2 的 {27,8} 结构性永不命中。
 *  v6.1: heytuya2 改 {27,8,22}(首选窗=稳定对[27,8], 得分(1-1/3)×均值),
 *  阈值 0.6094→0.50(前缀窗得分被 (1-1/len) 缩水, 旧值按整词标定不可达)。
 *  探针 k00-k39 保留: 注册在正式关键词之后, 库按注册序返回每窗首个命中
 *  → 正式关键词优先, 探针只兜走未被正式命中的窗口; 漏唤醒时仍能从日志
 *  看到引擎实际输出的 token 序列和分数。
 * ==========================================================================*/
#include "keyword_tflite.h"
#include "tuya_kws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned int timer_get_ms(void);   /* system/timer.h, 单调 ms */
extern void tuya_agentic_on_wake(void);   /* tuya_agentic_demo.c: 唤醒应答提示音 */

/* ---------------------------------------------------------------------------
 * heytuya 唤醒词 token 序列(模型 40 类 CTC id, blank 在折叠时已剔除)。
 * 标定数据(两轮日志, 6 句有效"嘿tuya"):
 *   [27,8] 对 6/6 句稳定出现(间隔恒 30ms), 是核心声学指纹;
 *   尾部多变: [22,5] / [22] / [1] / [38,1], 部分被 blank 吃掉。
 *
 * ★v6 实测教训(greedy 扫描的"前缀窗+跳过"规则, IR %91-%153 核实):
 *   每个起点只从 m=len-容差 的前缀窗开始试, 该窗距离≤容差但得分不足时
 *   直接 start+=m 跳过, 不再试该起点更长的窗 → 整词精确匹配基本不可达,
 *   真正的触发路径就是前缀窗(距离=容差, 得分被 (1-1/len) 缩水)。
 *   因此 len=2 的 {27,8} 首选窗=单 token 模糊窗, 得分≤0.5×后验, 结构性
 *   永不命中(v6 日志实证: [27,8] 均值 0.955 也没醒)。
 *   len=3 的 {27,8,22} 首选窗=稳定对 [27,8] 本身: 得分=(1-1/3)×均值,
 *   阈值 0.50 ⇒ 对均值≥0.75 即醒 —— 标定里 4 句好发音(均值 0.82~0.955)
 *   全中, 1 句弱发音(0.503)正确不中。
 *   heytuya={27,8,22,5} 保留: 尾部齐全时经 [27,8,22] 前缀窗(0.75×均值)
 *   命中, 分数更高、误触更低。 */
static const int TUYA_KWS_HEYTUYA_TOKENS[] = { 27, 8, 22, 5 };
static const int TUYA_KWS_HEY2_TOKENS[]    = { 27, 8, 22 };
#define TUYA_KWS_TOKENS_READY 1
#define TUYA_KWS_PROBE_LOG    1  /* 正式门控下仍注册 k00-k39 探针(只打印
                                  * 不唤醒): 注册在正式关键词之后, 库按
                                  * 注册序返回每窗首个命中 → 正式词优先,
                                  * 探针只兜没被正式命中的窗; 漏唤醒时
                                  * 仍留有引擎实际输出 token 的调优数据 */

#define TUYA_KWS_NAME        "heytuya"
#define TUYA_KWS_NAME2       "heytuya2"
#define TUYA_KWS_ARENA       49152   /* 构造器 CHECK 上限(1..49152)取满 */
#define TUYA_KWS_THRESHOLD   0.50f   /* 命中阈值。实际触发路径是"词长-容差"
                                      * 前缀窗, 得分=(1-1/词长)×段均值被缩水:
                                      * 0.6094 按整词命中标定, 前缀窗结构上
                                      * 到不了(v6 实测); 0.50 ⇒ [27,8] 对
                                      * 均值≥0.75 即醒, 弱句(0.503)不中 */
#define TUYA_KWS_AWAKE_MS    15000u  /* 唤醒窗: 窗内允许 VAD 起轮 */
#define TUYA_KWS_CLASSES     40      /* 内嵌模型输出类别数(tflite 逆向) */

/* 引擎工作内存: search_mode=1 需 30144B。静态块 4 对齐(create 要求)。 */
static char s_kws_mem[30144] __attribute__((aligned(4)));
static void                 *s_hdl;
static volatile int          s_ready;
static int                   s_tried;     /* 只初始化一次, 失败不反复重试 */
static volatile unsigned int s_awake_until;

/* 同句双命中拦截窗口:两次正式命中的起点帧差小于此值=同一句话(50 帧=2s)。
 * heytuya2 前缀窗与 heytuya 全词窗对同一句的命中起点相同,只差返回时机。*/
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
    if (keyword_tflite_add_keyword(hdl, TUYA_KWS_NAME, TUYA_KWS_HEYTUYA_TOKENS,
                                   (int)(sizeof(TUYA_KWS_HEYTUYA_TOKENS) /
                                         sizeof(TUYA_KWS_HEYTUYA_TOKENS[0])),
                                   TUYA_KWS_THRESHOLD) != 0
        || keyword_tflite_add_keyword(hdl, TUYA_KWS_NAME2, TUYA_KWS_HEY2_TOKENS,
                                      (int)(sizeof(TUYA_KWS_HEY2_TOKENS) /
                                            sizeof(TUYA_KWS_HEY2_TOKENS[0])),
                                      TUYA_KWS_THRESHOLD) != 0) {
        printf("[TUYA-KWS] add_keyword fail, destroy+fallback\r\n");
        keyword_tflite_destroy(hdl);
        s_hdl = NULL;
        return -1;
    }
    printf("[TUYA-KWS] ready: gating ON heytuya={27,8,22,5} heytuya2={27,8,22} "
           "(greedy thr=%.3f window=%ums)\r\n",
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
        /* 同一句 utterance 双命中拦截:heytuya2{27,8,22} 前缀窗先中(~0.3s 处),
         * heytuya{27,8,22,5} 全词窗后中——greedy 容差下全词窗可跨到上百帧外
         * (实测同一句两次命中起点同为 f=3621,第二次晚 4s 才返回,多播一声
         * "我在")。按命中起点帧判同句,与时间无关:|Δstart|<50 帧(2s)视为
         * 同一句,只认第一次;真正的重复唤醒起点至少差整句长度。*/
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
