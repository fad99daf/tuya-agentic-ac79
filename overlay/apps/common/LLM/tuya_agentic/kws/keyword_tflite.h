#ifndef __KEYWORD_TFLITE_H__
#define __KEYWORD_TFLITE_H__
/* ============================================================================
 * keyword_tflite.h — audio_subsys.a 唤醒词引擎接口（逆向重建版 v3）
 *
 * 头文件没有随库发布。本声明从库内 LLVM bitcode 完整逆向（2026-09-05，
 * llvm-nm/llvm-link -S 全函数体核对，含 create 的内存布局/decoder 分支、
 * add_keyword 的 token 语义、forward_pcm 全流程(逐 basic block)、
 * CtcGreedyKws::Search/MatchKeywords 的命中条件与阈值语义）。
 *
 * 库来源：涂鸦 tal_audio_subsys（pi32v2 bitcode, 229 成员 = wekws 运行时 +
 * TFLite Micro + tflite_signal + speexdsp + rnn_vad + 内嵌 heytuya 模型）。
 * ==========================================================================*/
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 引擎工作配置：14 个 4 字节字段（bitcode 实测 sizeof=56，无填充）。
 * keyword_tflite_config_init() 填下述默认值。
 * ⚠ 默认值不能直接用！tensor_arena 默认 98304 超出库内 CHECK 上限 49152
 *   （keyword_spotting_tflite.cc:106: 1<=arena<=49152 否则 printf+exit(-1)
 *   杀调用线程）。必须 config_init 后改 tensor_arena<=49152 再 create。
 * ------------------------------------------------------------------------- */
typedef struct keyword_tflite_config {
    int   num_bins;        /* f0 : 80    fbank mel 滤波组数(FeaturePipeline)  */
    int   sample_rate;     /* f1 : 16000 帧长=25ms*sr/1000, 帧移=10ms*sr/1000  */
    int   tensor_arena;    /* f2 : 98304→★必须改成 1..49152(见上), 直接传给
                              KeywordSpottingTflite 构造器, 无任何钳位        */
    int   beam_context;    /* f3 : 3     CtcBeamSearch context_size           */
    int   beam_size;       /* f4 : 20    CtcBeamSearch beam_size              */
    float threshold;       /* f5 : 1e-4f 同时作 greedy/beam 的默认命中阈值
                              (add_keyword threshold<=0 时回退取它)           */
    int   frame_threshold; /* f6 : 3     CtcGreedyKws min_frames(命中最短帧时长) */
    int   gap_threshold;   /* f7 : 250   CtcGreedyKws max_frames(命中最长帧时长) */
    int   chunk_frames;    /* f8 : 1  每次 Forward 的滑窗批帧数                */
    int   emit_stride;     /* f9 : 3  发射步进: 仅 frame_idx%stride==0 的帧
                              建窗推理(stride=3 → 每 30ms 一窗)               */
    int   left_context;    /* f10: 2  滑窗左上下文帧数                         */
    int   right_context;   /* f11: 2  滑窗右上下文帧数。滑窗宽=(L+1+R)*num_bins
                              =5*80=400, 必须等于模型输入宽 —— 改动任一字段
                              都会撞 Forward CHECK(feature_dim)崩机          */
    int   search_mode;     /* f12: 解码器选择, 决定工作内存(get_mem_size):
                                0=仅beam 77272B(默认)  1=仅greedy 30144B
                                2=双解码器 77400B(beam 命中优先, 同名去重)     */
    int   normalize_skip;  /* f13: 1(默认) 仅在 search_mode=1(greedy) 时生效:
                              非 0 → forward_pcm 跳过后验归一化(供输出已是
                              softmax 概率域的模型省一次转换)。
                              ★内嵌 heytuya 模型输出 log_softmax(全≤0):
                              跳过后 greedy 命中得分=(1-距离/词长)×平均后验
                              恒≤0, 任何正数阈值永不满足(实测全天零命中)。
                              用 greedy 必须置 0 —— 库会自动探测输出域
                              (首批概率和≈1 则不转换)后归一化, 两类模型安全 */
} keyword_tflite_config_t;

/* ---------------------------------------------------------------------------
 * 命中结果：76 字节（wekws DetectedKeyword 的 C 化，run_decoder_search
 * 尾部 memcpy 76B + strncpy(name,63) 实测）。name=add_keyword 注册名。
 * ------------------------------------------------------------------------- */
typedef struct keyword_tflite_result {
    char  name[64];    /* 命中的关键词注册名 */
    int   start_frame; /* 命中起始帧序号(绝对, 10ms/帧) */
    int   end_frame;   /* 命中结束帧序号(绝对)         */
    float score;       /* 命中得分 (1-编辑距离/词长)*平均概率 */
} keyword_tflite_result_t;

/* 工作内存需求(字节), 只看 cfg->search_mode: 0→77272 1→30144 2→77400。 */
int  keyword_tflite_get_mem_size(const keyword_tflite_config_t *cfg);

/* 填默认配置(见结构体注释)。 */
void keyword_tflite_config_init(keyword_tflite_config_t *cfg);

/* 创建引擎。所有对象(192B ctx + FeaturePipeline 29804B + 解码器)都在
 * 调用方给的 mem 里 bump 分配(4 对齐); TFLite 解释器束(50336B)走系统堆
 * (operator new→malloc), 初始化时机需保证堆上有 ~50KB 连续空间。
 *  model_name: NULL/"" → 库内嵌 heytuya FSMN int8 模型(打印"use default
 *              model"); 非空 → 从文件读(走 ifstream, 嵌入式勿用)。
 *  cfg       : 不可传 NULL(NULL=全默认, arena 98304 必触发 CHECK 崩溃)。
 *  返回 0=成功; 非 0=参数/内存不足。 */
int  keyword_tflite_create(const char *model_name,
                           const keyword_tflite_config_t *cfg,
                           void *mem, int mem_size, void **out_handle);

void keyword_tflite_destroy(void *handle);
void keyword_tflite_reset(void *handle);

/* 注册唤醒词(可多次调用注册多个)。tokens=模型输出类别 id 序列(CTC)，
 * 由模型词表决定 —— 内嵌 heytuya 模型输出 40 类(逆向其 tflite:
 * output shape [1,1,40]), token 序列不随库发布, 需标定/向涂鸦获取。
 * threshold<=0 时用 cfg.threshold。返回 0=成功, -1=参数错。 */
int  keyword_tflite_add_keyword(void *handle, const char *name,
                                const int *tokens, int n_tokens,
                                float threshold);

/* 喂 16k/16bit/单声 PCM(任意分块, 内部按 25ms/10ms 帧自缓冲)。
 * accept_pcm: 只调 FeaturePipeline::AcceptWaveform, 帧留在 pipeline,
 *   与 detect 配套的批处理流(嵌入式常听不要用, 见下方 detect 警告)。 */
int  keyword_tflite_accept_pcm(void *handle, const int16_t *pcm, int samples);
int  keyword_tflite_accept_wav(void *handle, const float *wav, int samples);

/* ★常听流式唯一正确入口(逐 basic block 核对): 一站式完成
 *   AcceptWaveform → 帧入 ctx 队列 → (left+1+right)*num_bins 滑窗堆叠
 *   → TFLite Forward → 后验归一化(f13/normalize_skip=0 时自动探测域)
 *   → 解码搜索 → 命中 strncpy(name,63)+帧区间+得分写入 result 并返回 1。
 *   返回 0=本轮无命中, -1=参数非法(handle/result 空)。
 *   内部按 stride 出窗、游标推进、trim 队列, 无泄漏, 无需外部驱动。 */
int  keyword_tflite_forward_pcm(void *handle, const int16_t *pcm, int samples,
                                keyword_tflite_result_t *result);

/* 流式收尾(把不足一帧的尾巴推完, 实时常听模式不用)。 */
void keyword_tflite_set_input_finished(void *handle);

/* ⚠⚠ 对内嵌 heytuya(FSMN 堆叠窗)模型禁用 ⚠⚠
 * detect 是给"非堆叠模型+外部攒批"用的批路径: 它把 pipeline 里的
 * 单帧(宽=num_bins=80)直接喂 TFLite Forward, 而本模型 feature_dim=
 * (left+1+right)*num_bins=400 → CHECK(frame.size()==feature_dim) 失败,
 * printf 后 exit(-1) 直接杀掉调用任务(2026-09-05 实测崩机根因)。
 * 常听唤醒一律用上面的 forward_pcm。 */
int  keyword_tflite_detect(void *handle, keyword_tflite_result_t *results,
                           int max_num);

void keyword_tflite_set_boost_enabled(const void *handle, int enable);

#ifdef __cplusplus
}
#endif
#endif /* __KEYWORD_TFLITE_H__ */
