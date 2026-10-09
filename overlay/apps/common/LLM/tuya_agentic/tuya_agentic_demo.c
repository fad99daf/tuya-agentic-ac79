/*
 * tuya_agentic_demo.c -- 涂鸦 AI 文本对话(AC791N 入口)。
 *
 * 流程(阶段0/1/2):
 *   iot_init(pal) → iot_client_init(devid/secret/local_key)
 *   → iot_client_get_session_token → parse_token
 *   → tai_ctx_init → tai_connect → tai_send_text → on_text 收回复
 *
 * 改自 agentic-kit/examples/posix/ai/rtc-tcp-client/text_chat_demo.c。
 * posix 专属的 main/usleep/iot_init_default 换成 AC79 的线程入口/msleep/iot_init(pal)。
 */
#include "app_config.h"   /* 拿 TUYA_BARGE_IN_ENABLE 等 app 级开关。同目录其它 .c 都显式 include,
                            * 原 demo.c 漏了→TUYA_BARGE_IN_ENABLE 不可见→barge-in 三处 #ifdef 全被编译掉,
                            * 功能从未进固件。纯宏(含 board_config.h),不引入 SDK FILE,不触发 newlib 冲突。*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system/includes.h"      /* late_initcall 等 */
#include "system/os/os_api.h"     /* msleep */
#include "mbedtls/base64.h"       /* 宿主 mbedTLS 3.4 */

#include "pal.h"
#include "iot_client.h"
#include "tuya_ai.h"
#include "tuya_ai_select.h"        /* 传输层选择:TUYA_TRANSPORT_STM_ENABLE=1 时 tai_* 重定向到 stm(UDP优先)后端 */
#include "tuya_agentic.h"
#include "tuya_mcp.h"
#include "tuya_ble_prov.h"        /* tuya_ble_wifi_creds_t(BLE 配网结果类型)*/
#include "tuya_auth_region.h"     /* 量产授权区(USER@0x5FE000):96B三元组 load/write/erase(阶段2) */
#ifdef TUYA_MUSIC_ENABLE
#include "tuya_music.h"           /* 音乐 SKILL 文本流重组+解析(实现 tuya_music.c) */
/* 实现见 apps/wifi_story_machine/app_music.c(导出模式同 app_music_play_netcfg_prompt):
 * net_download(https 自动 TLS)→ mp3 解码;on_dec_end 在播完/出错停机时回调。*/
int  app_music_tuya_play_url(const char *url, void (*on_dec_end)(int));
void app_music_tuya_music_stop(void);
int  app_music_tuya_music_busy(void);   /* 网络音乐仍占用(下载/解码中):等待循环感知失败退出 */
int  app_music_tuya_music_started(void);/* 网络音乐解码器已 START(过 open 在飞窗):停乐只许在此之后 */
void app_music_tuya_play_wake_prompt(void); /* "嘿tuya"唤醒应答提示音(WakeHeyTuya.mp3) */
#endif
#ifdef TUYA_UPLINK_OPUS_ENABLE
#include "tuya_opus_enc.h"        /* 上行 opus 编码(杰理闭源库经 audio_server virtual 通道,实现 tuya_opus_enc.c) */
#endif

/* 阶段3 用的音频流接口(实现见 apps/common/LLM/audio/audio_input.c)。
 * 这里【故意不】#include "audio_input.h":它会经 audio_server.h → utils/fs/fs.h 引入
 * SDK 的 FILE/fread/fwrite…,与本 TU 已包含的 newlib <stdio.h>(agentic-kit 依赖)的
 * FILE/__sFILE 冲突,报 "typedef redefinition / conflicting types"。
 * 只前向声明需要的几个函数(签名均为基本类型,不涉 FILE),链接期由 audio_input.c 提供符号。*/
int  _device_get_voice_data(void *data, unsigned int max_len);
int  _device_write_voice_data(void *data, unsigned int len);
void _device_wbuf_clear(void);
void _device_rbuf_clear(void);          /* 清下行播放 cbuf:barge-in 时立刻停 TTS */
void tts_prebuffer_arm(void);           /* TTS 本轮预蓄水:on_audio 收到 START 帧时调用 */
void audio_stream_init(int sample_rate, int bit_dept, int channel_num);
void start_audio_stream(void);
void stop_audio_stream(void);
int  get_recoder_state(void);
unsigned int _device_get_voice_level(void);   /* 录音 cbuf 水位,诊断上行是否丢话头 */
unsigned int _device_get_play_level(void);    /* 下行播放 cbuf 水位:排空≈DAC 播完 */
void _device_net_audio_play(bool flag);       /* 停/起 TTS 网络播放器(音乐交接时让出/收回 DAC) */
void _device_net_audio_play_keep(void);       /* 同上但恢复时不清播放 cbuf:音乐期云端已开答,保住回答头部 */

/* ===== 上行延迟诊断开关(定位"打断不佳"用)=====
 * 打开后在 tai_send_audio_chunk 前后打时间戳,超 UPLINK_LAT_WARN_MS 才打印(避免刷屏)。
 * 同时打印 send 期间的 mic cbuf 水位变化,判断"send 慢→堆积→丢音"是否成立。
 * 以及 send 发生时 TTS 是否在播(锁竞争假说: TTS drain 期间 worker 频繁持 yield_mutex)。
 * 用法: 定位完注释掉这行宏即可恢复原样。*/
#define TUYA_UPLINK_LATENCY_DEBUG
#ifdef TUYA_UPLINK_LATENCY_DEBUG
  #define UPLINK_LAT_WARN_MS     30      /* send 单帧耗时超过此值才打印(正常 ~5-15ms) */
  #define UPLINK_LAT_CBUF_WARN   2560    /* cbuf 水位超过此值(2 帧=80ms)才告警 */
#endif

/* === 产品三件套(涂鸦 IoT 平台创建产品时获得,构建期固定)===(前端填这个)*/
#define TUYA_PRODUCT_KEY    "rckqt7yipzqx4tv9"              /* PID [临时]10dB声学测试与同事样机同pid,测完恢复ptsig07xv6aehihz */
#define TUYA_UUID           "uuid81270ef7739ea8d7"          /* 设备 UUID */
#define TUYA_AUTH_KEY       "QKQJJSSZI6IcW2nTsZWDs0hAoAoAaOCC"  /* 授权码 AuthKey */

/* === 激活 token(配网时涂鸦 App 下发;调试期可从平台/App 取一次填这里)===(前端填这个)*/
#define TUYA_ACTIVATION_TOKEN "xxxxxxxx"

/* 走哪条路:
 *  1 = 配网激活路径(正解):三件套 + token → iot_client_init_on_boarding_with_token
 *      → 涂鸦云下发 devid/secret_key/local_key。token 单次/短期有效,反复测要刷新。
 *  0 = 直连路径(已预注册设备):填下面三元组,直接 iot_client_init。
 *      三元组正常应由"配网激活"获得;直连仅在你已从平台拿到时省事用。
 * 正式量产应:激活成功后把 devid/secret/local_key 存 flash,下次开机走直连。*/
#define TUYA_USE_ONBOARDING 1
#define TUYA_DEVID      "tuya_xxx"   /* 仅 TUYA_USE_ONBOARDING=0 时用 */
#define TUYA_SECRET_KEY "xxxx"       /* 仅 TUYA_USE_ONBOARDING=0 时用 */
#define TUYA_LOCAL_KEY  "xxxx"       /* 仅 TUYA_USE_ONBOARDING=0 时用 */

/* ===== [阶段2] 运行期三元组(方案《涂鸦三元组量产烧录改造方案》§5.2,2026-09-28)=====
 * tuya_agentic_main 开机先 tuya_auth_region_load() 填 g_tuya_triplet;三元组注入点
 * (本文件 tuya_agentic_demo 调试路径 / BLE配网 tuya_ble_netcfg_start / on_boarding
 * 填参)一律经下面 getter 取值:授权区有效→区域码;无效→回退上面默认宏(STRICT=0,
 * 开发板行为不变)。STRICT=1 时无效区域在"首次配网"处拦截(见 netcfg_start 前)。*/
static tuya_auth_region_t g_tuya_triplet;
static int g_tuya_triplet_valid = 0;

static const char *tuya_trip_pk(void)   { return g_tuya_triplet_valid ? g_tuya_triplet.product_key : TUYA_PRODUCT_KEY; }
static const char *tuya_trip_uuid(void) { return g_tuya_triplet_valid ? g_tuya_triplet.uuid        : TUYA_UUID; }
static const char *tuya_trip_key(void)  { return g_tuya_triplet_valid ? g_tuya_triplet.auth_key    : TUYA_AUTH_KEY; }

#define TUYA_WAIT_MS    60000
#ifdef TUYA_CLOUD_OPEN_ENABLE
/* 纯云端开口试验(哑门+云端裁决)硬依赖:云端判停 + TCP 传输 + barge-in 预填
 * 机制(prefill 回溯复用)。不满足直接编译报错,防止静默配出错误组合。 */
#if !defined(TUYA_SERVER_VAD_ENABLE) || TUYA_TRANSPORT_STM_ENABLE || !defined(TUYA_BARGE_IN_ENABLE)
#error "TUYA_CLOUD_OPEN_ENABLE needs TUYA_SERVER_VAD_ENABLE + TCP(TUYA_TRANSPORT_STM_ENABLE=0) + TUYA_BARGE_IN_ENABLE"
#endif
#endif
#define TUYA_BARGE_COOLDOWN_MS  1000   /* barge-in 后冷却(ms):此窗口内忽略老轮 chat_break 后在途 TTS 残响引起的二次触发(竞态) */
/* 起轮单帧能量门:Σ|int16|/帧。仅用于空闲起轮(无 TTS,底噪低),100k 够用;
 * 真话音 onset 实测 110万~220万,回声/噪音远低于此。
 * (2026-09-17 噪音实验曾置 0=全交云端判定,实验后恢复原值。) */
#define BARGE_MIN_ENERGY        100000u
/* 播放中 barge-in 3帧确认门(每帧都要≥此值),TTS/排空/音乐停播共用。
 * 2026-09-04 深圳天气轮日志:
 * AEC 残留骗过 3/3 确认三次(各帧 sum 最高仅 38.4万)→ 天气播报被掐、碎片
 * 轮错乱;真人插话确认帧全部 ≥115.9万。取 60万:误触发余量 1.6×,真人余量
 * 1.9×。贴耳小声插话若失灵,降到 50万;TTS 仍被误掐则升到 80万。
 * (2026-09-17 噪音实验曾降 50万,实验后恢复原值。不能置 0:喇叭回声 3 帧
 * 即"确认",播放会自杀循环——播放期全开需云端回声判别,端侧无解。) */
#define BARGE_CONFIRM_ENERGY    600000u
/* 长流播放期回声闸的重关门限(app_config.h TUYA_STREAM_PLAYBACK_GATE,2026-09-24):
 * 开门后连续 25 帧(≈1s)sum 低于此值重新扣帧。取 40万=TTS 残差实测最坏 38.4万
 * 之上、真人确认帧(≥115.9万)之下的空隙;说话中 <1s 的自然停顿不会误重扣。 */
#define TUYA_PLAY_GATE_REGATE_ENERGY 400000u

/* ★ 自适应开门门限(2026-09-22 噪声专项):固定 4万 哑门按安静底噪(4k~1万)
 * 标定,稳态噪声下空闲底噪中位数 17.7万(10dB SNR)/31万(0dB)直接泡死门限
 * →9.22下午 26min 开出 96 个空轮(58%),49% 真话音落进空轮,而轮次制下云端
 * VAD 事件一轮只记一次、已在轮首被噪声花掉 →10dB 检出率钉死 72%(9.21 DNS
 * 开/9.22 DNS 关两轮同值,与降噪无关,纯轮次碰撞)。改为跟踪空闲底噪:
 *   gate = clip(TUYA_OPEN_ENERGY_MIN, K/1000×底噪中位数, CAP)
 * 底噪在 idle drain 处采样(TTS 期不入样防回声抬底);安静时门仍=4万,
 * 行为与固定门完全一致。20dB 底噪 2.3k→门 4万不变(9.22 实测 99.31% 不伤)。*/
#define TUYA_OPEN_GATE_K        1800u  /* 系数(千分比)=1.8×:10dB 底噪 17.7万→门 31.9万
                                        * (压过噪声 p75 27.5万,拦空轮);0dB 话音≈2×底噪
                                        * 临界可过,不为 0dB(已不可救,WER 70%)锁死全场 */
#define TUYA_OPEN_GATE_CAP     600000u /* 门上限:极端嘈杂下防把门抬到话音也开不了 */
#define TUYA_OPEN_GATE_NBAT        8u  /* 底噪样本环批数:8批×16帧≈5s,中位数抗话音突发 */

/* ★ 长流模式(TUYA_STREAM_MODE,开关见 app_config.h)无本地参数:会话建立即
 * 开流、永不主动收流(v3,2026-09-23 定稿,完全对齐小智)——收/开全由云端
 * 事件驱动,设备侧零信号判定、零计时器、零能量门。*/

extern const pal_t *tai_pal_ac791n(void);

typedef struct {
    volatile int got_done;
} demo_ctx_t;

/* === 阶段3 语音循环的跨线程状态 ============================================
 * on_audio / on_event / on_disconnect 在涂鸦 worker 线程触发并写这些标志;
 * tuya_ai_run 的主循环在 tuya_agentic 任务线程里读。单 int 读写本平台安全。=== */
static volatile int g_audio_ready;        /* 音频流已起:on_audio 才允许喂 DAC(否则独立文本 demo 未起流会踩空 cbuf)*/
static volatile int g_tts_playing;        /* 云端正在播 TTS:期间暂停上行,防麦克风采到自身喇叭 */
static volatile int g_turn_done;          /* 本轮回复结束(TAI_EVT_END),可重新听音 */
/* 2026-09-05"第一次放歌没反应"根因:barge-in 打断的旧轮,云端回复流被拖后收尾,
 * 旧轮的 TAI_EVT_END 在新轮 ④ 等待期到达 → g_turn_done 误置位 → 等待提前退出,
 * 新轮晚到的音乐 SKILL 无人消费。修复:用轮 id 关联——on_text 持续记录"正在
 * 应答的轮 id",起轮时快照它(=被打断旧轮的 id);on_event 的 END 命中快照
 * = 旧轮残留,丢弃不置 g_turn_done。event id 是 SDK 每轮新生成的,不会撞。*/
static char g_answer_bizid[48];           /* 最近见到的回复轮 id(on_text 回调线程写) */
static char g_stale_end_bizid[48];        /* 本轮起轮时 g_answer_bizid 的快照(语音循环线程写) */
/* 作废旧轮的 TTS 残包排空窗:chat_break 只作废"当前轮"(ctx 里最近一次 audio_start
 * 的 event-id),喇叭里正播的常是更早一轮的回话,云端不会停它——NLG/音频还会再流
 * 几秒,on_audio 在窗口内全部丢弃。唤醒打断处置位,新轮 audio_start 清零关闭。*/
static volatile unsigned int g_tts_drop_until; /* 排空窗截止时刻 ms(0=关) */
static volatile unsigned int g_tts_drop_cnt;   /* 窗口内丢弃的下行帧计数(诊断) */
static volatile int g_barge_in;           /* barge-in 触发:跳过 TTS 后清缓冲/冷却,直接进新一轮(TUYA_BARGE_IN_ENABLE)*/
static volatile int g_exit;               /* on_disconnect 置位:令本次会话的语音循环退出(supervisor 稍后重连) */
static volatile int g_link_broken;        /* 上行发送失败:链路已断,快速报废本次会话交 supervisor 重连(不等 60s 超时) */
#ifdef TUYA_UPLINK_OPUS_ENABLE
static int g_use_opus_uplink;             /* 上行 opus 编码器可用(会话入口 init 成功置 1):主循环与
                                          * music_handoff 共用——音乐期上行的编码形态必须与会话入口
                                          * audio_start 声明的 codec 一致,两处不能各自为政(2026-10-08) */
#endif
static volatile int g_mqtt_ka_run;        /* MQTT 心跳线程运行标志:生命周期=整个 tuya 流程,不跟 AI 会话共存亡 */
/* protocol 11 的 MQTT 回调只写此请求槽。首次请求锁定类型；重复帧不覆盖它，
 * 释放 iot/擦 VM/复位全部由 tuya_ai_run 这个唯一监督者完成。 */
#define TUYA_CLOUD_RESET_NONE (-1)
static volatile int s_cloud_reset_type = TUYA_CLOUD_RESET_NONE;
/* protocol 15(APP 确认升级)的回调同样只写请求槽(跑在 iot_client_process 的
 * MQTT 线程里,契约见 iot_client.h:只许置标志/发信号量,禁止调 OTA API)。
 * 心跳线程下一拍派发 worker 线程执行 查询→下载→校验→烧写;首个确认锁定通道,
 * worker 忙碌期间的重复确认帧不叠加(成功路径烧完反正 2s 内重启)。*/
static volatile int s_ota_confirm_pending;
static volatile int s_ota_confirm_channel;
static volatile int s_ota_worker_busy;
/* K6 runs in app_music's key context.  With a live iot client it only sets
 * this request; tuya_ai_run serialises the best-effort cloud notification and
 * the local reset state machine.  Before a client exists, K6 follows the
 * standalone local path so an offline boot cannot make factory reset unusable. */
static volatile int s_local_factory_reset_requested;
static volatile int s_tuya_cloud_client_ready;
static volatile int s_mqtt_ka_exited;
static int g_audio_frame_logged;          /* 下行首帧帧长只打印一次,供核对 opus_cbr_pktlen */
#ifdef TUYA_SERVER_VAD_ENABLE
static volatile int g_server_vad_stop;    /* 云端VAD(TAI_EVT_SERVER_VAD)通知停说:上行循环据此收尾。on_event 在 worker 线程置位,主循环读 */
#ifdef TUYA_STREAM_MODE
static volatile int g_cloud_break_evt;    /* 长流模式:云端判打断(chat_break 非回执)。on_event 置位,
                                           * 语音循环读后处理(回调线程不能碰音频服务)。
                                           * 2=打断时 TTS 确实在播(停喇叭+开残包排空窗);
                                           * 1=没在播(仅记录,不动任何东西——见 on_event 小智对齐注释)。 */
#endif
#endif
#ifdef TUYA_STREAM_PLAYBACK_GATE
/* 长流播放期回声闸状态机(开关见 app_config.h,方案2,2026-09-24;主循环有详注):
 * open    1=播放期已确认真人插话、实时上行中;0=扣帧只入 barge 历史不上云
 * confirm 连续 ≥60万 帧计数,3 帧开门(方案C 同门:残差最坏 38.4万/真人 ≥115.9万)
 * quiet   开门后连续 <40万 帧计数,25 帧(≈1s)重扣,防 TTS 尾音残差续漏
 * hold    播放期累计扣帧数(约 1s 一条遥测)
 * v2(2026-09-24 箱测自说自话定位后):
 * blind    起播盲窗剩余帧数(8 帧≈320ms,AEC 未收敛爆发期,丢帧不喂历史)
 * was_busy 上一帧播放判据值(侦测起播上升沿)
 * last_hi  上一帧是否 ≥60万(人声在谈则起播不盲,防剪快速对话的真人话头) */
static int s_play_gate_open;
static unsigned int s_play_gate_confirm, s_play_gate_quiet, s_play_gate_hold;
static unsigned int s_play_gate_blind, s_play_gate_was_busy, s_play_gate_last_hi;
#endif
/* —— 2026-09-21 全量测试两大杀轮根因的修复状态 ——
 * ① chat_break 回执对冲:本地 tai_chat_break(全部经 tuya_break_send)成功即
 *    计数;on_event 收到 CHAT_BREAK 先对冲,对冲干净才算云端判停。否则自己
 *    barge-in 的回执(0.3~3s 后到,常落在新轮上行中)被当判停——9.21 一天
 *    杀掉 523 轮,而全天真正的 SERVER_VAD 事件 = 0。
 * ② 上行窗 END 判属:g_uplinking 标记 audio_start 成功 → audio_end 发出之间的
 *    上行窗口;窗内 END 只有命中本轮自己的 event-id(g_turn_event_id)才生效
 *    (纯噪声轮的就地收尾,纯云停说设计依赖它),其余一律旧轮残留判弃——
 *    9.21 一天 813 轮死于 frames=1-9 的"cloud-end"。(旧 id-快照守卫在 barge
 *    链下系统性失效:回执自带旧轮 event-id,协议层锁存被改写,随后 TEXT 继承
 *    到错 id → g_answer_bizid 污染 → 快照存错,比对必不中;协议层已同步跳过
 *    CHAT_BREAK 的 latch 根治投毒,此处按上行窗结构性兜底。) */
static volatile int g_local_break_pend;    /* 已发出未收到的本地 chat_break 数(主循环写,worker 线程减,临界区内增减) */
static volatile int g_uplinking;           /* 上行窗口标志(主循环写,on_event 读) */
static char g_turn_event_id[64];           /* 本轮 event-id(audio_start 后读回;临界区内更新,on_event 上行窗判属用) */
/* 阶段2·服务器时间过滤双判(上游 65ce503):本轮 TTS START 帧的服务器媒体头
 * 时间戳(tai_audio_msg_t.timestamp_ms,on_audio worker 线程写)。chat_break 的
 * breakAttributes.time 与它同一时钟轴——≥本轮起点=打断属于本轮,<起点=旧轮
 * 在途回执。现阶段只打对照日志不改判属行为,板上确认与 event-id/对冲判属
 * 一致率后再收编为唯一判据(计划见 UPSTREAM-BASE.md §4 阶段2)。*/
static volatile unsigned long long g_turn_start_ts;
#if TUYA_TRANSPORT_STM_ENABLE && TUYA_STM_MCP_VIA_TEXT
/* STM 暂用 TEXT 承载 MCP response，云端会误把 JSON 当作一轮用户文本并生成
 * NLG/TTS。下一轮 AUDIO 前由 demo 任务 chat_break 隔离这轮污染。 */
static volatile unsigned s_mcp_text_reply_pending;
static volatile unsigned s_mcp_text_break_request;
static volatile unsigned s_mcp_text_break_sent;
static volatile unsigned s_mcp_text_reply_deadline;
#endif
static volatile unsigned int g_barge_cooldown_until;
#ifdef TUYA_CLOUD_OPEN_ENABLE
static volatile unsigned int g_open_cooldown_until; /* 纯云端开口冷却到期 ms(0=始终过期):上一轮收尾后短暂关开口窗,防 AEC 尾自激与空轮翻滚;barge-in 轮豁免 */
#endif /* barge-in 冷却到期 ms 时间戳;此前的 g_tts_playing 视为老轮在途残响,忽略(0=始终过期) */
/* —— 唤醒词全程在线(TuyaOpen 语义:tdd_audio 驱动层无条件喂 KWS,IDLE/LISTEN/
 *    UPLOAD/THINK 任何状态不关门)。JL 这边 KWS 帧来自各阶段的 mic 消费点:
 *    idle 排空 + 上行循环 + TTS 等待/排空循环 + 音乐等待循环,命中即视为
 *    "万能打断"。
 *    只在 KWS 的 on_wake 里置位;不加 ifdef:music_handoff/on_wake(MUSIC 路径)
 *    也要引用,KWS 关闭时恒 0 无副作用。*/
static volatile int g_wake_hit;             /* 本拍 KWS 命中,由当前所处阶段的循环就地处理 */
static volatile int g_wake_prompt_pending;  /* 播放中命中:提示音由打断序列停播后再播(不抢 DAC) */
static volatile int g_wake_break;           /* 唤醒打断了本轮:④ 的 TTS 排空收尾与 pending 音乐跳过 */
#ifdef TUYA_MUSIC_ENABLE
static volatile int g_music_handoff;        /* 音乐停乐→TTS 播放器恢复之间的 DAC 交接窗:
                                             * 此窗口命中唤醒,提示音延后到恢复后再播 */
/* 音乐被能量抢答停掉后的"哑火"补救:音乐+人声经 AEC 双讲削损后,引擎对唤醒词
 * 连一个 token 都提不出来(实测探针零命中,安静时 k27=0.886)——抢答停了音乐、
 * 没提示音没回复,用户只会觉得"第一次喊没反应"。停乐后若 2.5s 内没有命令起轮
 * 且 VAD 已关(=刚才那声其实是唤醒词),补播提示音并打开 15s 追问窗。*/
#define TUYA_BARGE_PROMPT_WAIT_MS 2500u
static volatile int g_barge_prompt_wait;
static volatile unsigned g_barge_prompt_deadline;
static volatile unsigned g_last_wake_alert; /* 最近一次播"我在"的时刻(on_wake 与哑火补救
                                             * 共用去重窗):哑火刚播完、引擎随后把词迟到
                                             * 补认出来时,on_wake 不再重播第二个提示音 */
/* 唤醒词在抢答停乐/停TTS 过程中被识别(TuyaOpen wakeup 语义):on_wake 已置
 * pending/吞咽,但抢答路径已把含唤醒词的历史装进 prefill——那轮上行只会让
 * 云端听到"嘿tuya"并回一句废话(2026-09-05 实测 ASR"对嗯"→云端回"好的",
 * 用户听到提示音和云端回复混着来)。置位后主循环作废带 prefill 的抢答轮。
 * 10s 时限:过期后的抢答轮是全新事件,不受上一次唤醒影响。*/
#define TUYA_WAKE_SUPPRESS_BARGE_MS 10000u
static volatile int g_wake_suppress_barge;
static volatile unsigned g_wake_suppress_barge_until;
/* 先判后恢复:music_handoff 里要播提示音/判抢答后续的路径,不再先恢复 TTS 播放器
 * (先恢复、马上又要停它换 mp3,白折腾 ~0.4s——音乐唤醒应答慢的主因),置位把
 * 恢复推迟到下一次起轮时(主循环 g_turn_done=0 前):回话 TTS 至少 ~1.5s 后才
 * 到,必定赶得上;提示音播 mp3 期间也不开 opus 抢 DAC。正常播完/下载失败路径
 * 仍当场恢复。*/
static volatile int g_player_restore_pending;
#endif
#ifdef TUYA_MUSIC_ENABLE
static volatile int g_music_playing;   /* app_music 网络音乐播放中:期间不上行(音乐回采会假触发 VAD),播完回听音 */
/* dec_end 回调(app_music 解码事件上下文触发):整首播完/下载解码出错停机。
 * 只清标志——不在此碰播放器(跨线程),TTS 播放器的恢复由语音循环侧做。*/
static void tuya_music_dec_end_cb(int arg) { (void)arg; g_music_playing = 0; }
#endif
#ifdef TUYA_KWS_ENABLE
#include "tuya_kws.h"   /* 唤醒词("嘿tuya")引擎薄封装: init/feed/唤醒窗门控 */
#endif
extern unsigned int timer_get_ms(void);   /* system/timer.h,单调 ms */
/* barge-in 冷却是否已过:过期=可受理新 g_tts_playing 信号(云端真回话/真打断);
 * 未过期=老轮 chat_break 后在途 TTS 残响,忽略以免二次触发(竞态)。
 * (int)差值比较处理回绕;g_barge_cooldown_until=0 时始终过期(非 barge 轮/初始态)。*/
static int barge_cooldown_expired(void) { return (int)(timer_get_ms() - g_barge_cooldown_until) >= 0; }

/* ------------------------------------------------------------------------- */
/* 极简 JSON 助手(搬自 text_chat_demo.c,不依赖 cJSON)                      */
/* ------------------------------------------------------------------------- */
static const char *json_find_value(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return p;
}
static int json_get_string(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = json_find_value(json, key);
    if (!p || *p != '\"') return -1;
    p++;
    const char *end = strchr(p, '\"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}
static char *json_get_object_raw(const char *json, const char *key)
{
    const char *p = json_find_value(json, key);
    if (!p || *p != '{') return NULL;
    int depth = 0;
    const char *start = p, *q = p;
    while (*q) {
        if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) {
            size_t len = (size_t)(q - start + 1);
            char *obj = (char *)malloc(len + 1);
            if (obj) { memcpy(obj, start, len); obj[len] = '\0'; }
            return obj;
        }
        q++;
    }
    return NULL;
}
static int json_array_first_string(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = json_find_value(json, key);
    if (!p || *p != '[') return -1;
    p++;
    while (*p == ' ') p++;
    if (*p != '\"') return -1;
    p++;
    const char *end = strchr(p, '\"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}
/* json_find_value 返回点起的极简 u64 解析(容忍前导引号/空白;非数字/空=0)。
 * 服务端时间戳 13 位毫秒必须 64 位;不走 strtoull/printf %ll——demo.c:3600 注释
 * 明言杰理 newlib 对 64 位格式符支持存疑(tai_pkt_log.c 的 %llu 仅活在日志级
 * 门控后),对照日志用 %x 半字对打印,彻底避开。*/
static unsigned long long json_u64_after(const char *p)
{
    unsigned long long v = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == '\"' || *p == '\t') p++;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned long long)(*p - '0');
        p++;
    }
    return v;
}

/* ------------------------------------------------------------------------- */
/* base64 + token 解析(搬自 text_chat_demo.c)                               */
/* ------------------------------------------------------------------------- */
static char *b64_decode(const char *encoded, size_t *out_len)
{
    size_t elen = strlen(encoded);
    size_t dlen = 0;
    if (mbedtls_base64_decode(NULL, 0, &dlen,
                               (const unsigned char *)encoded, elen)
            != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
        return NULL;
    char *out = (char *)malloc(dlen + 1);
    if (!out) return NULL;
    if (mbedtls_base64_decode((unsigned char *)out, dlen, &dlen,
                               (const unsigned char *)encoded, elen) != 0) {
        free(out);
        return NULL;
    }
    out[dlen] = '\0';
    if (out_len) *out_len = dlen;
    return out;
}

typedef struct {
    char     host[256];
    char     tls_sni[256];
    char     derived_client_id[256];
    char     agent_token[256];
    uint16_t port;
    long     biz_code;
    long     biz_tag;
} tai_conn_params_t;

static int parse_token(const char *raw_token, tai_conn_params_t *p)
{
    memset(p, 0, sizeof(*p));
    char *json = NULL;
    size_t dl = 0;
    char *decoded = b64_decode(raw_token, &dl);
    if (decoded && dl > 0 && decoded[0] == '{') {
        json = decoded;
    } else {
        free(decoded);
        json = strdup(raw_token);
    }
    if (!json) return -1;

    char *conn = json_get_object_raw(json, "connect_conf");
    if (!conn) { free(json); return -1; }
    json_array_first_string(conn, "hosts", p->host, sizeof(p->host));
    if (json_array_first_string(conn, "domains", p->tls_sni, sizeof(p->tls_sni)) != 0)
        strncpy(p->tls_sni, p->host, sizeof(p->tls_sni) - 1);

    const char *pp = json_find_value(conn, "ecc_tls_port");
    long port = pp ? strtol(pp, NULL, 10) : 0;
    p->port = (port > 0) ? (uint16_t)port : 443;

    json_get_string(conn, "derived_client_id", p->derived_client_id, sizeof(p->derived_client_id));
    free(conn);

    char *sess = json_get_object_raw(json, "session_conf");
    if (sess) {
        json_get_string(sess, "agentToken", p->agent_token, sizeof(p->agent_token));
        char *biz = json_get_object_raw(sess, "bizConfig");
        if (biz) {
            const char *bc = json_find_value(biz, "bizCode");
            const char *bt = json_find_value(biz, "bizTag");
            if (bc) p->biz_code = strtol(bc, NULL, 10);
            if (bt) p->biz_tag  = strtol(bt, NULL, 10);
            free(biz);
        }
        free(sess);
    }
    free(json);

    if (p->host[0] == '\0') return -1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* TAI 回调(均在 SDK worker 线程触发)                                       */
/* ------------------------------------------------------------------------- */
/* MQTT 下行消息回调(MQTT 常驻后收 DP/控制命令)。
 * 涂鸦平台下发的 DP 在这里收到(topic=smart/device/in/{devid},data 是加密后的 JSON)。
 * agentic-kit 的 iot_client 已解密,data 是明文 JSON。打印完整内容看 DP 值。*/
static void on_mqtt_message(const char *topic, size_t topic_len,
                            const uint8_t *data, size_t data_len)
{
    printf("[TUYA-MQTT] topic=%.*s len=%u: %.*s\r\n",
           (int)(topic_len < 64 ? topic_len : 64), topic,
           (unsigned)data_len,
           (int)(data_len < 512 ? data_len : 512),
           data ? (const char *)data : "(null)");
}

/* 阶段2·9000 MQTT AI 控制通道(上游 387957b):服务端 AI 事件(asrInterrupt 等)
 * 经 MQTT 带外下发,分发链 reset→ota_confirm→9000→DP→raw(iot_client_message.c)。
 * 与 TAI TCP 的 CHAT_BREAK/breakAttributes 双通道并行观察打断时序,回调内只打
 * 日志不动状态——type/json_data 均为 borrowed、回调须非阻塞(iot_client.h 契约)。
 * 板上对照一致率确认后,再决定打断以哪条通道为准(计划阶段2 收编判据)。*/
static void on_ai_ctrl(const char *type, const char *json_data,
                       size_t data_len, void *user_data)
{
    (void)user_data;
    printf("[TUYA-AI-CTRL] type=%s data=%.*s\r\n",
           type ? type : "(null)",
           (int)(data_len < 256 ? data_len : 256),
           json_data ? json_data : "");
}

extern u32 wifi_get_tuya_network_ready_generation(void);
extern int wifi_tuya_network_is_ready(void);

/* MQTT 心跳维持线程(MQTT 常驻模式)。
 * 断线自愈:iot_client_process 失败持续超过 TUYA_MQTT_DEAD_WINDOW_MS 才判定连接
 * 死亡,销毁旧句柄(防泄漏,try_connect 直接覆盖指针不 free)后重连;重连退避
 * 5s→10s→…→60s 封顶,成功后自然复位。WiFi 掉线由 SDK 层自动重连
 * (WIFI_EVENT_STA_DISCONNECT → NET_EVENT_DISCONNECTED_AND_REQ_CONNECT →
 * wifi_return_sta_mode),这里只管 MQTT 层。
 * 轮询周期 1 tick(100Hz tick 即 10ms):云端 DP(音量等控制指令)只由本线程收,
 * 休眠过长会把下发延迟拉到秒级(如音量 DP 晚到 TTS 播完之后)。空闲时
 * transport_recv 本就阻塞在 recv(超时 MQTT_RECV_TIMEOUT_MS=1s),缩短休眠只是
 * 每 ~1s 多醒一次,CPU 基本不变。*/
#define TUYA_MQTT_DEAD_WINDOW_MS 10000u  /* 失败持续超该时长才判死:滤掉毫秒级瞬断(原按"连续2轮"计数,轮询提速后两轮仅隔20ms会误杀) */
static void on_cloud_reset(iot_reset_type_t type, void *user_data)
{
    (void)user_data;
    if (type != IOT_RESET_REMOTE_UNBIND && type != IOT_RESET_REMOTE_FACTORY) {
        return;
    }

    /* This runs inside iot_client_process().  It deliberately performs no
     * allocation, VM I/O, disconnect, deinit or reboot. */
    if (s_cloud_reset_type == TUYA_CLOUD_RESET_NONE) {
        s_cloud_reset_type = (int)type;
        printf("[TUYA] cloud reset queued: %s\r\n",
               type == IOT_RESET_REMOTE_FACTORY ? "factory" : "unbind");
    } else {
        printf("[TUYA] duplicate cloud reset ignored; keeping %s request\r\n",
               s_cloud_reset_type == IOT_RESET_REMOTE_FACTORY ? "factory" : "unbind");
    }
    g_exit = 1;
}

/* APP 确认升级回调(MQTT protocol 15,iot_client_process 线程):
 * 只入队请求(与云复位槽同款策略:首个确认锁定通道,重复帧不覆盖);
 * 真正 查询/下载/烧写 在 tuya_mqtt_keepalive_task 派生的
 * tuya_ota_confirm_task worker 里做,绝不在这里做(会卡死 MQTT 泵)。*/
static void on_ota_confirm(int channel, void *user_data)
{
    (void)user_data;
    if (!s_ota_confirm_pending) {
        s_ota_confirm_channel = channel;
        s_ota_confirm_pending = 1;
    }
    printf("[TUYA] ota confirm queued: app confirmed upgrade (channel=%d)\r\n", channel);
}

/* APP 确认升级 worker:独立线程跑 tuya_ota_check_and_upgrade_channel
 * (查询→下载→校验→烧写),不占 MQTT 泵,下载期间心跳照常。成功路径烧完
 * boot info 后 2s 自动重启,worker 打印一句就退出;失败上报 ERROR 后设备
 * 照常跑。与 K6/云复位监督者的并发窗口:若复位恰在下载中到达,supervisor
 * 停 MQTT 后 deinit client 可能与本线程踩踏——板级验证阶段接受(点完 App
 * 确认后几秒内不按 K6 即可),要严格串行时在 supervisor 停 MQTT 前加对本
 * 线程的等待。栈 8KB 与 tuya_ota_chk 同级(HTTPS/TLS 下载实测够用)。*/
static void tuya_ota_confirm_task(void *arg)
{
    extern int tuya_ota_check_and_upgrade_channel(iot_client_t *client, int channel);
    iot_client_t *iot = (iot_client_t *)arg;
    int chan = s_ota_confirm_channel;
    int ret = tuya_ota_check_and_upgrade_channel(iot, chan);
    printf("[TUYA] ota confirm worker end (channel=%d ret=%d)%s\r\n", chan, ret,
           ret == 1 ? ", wait reboot" : ", continue");
    s_ota_worker_busy = 0;
}

static void tuya_mqtt_keepalive_task(void *arg)
{
    extern int  iot_client_message_connect(iot_client_t *client);    /* src/iot_client_message.h 未进公共头 */
    extern void iot_client_message_disconnect(iot_client_t *client);
    iot_client_t *iot = (iot_client_t *)arg;
    if (!iot) {
        s_mqtt_ka_exited = 1;
        return;
    }
    int failing = 0;              /* 连续失败中标记(成功清零),配合 fail_since_ms 计失败持续时长 */
    unsigned int fail_since_ms = 0;
    int offline_logged = 0;
    int mqtt_disconnected = 0;
    while (g_mqtt_ka_run) {
        /* No DHCP lease means there is no usable IP link.  Do not turn a
         * Wi-Fi association failure into repeated TLS/TCP attempts. */
        if (!wifi_tuya_network_is_ready()) {
            if (!offline_logged) {
                printf("[TUYA] network offline, cloud reconnect paused\r\n");
                offline_logged = 1;
            }
            if (!mqtt_disconnected) {
                iot_client_message_disconnect(iot);
                mqtt_disconnected = 1;
            }
            failing = 0;
            os_time_dly(100);
            continue;
        }
        if (offline_logged) {
            printf("[TUYA] network restored, resuming cloud reconnect\r\n");
            offline_logged = 0;
        }
        mqtt_disconnected = 0;
        int ret = iot_client_process(iot, 0);
        if (ret == 0) {
            failing = 0;
        } else if (!failing) {
            failing = 1;         /* 单次失败可能只是瞬断,先记起点等下一轮确认 */
            fail_since_ms = sys_timer_get_ms();
        } else if (sys_timer_get_ms() - fail_since_ms >= TUYA_MQTT_DEAD_WINDOW_MS) {
            printf("[TUYA] mqtt dead (ret=%d), reconnect...\r\n", ret);
            iot_client_message_disconnect(iot);
            unsigned int backoff = 5000;
            while (g_mqtt_ka_run && wifi_tuya_network_is_ready() &&
                   iot_client_message_connect(iot) != 0) {
                printf("[TUYA] mqtt reconnect fail, retry in %ums\r\n", backoff);
                for (unsigned int waited_ms = 0;
                     waited_ms < backoff && g_mqtt_ka_run &&
                     wifi_tuya_network_is_ready();
                     waited_ms += 100) {
                    msleep(100);
                }
                if (backoff < 60000) backoff *= 2;
            }
            if (g_mqtt_ka_run && wifi_tuya_network_is_ready()) {
                printf("[TUYA] mqtt reconnected\r\n");
            }
            failing = 0;
        }

        /* APP 确认升级派发:protocol 15 回调已入队,这里派 worker 执行
         * (云复位请求挂起时不派——supervisor 马上要停本线程并 deinit client,
         * 此时再 fork worker 只会加大踩踏窗口;worker 忙碌时新确认帧留队)。*/
        if (s_ota_confirm_pending && !s_ota_worker_busy &&
            s_cloud_reset_type == TUYA_CLOUD_RESET_NONE) {
            s_ota_confirm_pending = 0;
            s_ota_worker_busy = 1;
            printf("[TUYA] ota confirm: dispatch upgrade worker (channel=%d)\r\n",
                   s_ota_confirm_channel);
            if (thread_fork("tuya_ota_cf", 4, 8 * 1024, 0, 0,
                            tuya_ota_confirm_task, iot) != 0) {
                printf("[TUYA] ota confirm worker fork failed\r\n");
                s_ota_worker_busy = 0;
            }
        }
        os_time_dly(1);
    }
    s_mqtt_ka_exited = 1;
}

/* DP 下行回调:云端下发 DP 值时触发(如说"音量调到20"→云端识别后下发 DP)。
 * dp_id:涂鸦平台配的 DP 点编号(如 102)
 * value:DP 值(按 type 区分 bool/int/string/enum/raw)。
 * 指针仅在回调期间有效,需要保留请拷贝。*/
#include "iot_dp.h"
void on_dp_downlink(uint8_t dp_id, const iot_dp_value_t *value, void *user_data)
{
    (void)user_data;
    if (!value) { printf("[TUYA-DP] dp=%u (null value)\r\n", dp_id); return; }
    switch (value->type) {
    case IOT_DP_TYPE_BOOL:
        printf("[TUYA-DP] dp=%u bool=%d\r\n", dp_id, value->value.boolean ? 1 : 0);
        break;
    case IOT_DP_TYPE_VALUE:
        printf("[TUYA-DP] dp=%u value=%d\r\n", dp_id, value->value.integer);
        break;
    case IOT_DP_TYPE_STRING:
        printf("[TUYA-DP] dp=%u string=%s\r\n", dp_id, value->value.string ? value->value.string : "(null)");
        break;
    case IOT_DP_TYPE_ENUM:
        printf("[TUYA-DP] dp=%u enum=%d\r\n", dp_id, value->value.enum_index);
        break;
    case IOT_DP_TYPE_RAW:
        printf("[TUYA-DP] dp=%u raw len=%u\r\n", dp_id, (unsigned)value->value.raw.len);
        break;
    default:
        printf("[TUYA-DP] dp=%u type=%d(unknown)\r\n", dp_id, value->type);
        break;
    }
}

/* DP schema 更新回调:从云端拉到 DP schema 时触发。
 * schema_id:版本标识(用于增量查询)
 * new_schema:DP schema JSON(含 DP id/类型/范围等,设备侧据此解析 DP 下发)*/
void on_dp_schema_update(const char *schema_id, const char *new_schema, void *user_data)
{
    (void)user_data;
    printf("[TUYA-DP] schema updated: id=%s\r\n", schema_id ? schema_id : "(null)");
    if (new_schema) {
        printf("[TUYA-DP] schema: %.512s\r\n", new_schema);  /* 最多打 512 字节 */
    }
}

/* ------------------------------------------------------------------------- */
static void on_text(tai_ctx_t *ctx, const tai_text_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    /* 记录当前回复轮 id(文本各分片的 event_id 一致 = 本轮 event-id):
     * on_event 据此识别"被打断旧轮"的残留 END(见 g_stale_end_bizid 注释)。*/
    if (msg->event_id && msg->event_id[0]) {
        strncpy(g_answer_bizid, msg->event_id, sizeof(g_answer_bizid) - 1);
        g_answer_bizid[sizeof(g_answer_bizid) - 1] = '\0';
    }
    printf("[TUYA-AI] %.*s\r\n", (int)msg->len, msg->text);
#ifdef TUYA_MUSIC_ENABLE
    /* 并行重组文本流:音乐 SKILL 是一份可跨分片的完整 JSON,拼完才能解析
     * (msg->text 无 '\0' 结尾,tuya_music_text_accum 内部先按 len 拷贝)。
     * NLG 流重组后不是音乐响应,自然丢弃(逐片打印已在上面完成)。*/
    tuya_music_text_accum((int)msg->stream_flag, msg->text, msg->len);
#endif
}
static void on_audio(tai_ctx_t *ctx, const tai_audio_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    /* 独立文本 demo 未起音频流 / 空帧:跳过,避免往未初始化的 cbuf 写 */
    if (!g_audio_ready || !msg || !msg->len) {
        return;
    }
#if TUYA_TRANSPORT_STM_ENABLE && TUYA_STM_MCP_VIA_TEXT
    if (s_mcp_text_reply_pending) {
        /* 这次下行属于启动时的 MCP/TEXT 伪聊天轮；标记后丢弃其全部音频。
         * 实测该轮 TTS 在 chat_break 之后约 1.2s 才陆续到达，固定排空窗口
         * 关早了会把污染播报漏给喇叭——每收到一帧就顺延 1.5s，静音 1.5s
         * 后主循环才放行。demo 任务在下一次循环安全调用 chat_break。 */
        s_mcp_text_break_request = 1;
        s_mcp_text_reply_deadline = timer_get_ms() + 1500;
        _device_rbuf_clear();
        g_tts_playing = 0;
        return;
    }
#endif
    /* 唤醒/打断作废轮的 TTS 残包排空窗(仿上方 MCP 排空窗):2026-09-05 实测,
     * 故事 TTS 中唤醒成功、播完"我在"后,旧轮回话的尾巴又续播 ~5s("…你平时
     * 也喜欢涂鸦画画吗?")——chat_break 作废的是刚起的空轮,正在播的那轮云端
     * 不停。窗口内下行帧全丢,每丢一帧顺延 1.5s,静默 1.5s 自然关窗;新轮
     * audio_start 处显式清零,新轮回话不受影响。
     * ★ 2026-09-24 修(长流模式):新流的 START 帧=新轮回话,绝不可能是旧轮残包,
     *   落在丢弃窗内也不能丢(实测 ASR+LLM ~1.3s 就回话,撞进 3s 窗;且丢帧顺延
     *   1.5s 会把窗口越续越长,整段回答被吃光——"问了没声音"第二轮根因)。
     *   START 帧直接关窗走正常路径。*/
    if (msg->stream_flag == TAI_STREAM_START) {
        g_tts_drop_until = 0;
    }
    if ((int)(timer_get_ms() - g_tts_drop_until) < 0) {
        if ((g_tts_drop_cnt & 31u) == 0) {
            printf("[TUYA] drop stale TTS of broken turn (cnt=%u..)\r\n", g_tts_drop_cnt);
        }
        g_tts_drop_cnt++;
        g_tts_drop_until = timer_get_ms() + 1500;   /* 残包还会来:顺延排空窗 */
        _device_rbuf_clear();
        g_tts_playing = 0;
        return;
    }
    /* 任意下行帧到达即视为 TTS 正在播放;END 显式清(TAI_EVT_END 再兜底清一次) */
    g_tts_playing = (msg->stream_flag == TAI_STREAM_END) ? 0 : 1;

    /* 本轮 TTS START 帧:清播放 cbuf(上一轮残留)+ 触发预蓄水(首字抗卡顿)。
     * 预蓄水:本轮首次 fread 会等 cbuf 攒够 TTS_PREBUFFER_BYTES 再喂解码器,
     * 避免首帧 80B 直接播→40ms underrun 卡顿。START 才触发,后续帧不动。*/
    if (msg->stream_flag == TAI_STREAM_START) {
        g_turn_start_ts = msg->timestamp_ms;   /* 阶段2双判:本轮起点(服务器时钟轴,见 g_turn_start_ts 注释) */
        _device_rbuf_clear();
        tts_prebuffer_arm();
    }
    if (g_audio_frame_logged < 5) {
        /* 打印每会话前 5 帧:opus 模式核对 CBR pktlen;PCM 模式核对云端确实发 codec=101/16k */
        printf("[TUYA-AI] on_audio #%d: len=%d codec=%u sr=%u frame_ms=%u stream=%u\r\n",
               g_audio_frame_logged + 1, (int)msg->len, (unsigned)msg->codec,
               (unsigned)msg->sample_rate, (unsigned)msg->frame_duration,
               (unsigned)msg->stream_flag);
        g_audio_frame_logged++;
    }
    _device_write_voice_data((void *)msg->data, msg->len);
}
/* ------------------------------------------------------------------------- */
/* MCP response dispatch                                                     */
/* ------------------------------------------------------------------------- */
/* on_event runs in the Agentic transport worker.
 * It only queues MCP work; the existing session task calls this non-blocking
 * service hook at its normal scheduling points.  No MCP keep-alive task or
 * wait loop is created. */
static void mcp_resp_pump(tai_ctx_t *ctx)
{
    int sent = tuya_mcp_pump(ctx);

#if TUYA_TRANSPORT_STM_ENABLE && TUYA_STM_MCP_VIA_TEXT
    if (sent > 0) {
        s_mcp_text_reply_pending = 1;
        s_mcp_text_break_request = 0;
        s_mcp_text_break_sent = 0;
        s_mcp_text_reply_deadline = timer_get_ms() + 5000;
    }
#else
    (void)sent;
#endif
}

#define TUYA_OPUS_FRAME_LEN 1280   /* PCM 16k/16bit/mono 40ms = 1280B/帧(原 opus 180B 改 PCM) */
static void opus_frame_stat(const unsigned char *p, int len,    /* 定义在后,先声明给 music_handoff 用 */
                            unsigned int *out_sum, unsigned int *out_act);

/* ---- barge-in 话音历史:排空循环滚动缓存最近 ~1.8s 已消费帧 ----
 * TTS/音乐播放期间循环必须持续读帧防 cbuf 溢出(只喂 KWS/丢弃),而能量
 * barge-in 确认只要 3 帧(~120ms)——用户从开口到确认之间的命令头部全丢在
 * 循环里。2026-09-05 实测:音乐中喊"给我放一首周杰伦的歌",云端只收到尾部
 * "伦的歌。"。barge-in 命中时把历史线性化进 g_barge_prebuf 作上行 prefill
 * 补发,云端才能听到完整命令。push 每帧 memmove 57KB@25Hz≈1.4MB/s,可承受;
 * 线性化只在 barge 命中(罕见)时执行一次。
 * 2026-09-20 起idle 排空同样喂历史:空闲起轮的 VAD 判定窗(~100ms)话音头
 * 也靠它留底回溯,治上行音频首字缺声母。*/
#define BARGE_HIST_FRAMES   45    /* 45*40ms=1.8s 滚动窗口 */
#define BARGE_PREFILL_CAP   20    /* hist→prefill 帧数上限(20*40ms=800ms,含确认3帧+话音onset)。
                                   * 2026-09-17 晚两轮实测:纯 cap12 时"你好涂鸦"8 次中 5 次丢字
                                   * (丢"好"×3——抢在 TTS 刚响就插话,onset 落在回声区深处,
                                   * 12 帧窗够不着)。改为能量回溯+上限 20:回声被能量闸切断,
                                   * 话音 onset 保住。⚠ 音乐期回采 AEC 未消(实测 sum 可达
                                   * 77万),音乐打断轮回溯会走满 20 帧上限,等同无能量闸
                                   * (仍好于旧的整段 45 帧)。*/
#define BARGE_PREFILL_ENERGY 80000u /* 能量回溯门:Σ|int16|/帧。TTS 期自身播报的 AEC 残留
                                   * 实测 sum≤6.5万(常态<1万),取 8万 切断;正常说话 onset
                                   * ≥10万。仅作用于历史补发段,不碰实时上行。*/
#define BARGE_PREBUF_FRAMES 60    /* prebuf 容量:cap20+确认3+裕量 */
static unsigned char g_barge_prebuf[1280 * BARGE_PREBUF_FRAMES] __attribute__((aligned(4)));   /* 转 short* 进 opus 编码,align 1 全局无偶地址保证,同 abuf */
static unsigned int g_barge_prefill;
static unsigned char s_barge_hist[1280 * BARGE_HIST_FRAMES] __attribute__((aligned(4)));
static unsigned int  s_barge_hist_cnt;    /* 有效帧数(≤45, newest 在尾部) */

/* 排空循环(idle/TTS/音乐)每读一帧调一次:进历史环形窗(满则挤掉最旧) */
static void barge_hist_push(const unsigned char *frame)
{
    if (s_barge_hist_cnt < BARGE_HIST_FRAMES) {
        memcpy(s_barge_hist + s_barge_hist_cnt * 1280, frame, 1280);
        s_barge_hist_cnt++;
    } else {
        memmove(s_barge_hist, s_barge_hist + 1280, (BARGE_HIST_FRAMES - 1) * 1280);
        memcpy(s_barge_hist + (BARGE_HIST_FRAMES - 1) * 1280, frame, 1280);
    }
}

/* 历史最新段(旧→新)作为上行 prefill,接在 prebuf 已有帧(确认帧)之后,复位历史。
 * 调用点:barge-in 确认命中后、停乐排空后、idle 起轮能量门过门后——下一轮上行
 * 把它们补在最前。
 * 取法(2026-09-17 晚 B 方案):窗口=最新 BARGE_PREFILL_CAP 帧,从窗口最旧帧起
 * 逐帧砍掉头部的低能量帧(<BARGE_PREFILL_ENERGY,回声/静音),首个达话音能量的
 * 帧起整段保留(话音中间的短暂停顿不切断)。确认 3 帧恒≥50万在尾部,不会被砍光。*/
static void barge_hist_to_prefill(void)
{
    unsigned int cnt = s_barge_hist_cnt, take, sum, act;

    if (cnt == 0) {
        return;
    }
    take = (cnt > BARGE_PREFILL_CAP) ? BARGE_PREFILL_CAP : cnt;
    while (take > 3) {                 /* 尾部 3 帧确认帧永不为 0,循环不掏空 */
        opus_frame_stat(s_barge_hist + (cnt - take) * 1280, 1280, &sum, &act);
        if (sum >= BARGE_PREFILL_ENERGY) {
            break;                     /* 碰到话音能量:从这里起整段保留 */
        }
        take--;                        /* 窗口头部是回声/静音:丢弃继续砍 */
    }
    memmove(g_barge_prebuf + take * 1280, g_barge_prebuf,
            g_barge_prefill * 1280);
    memcpy(g_barge_prebuf, s_barge_hist + (cnt - take) * 1280, take * 1280);
    g_barge_prefill += take;
    s_barge_hist_cnt = 0;
    printf("[TUYA] barge history -> prefill %u frames (hist %u, echo dropped %u)\r\n",
           g_barge_prefill, cnt, cnt - take);
}

/* barge-in 确认命中/idle 起轮过门后的收尾:确认帧已在 barge_in_energy_confirmed
 * 读帧时喂 KWS+入历史、idle 门帧在能量门处入历史(成功/失败都进,保语音流连续),
 * 这里只剩历史→prefill 的线性化。*/
static void barge_in_prefill_arm(void)
{
    barge_hist_to_prefill();
}

/* 抢答停播后判断用户是否还在继续说(命令)还是只说了句短话(多半是没识别出的
 * 唤醒词):VAD 连续开 ≥300ms=真在说→正常起 prefill 轮;连续关 ≥350ms=说完了
 * →丢弃该轮交给哑火补救播"我在"(上行只会让云端收到"哎嗯"式残响回"你怎么
 * 了呀",2026-09-05 22:29 实测)。closed_since 传入进入前"已连续关闭"的起点
 * (0=刚还开着):停播→判定前的排空/等 DAC 耗时也计入连续关闭时长,无后续
 * 语音时很快即可判出;1.5s 上限兜底。
 * ★ VAD 开着不能立刻判"在说"(23:49 实测:词说完后 VAD 挂账 ~250-500ms,
 * 首拍采样撞上挂账尾→误判"有后续"→起轮等不来下文,既无提示音也无哑火,
 * 用户喊了没反应)。挂账活不过 300ms 连续开,真命令则一直开——持久性一测
 * 便知。真命令首拍起 300ms 后放行,多付的延迟命令语音自己就盖过去了。*/
static int barge_followup_wait(unsigned int closed_since)
{
    unsigned int t0 = timer_get_ms();
    unsigned int open_since = 0;
    while (!g_exit) {
        unsigned int now = timer_get_ms();
        if (get_recoder_state()) {
            closed_since = 0;
            if (!open_since) {
                open_since = now;
            }
            if (now - open_since >= 300u) {
                return 1;
            }
        } else {
            open_since = 0;
            if (!closed_since) {
                closed_since = now;
            }
            if (now - closed_since >= 350u) {
                return 0;
            }
        }
        if (now - t0 >= 1500u) {
            return 0;
        }
        msleep(20);
    }
    return 0;
}

#ifdef TUYA_MUSIC_ENABLE
#ifdef TUYA_UPLINK_OPUS_ENABLE
static int tuya_uplink_send_frame(tai_ctx_t *ctx, unsigned char *pcm1280);  /* 实现在本文件下方:
                                                                            * 音乐期上行(2026-10-08)在定义点之前调用 */
#endif
/* ------------------------------------------------------------------------- */
/* ⑤ 音乐技能交接:本轮云端回了音乐 SKILL(试听 mp3 URL,解析见 tuya_music.c)。
 * TTS 排空把"正在为您播放…"播完 → 停我们的 TTS 解码器让出 DAC → 交给
 * app_music 网络解码(net_download,https 自动 TLS)→ 等整首播完
 * (dec_end 回调清 g_music_playing)→ 重启 TTS 播放器,回 ① 继续听音。
 * ★ 2026-10-08 纯云形态(用户拍板"端侧把声音都交给云端,不考虑成本"):
 *   播放期间每帧照常上行(与主循环同形态),停乐由云端裁决、设备只执行——
 *   两臂信号:chat_break(g_cloud_break_evt)或云端直接开答新轮(g_tts_playing,
 *   主信号:音乐不在云端轮生命周期内,云端多半不吐 chat_break 直接开答)。
 *   旧本地门(VAD+能量 3 帧确认)10.8 声学箱证伪:0/10dB 噪声底 100万~490万
 *   > 门槛 60万,5 次"打断"4 次假;降级为 A/B 开关(TUYA_MUSIC_LOCAL_BARGE,
 *   app_config.h,默认关)。每秒 [MUSIC-DBG] 能量基线保留=对照云端裁决的数据。
 *   唤醒词停乐保留(KWS 全程在线同一循环喂帧;本配置未启用)。
 * ★ DAC 交接前后各等 300ms:两个解码器共享 DAC,边停边开会踩到
 *   subdevice_dac 的格式重配断言(2026-08-27 提示音教训)。
 * 抽成函数有两个调用点:正常=轮末(④ TTS 排空后);兜底=idle 分支——旧轮残留
 * END 把等待提前骗退/回包晚到时音乐已挂起却无人接管(2026-09-05 修复)。*/
static void music_handoff(tai_ctx_t *ctx)
{
    if (tuya_music_pending() && !g_link_broken) {
        /* 新一场音乐=全新上下文:清掉上一场遗留的"唤醒作废抢答轮"标记,
         * 否则它(10s 时限内)会让本场的抢答短句跳过哑火判断(实测时序漏洞:
         * 上一场的标记可能整个本场前半程都没经过主循环去过期)。*/
        g_wake_suppress_barge = 0;
        char murl[512];
        strncpy(murl, tuya_music_get_url(), sizeof(murl) - 1);
        murl[sizeof(murl) - 1] = '\0';
        printf("[TUYA-MUSIC] play: %s - %s\r\n",
               tuya_music_get_artist(), tuya_music_get_name());
        tuya_music_clear_pending();          /* 先消费:防下轮误重播 */
        _device_net_audio_play(0);           /* 停 TTS 解码器:让出 DAC */
        msleep(300);                         /* 等 audio_server 释放 DAC */
        g_music_playing = 1;
        if (app_music_tuya_play_url(murl, tuya_music_dec_end_cb) != 0) {
            printf("[TUYA-MUSIC] start play fail, restore TTS player\r\n");
            g_music_playing = 0;
            msleep(300);
            _device_net_audio_play(1);       /* 恢复 TTS 播放器 */
        } else {
            /* 等播完(试听 ~30s,整首几分钟;600s 兜底防卡死)。
             * 退出条件:dec_end 回调清 g_music_playing(播完/解码停机);
             * 或 busy=0——下载失败路径 __net_music_dec_file 的 __err 不走
             * dec_end 回调,靠 net_file 已被关闭置空感知,别傻等 600s。
             * _device_get_voice_data 内部按帧节拍(~40ms)阻塞,逐帧取音乐期
             * mic(音乐回声+AEC 残差)【照常上行】——停乐判定 100% 云端(2026-10-08):
             *   臂一 g_cloud_break_evt(chat_break):音乐期无活 TTS,消费即停,
             *     不开 3s 排空窗、不清 rbuf;
             *   臂二 g_tts_playing(云端已开答新轮):停乐让位,主信号——音乐不在
             *     云端轮生命周期内,云端多半不吐 chat_break 直接开答。
             *   两臂均经 app_music_tuya_music_started() 前置(10.8 崩机修复的
             *   START 闸:open 在飞时锁存信号,START 后再动手)。
             *   [MUSIC-DBG] 每 ~1s 打 mic 能量基线:对照云端裁决的数据。
             * 本地能量 barge-in 默认关(TUYA_MUSIC_LOCAL_BARGE,app_config.h):
             *   10.8 声学箱证伪(噪声底 100万~490万>门槛 60万,4/5 假打断),
             *   A/B 需要时放开宏即回旧行为(判据注释见宏内)。*/
            unsigned int mstart = timer_get_ms();
            s_barge_hist_cnt = 0;   /* 历史从本曲起算:A/B 本地停乐时打断补发只含音乐期间的话音 */
            int mwake = 0, mcloud = 0;           /* 停乐原因: 唤醒词打断 / 云端裁决(chat_break或新轮回话) */
            unsigned int mframes = 0;            /* 帧计数:[MUSIC-DBG] 基线节拍用 */
            unsigned int mup = 0;                /* 音乐期已成功上行帧数:[MUSIC-DBG] 每秒报数,
                                                  * 增量≈25/条=流在传(纯云形态验收证据) */
#if defined(TUYA_BARGE_IN_ENABLE) && defined(TUYA_MUSIC_LOCAL_BARGE)
            int mbarge = 0;                      /* 停乐原因: 本地能量 barge-in(A/B 开关,默认关) */
            unsigned int mhi = 0;                /* 连续达标帧数 */
            unsigned int msums[3] = {0, 0, 0};
#endif
            while (!g_exit && !g_link_broken && g_music_playing &&
                   app_music_tuya_music_busy() &&
                   timer_get_ms() - mstart < 600000) {
                /* ★ aligned(4) 不能省:本帧 2026-10-08 起要进 opus 编码器(短整型
                 *   load,align 1 栈布局撞奇地址=pi32v2 misalign 崩,同主循环 abuf)。*/
                unsigned char _mt[TUYA_OPUS_FRAME_LEN] __attribute__((aligned(4)));
                if (_device_get_voice_data(_mt, sizeof(_mt)) != sizeof(_mt)) {
                    continue;               /* 读不够一帧:等下一拍再来 */
                }
                tai_log_flush();            /* 音乐长循环(可达10min)也保持库日志节拍 */
                mcp_resp_pump(ctx);
                barge_hist_push(_mt);       /* 音乐期帧进 barge 历史:A/B 本地停乐时补发命令头部 */
#ifdef TUYA_KWS_ENABLE
                /* KWS 全程在线:音乐播放期也喂唤醒词(帧已到手不浪费)。命中即停乐
                 * (TuyaOpen wakeup 回调的 player_stop 语义),提示音等恢复 TTS 播放器
                 * 后再播(on_wake 已置 pending,见循环后)。
                 * ★ 未 START(open 在飞)不吃标志不下手:此窗口强停=踩 app_music
                 *   并发 teardown 崩机窗(2026-10-08 板测);留标志下一拍 START 后再停。*/
                tuya_kws_feed(_mt, sizeof(_mt));
                if (g_wake_hit && app_music_tuya_music_started()) {
                    g_wake_hit = 0;
                    printf("[TUYA-MUSIC] wake stops music\r\n");
                    mwake = 1;
                    break;
                }
#endif
                mframes++;
                unsigned int mes, mea;
                opus_frame_stat(_mt, sizeof(_mt), &mes, &mea);
                if ((mframes % 25) == 0) {  /* 每 ~1s 打能量基线+上行计数:对照云端裁决、
                                              * 验证音乐期流在传(uplink 增量≈25/条) */
                    printf("[MUSIC-DBG] playing, mic post-AEC: sum=%u act=%u%% rec=%d uplink=%u\r\n",
                           mes, mea, get_recoder_state(), mup);
                }
#ifdef TUYA_STREAM_MODE
                /* ---- 云端停乐两臂(2026-10-08 纯云形态;主循环此间被本函数阻塞,
                 *      云端信号只有这里能消费)。对齐小智 realtime:停播纯服务端
                 *      裁决,设备只执行。 ---- */
                if (app_music_tuya_music_started()) {   /* START 闸:open 在飞只锁存,不动手 */
                    if (g_cloud_break_evt) {
                        g_cloud_break_evt = 0;   /* 臂一:音乐期无活 TTS,不开排空窗不清 rbuf */
                        printf("[TUYA-MUSIC] cloud break -> stop music (f#%u sum=%u)\r\n",
                               mframes, mes);
                        mcloud = 1;
                        break;
                    }
                    if (g_tts_playing) {           /* 臂二(主信号):云端已开答新轮,停乐让位 */
                        printf("[TUYA-MUSIC] cloud answer -> stop music (f#%u sum=%u)\r\n",
                               mframes, mes);
                        mcloud = 1;
                        break;
                    }
                }
                /* ---- 逐帧上行:与主循环同形态(编码形态=会话 audio_start 声明,见
                 *      g_use_opus_uplink);失败=断链,同主发送快速报废会话 ---- */
                {
#ifdef TUYA_UPLINK_OPUS_ENABLE
                    int _msnd = g_use_opus_uplink ? tuya_uplink_send_frame(ctx, _mt)
                                                  : tai_send_audio_chunk(ctx, _mt, TUYA_OPUS_FRAME_LEN);
#else
                    int _msnd = tai_send_audio_chunk(ctx, _mt, TUYA_OPUS_FRAME_LEN);
#endif
                    if (_msnd != TAI_OK) {
                        printf("[TUYA-MUSIC] uplink chunk fail\r\n");
                        g_link_broken = 1;
                        break;
                    }
                    mup++;                  /* 成功发出+1:失败路径上面已 break,不会计 */
                }
#endif /* TUYA_STREAM_MODE */
#if defined(TUYA_BARGE_IN_ENABLE) && defined(TUYA_MUSIC_LOCAL_BARGE)
                /* A/B 本地能量停乐(默认关,app_config.h TUYA_MUSIC_LOCAL_BARGE)。
                 * 判据照搬 barge_in_energy_confirmed:VAD 在线 + 3 帧连续(120ms)
                 * sum≥BARGE_CONFIRM_ENERGY 才停乐,断一帧重数(滤音乐瞬态拍子);
                 * 开头 25 帧(1s)不设防(避开 DAC 交接瞬态和曲首重拍)。
                 * ★ 停乐前置条件:解码器已 START。open 在飞(TLS 慢 >1s)时强停会踩
                 *   app_music 停止路径与 __err 的并发 teardown(2026-10-08 板测
                 *   axi_rd_inv 崩机:噪声假 barge-in 恰好落进该窗)。25 帧盲窗之外
                 *   再加这道闸,真打断最多晚几十毫秒(START 一到即可停)。*/
                if (mframes > 25 && app_music_tuya_music_started() &&
                    get_recoder_state() && mes >= BARGE_CONFIRM_ENERGY) {
                    msums[mhi++] = mes;
                    if (mhi >= 3) {         /* 3 帧连续达标:确认真话音,停乐 */
                        printf("[TUYA-MUSIC] barge-in: stop music (sums=%u,%u,%u)\r\n",
                               msums[0], msums[1], msums[2]);
                        mbarge = 1;
                        /* 后续上行轮走 barge-in 路径:跳过起轮能量门(否则门会重读
                         * 1 帧覆盖 prefill)且豁免唤醒窗(窗可能早已过期)。*/
                        g_barge_in = 1;
                        break;
                    }
                } else {
                    mhi = 0;                /* VAD 掉线/能量掉线:重数 */
                }
#endif
            }
            g_music_handoff = 1;             /* 进入 DAC 交接窗:停乐→恢复期间命中唤醒,提示音延后 */
            if (g_music_playing) {           /* 超时/失败/打断/退出:强停,防 DAC 被占死
                                               * (net_stop_req 单一所有权护栏兜底) */
                printf("[TUYA-MUSIC] stop (%s)\r\n",
#if defined(TUYA_BARGE_IN_ENABLE) && defined(TUYA_MUSIC_LOCAL_BARGE)
                       mbarge ? "barge-in" :
#endif
                       mcloud ? "cloud" :
                       mwake ? "wake" :
                       app_music_tuya_music_busy() ? "timeout/exit" : "download/decode fail");
                app_music_tuya_music_stop();
                g_music_playing = 0;
            }
#if defined(TUYA_BARGE_IN_ENABLE) && defined(TUYA_MUSIC_LOCAL_BARGE)
            if (mbarge || mwake) {           /* 停乐后排 ~300ms 残响:打断词/唤醒词与音乐混叠,
                                               本轮已作废不清会被音乐尾巴当下句触发假轮 */
                unsigned char _mt[TUYA_OPUS_FRAME_LEN];
                for (int i = 0; i < 8 && !g_exit; i++) {
                    if (_device_get_voice_data(_mt, sizeof(_mt)) == sizeof(_mt)) {
#ifdef TUYA_KWS_ENABLE
                        /* 残响帧喂引擎不丢弃:能量 barge-in 确认(~120ms)抢在唤醒词
                         * 识别完成(~600ms)前停乐,词尾正好落在这 8 帧里——丢了它
                         * KWS 流断一截,词永远补不中(2026-09-05 实测根因)。喂进去,
                         * 词在此处或后续 idle 排空里补完命中,on_wake 置 pending。*/
                        tuya_kws_feed(_mt, sizeof(_mt));
#endif
                        barge_hist_push(_mt);   /* 残响若是命令尾部,一并进 prefill 补发 */
                    }
                }
            }
            if (mbarge) {
                barge_hist_to_prefill();   /* 音乐期间的话音(含残响)整体转上行 prefill:
                                            * 2026-09-05 实测不补发则云端只听到"伦的歌。"*/
            }
#endif /* TUYA_BARGE_IN_ENABLE && TUYA_MUSIC_LOCAL_BARGE */
#ifdef TUYA_KWS_ENABLE
            /* mbarge 判"后续话音"的种子(见 barge_followup_wait):此刻 VAD 已关=
             * 停乐前短句已说完(唤醒词典型);还开着=在继续说(命令)。停乐后的
             * 排空/等待耗时也计入连续关闭时长。*/
            unsigned int vclosed_since = get_recoder_state() ? 0 : timer_get_ms();
#endif
            /* 等音乐解码器释放 DAC(总时长仍 300ms),切片顺带追 VAD 关闭时刻:
             * 词尾挂账常在这段里到期(23:49 实测 06.648 关、整段睡完才看到,
             * 白多等 190ms)。vclosed_since 抓到真关闭点,后面的关闭确认从此起算。*/
            unsigned int _rwait = timer_get_ms();
            while (!g_exit && timer_get_ms() - _rwait < 300u) {
#ifdef TUYA_KWS_ENABLE
                if (get_recoder_state()) {
                    vclosed_since = 0;
                } else if (!vclosed_since) {
                    vclosed_since = timer_get_ms();
                }
#endif
                msleep(20);
            }
            g_player_restore_pending = 0;    /* 先判后恢复:默认下面当场恢复,要走提示音/
                                             * 抢答路径的分支改为置位推迟(见全局注释) */
#ifdef TUYA_KWS_ENABLE
            if (g_wake_prompt_pending) {     /* 唤醒停乐/排空期补中:不先恢复播放器,
                                             * 直接播应答提示音(mp3 独立开,免一次
                                             * "恢复 opus→停 opus→开 mp3"的折腾) */
                g_wake_prompt_pending = 0;
                printf("[TUYA] wake alert: WakeHeyTuya.mp3\r\n");
                app_music_tuya_play_wake_prompt();
                g_player_restore_pending = 1;   /* 播放器恢复推迟到下一轮起轮 */
            }
#endif
            g_music_handoff = 0;             /* 交接完成,退出 DAC 保护窗 */
#if defined(TUYA_KWS_ENABLE) && defined(TUYA_BARGE_IN_ENABLE) && defined(TUYA_MUSIC_LOCAL_BARGE)
            /* 抢答后续判定只在 A/B 本地停乐形态下存在(mbarge 唯一来源);
             * 纯云形态停乐走 mcloud 臂,云端自己判后续,无此分支。*/
            if (mbarge && !g_wake_prompt_pending && !g_wake_hit) {
                /* 抢答拦下的短句,引擎没认出唤醒词(AEC 双讲削损,同场景实测
                 * 成功率约一半):判用户是否还在继续说——VAD 连续开 300ms=命令,
                 * 起 prefill 轮上行;连续关 350ms=短句已说完,多半是没识别出的
                 * 唤醒词,不起轮——上行只会让云端收到"哎嗯"回"你怎么了呀"
                 * (2026-09-05 22:29 实测),交给哑火补救补播"我在"。判法见
                 * barge_followup_wait(vclosed_since 含停乐/排空等待耗时)。
                 * ★ g_wake_hit:排空期引擎把词补认出来了(on_wake 已置 pending
                 * 播过提示音+suppress 作废 prefill 轮)——这里必须整个跳过,
                 * 23:49 实测没跳过→"无后续"分支武装哑火,主循环 20ms 后又播
                 * 第二个"我在"(反应两次)。
                 * ★ 判"有后续"也留安全网:持久性判错(噪音/长挂账骗过 300ms)
                 * 时起轮等不来下文,1.5s 内没起轮且 VAD 关→主循环照样补"我在",
                 * 不再出现"喊了没反应"式死寂(23:49 42s 实测:挂账尾 6ms 撞上
                 * 首拍→误判有后续→之后无任何反馈)。真命令起轮当拍即 disarm。*/
                if (barge_followup_wait(vclosed_since)) {
                    printf("[TUYA-MUSIC] follow-up speech → uplink barge turn\r\n");
                    g_barge_prompt_wait = 1;   /* 安全网:1.5s 内没起轮且 VAD 关→补播 */
                    g_barge_prompt_deadline = timer_get_ms() + 1500u;
                    g_player_restore_pending = 1;   /* 命令回话要用:起轮时恢复播放器
                                                     * (距音乐解码器关闭已 >1s,无碰撞) */
                } else {
                    printf("[TUYA-MUSIC] no follow-up → drop prefill turn (likely wake)\r\n");
                    g_barge_prefill = 0;
                    g_barge_in = 0;
                    g_barge_prompt_wait = 1;
                    g_barge_prompt_deadline = timer_get_ms();   /* VAD 已关:主循环下一拍即补播 */
                    g_player_restore_pending = 1;   /* 哑火播提示音后,恢复同样推迟到起轮 */
                }
            }
#endif
            if (!g_player_restore_pending) {
                if (g_tts_playing) {
                    /* 云端已在音乐期开答(停乐原因=cloud):回答帧正积在播放 cbuf
                     * (on_audio 的 START 帧已清过一次,之后全是回答正文),不清缓冲
                     * 恢复——保住回答头部无缝续播。兼治自然播完后的链式回答吞头
                     * (旧行为:恢复即 cbuf_clear,缓冲里的新回答整段弃掉)。*/
                    _device_net_audio_play_keep();
                } else {
                    _device_net_audio_play(1);   /* 正常播完/下载失败(无提示音、无抢答):
                                                  * 当场恢复播放器,原行为 */
                }
            }
            printf("[TUYA-MUSIC] done, back to listening\r\n");
        }
    }
}
#endif /* TUYA_MUSIC_ENABLE */

/* KWS 唤醒命中回调(tuya_kws.c WAKE 分支同线程调用): 播本地应答提示音。
 * 时机照搬 TuyaOpen __ai_mode_kws_wakeup() 的 ai_audio_player_alert(WAKEUP):
 * 唤醒即播、不占听音窗——提示音走 app_music 解码器, 与 mic 上行并行,
 * AEC 会把提示音回采从 mic 消掉(实测 TTS/音乐播放期 VAD 不误触发)。
 * 同时做 TuyaOpen ai_audio_input_reset()(丢唤醒词尾巴)的 JL 等价动作:
 * 无 ringbuf reset 钩子,改为开"吞咽窗"——见 g_wake_swallow 与主循环唤醒门。*/
#ifdef TUYA_KWS_ENABLE
#define TUYA_WAKE_SWALLOW_MS 900u  /* 兜底:连说"嘿tuya今天星期几"只丢接缝,不吞掉问题 */
static int      g_wake_swallow;             /* 吞咽窗激活: 只排空不上行 */
static unsigned g_wake_swallow_deadline;    /* 吞咽兜底截止时刻 */
#endif
void tuya_agentic_on_wake(void)
{
#ifdef TUYA_MUSIC_ENABLE
    g_barge_prompt_wait = 0;   /* 引擎真识别出唤醒词(抢答后迟到补中也算):哑火补救撤销,
                                * 否则 2.5s 到点会再播一遍提示音 */
    /* 同一句话双命中去重:heytuya2{27,8,22} 前缀窗(~0.3s 处)先中,heytuya
     * {27,8,22,5} 全词窗(~0.9s 处)后中,两次 WAKE 相隔 0.3~0.6s(2026-09-05
     * 实测 f=7272..7275 与 f=7272..7299 同起点)。提示音只播一次,1.5s 内的
     * 后续命中视为同句;窗/吞咽仍照常续期(词尾还在进来)。
     * 时间戳是全局 g_last_wake_alert:哑火补救播的"我在"也盖章——排空期喂进去
     * 的词尾常在哑火播完后才补认完,on_wake 迟到命中不会再播第二个(23:49
     * 实测风险路径)。*/
    unsigned now = timer_get_ms();
    if ((unsigned)(now - g_last_wake_alert) >= 1500u) {
        g_last_wake_alert = now;
        if (g_tts_playing || g_music_playing || g_music_handoff) {
            /* TTS/音乐播放中(或停乐→恢复 TTS 播放器的交接窗)命中(TuyaOpen
             * __ai_mode_kws_wakeup:唤醒词=万能打断):此刻不播提示音——app_music
             * 的 mp3 解码器和正播着的 TTS/音乐解码器抢 DAC,交接窗里 DAC 也还没
             * 交接完。置 pending,由打断序列停播/恢复后再播。*/
            printf("[TUYA] wake during playback, interrupting\r\n");
            g_wake_prompt_pending = 1;
            /* 抢答路径已把话音历史(含这句唤醒词)装进 prefill:那轮上行作废,
             * 否则云端收到裸唤醒词回废话(见 g_wake_suppress_barge 注释)。*/
            g_wake_suppress_barge = 1;
            g_wake_suppress_barge_until = timer_get_ms() + TUYA_WAKE_SUPPRESS_BARGE_MS;
        } else {
            printf("[TUYA] wake alert: WakeHeyTuya.mp3\r\n");
            app_music_tuya_play_wake_prompt();
        }
    } else {
        printf("[TUYA] wake dup, alert suppressed\r\n");
    }
#endif
#ifdef TUYA_KWS_ENABLE
    g_wake_hit = 1;   /* 空闲命中由主循环唤醒门清掉;TTS/音乐循环里由打断分支消费 */
    /* VAD 随"嘿"就开口了,WAKE 后它还开着的一轮装的是词尾(2026-09-05 实测
     * ASR 收到"嗯?"就是这个):排空不上行,直到 VAD 关闭(词说完,下一句才是
     * 问题)或 900ms 兜底(VAD 还开着=连着说,放行起轮只丢接缝处零点几秒)。*/
    g_wake_swallow = 1;
    g_wake_swallow_deadline = timer_get_ms() + TUYA_WAKE_SWALLOW_MS;
#endif
}

/* 本地 chat_break 统一出口:发送成功才计数(g_local_break_pend,封顶 3 防回执
 * 丢失后残留),on_event 收到 CHAT_BREAK 回执时对冲——对冲干净才是云端判停。
 * 见全局区 2026-09-21 修复状态注释①。*/
static int tuya_break_send(tai_ctx_t *ctx)
{
    int rc = tai_chat_break(ctx);
    if (rc == TAI_OK && g_local_break_pend < 3) {
        OS_ENTER_CRITICAL();
        g_local_break_pend++;
        OS_EXIT_CRITICAL();
    }
    return rc;
}

#ifdef TUYA_KWS_ENABLE
/* 唤醒词在 TTS 等待/播放阶段命中的打断收尾(对应 TuyaOpen wakeup 回调里的
 * player_stop + CHAT_BREAK):作废本轮 + 清播放 cbuf 立刻停 TTS 喇叭。
 * 不置 g_barge_in(那走高能量抢答路径)——吞咽窗已开,词尾由 idle 排空吞掉,
 * 打断后直接回空闲听音,不起新轮。g_wake_break 让 ④ 收尾(TTS 到达/排空等待)
 * 与 pending 音乐(本轮已作废)一并跳过。提示音此刻才播:TTS 解码器只清空
 * 不断开,opus 开着播 app_music 的 mp3 与空闲唤醒同构,无 DAC 交接问题。*/
static void wake_break_tts(tai_ctx_t *ctx)
{
    printf("[TUYA] wake breaks TTS → chat_break + stop\r\n");
    tuya_break_send(ctx);              /* 通知云端中止本轮 TTS */
    _device_rbuf_clear();             /* 清播放 cbuf,立刻停 TTS 喇叭 */
    g_tts_playing = 0;
    g_turn_done = 1;
    g_wake_break = 1;
    /* 正在播的旧轮回话(chat_break 够不到它)的残包排空窗:见 on_audio 注释。
     * 3s 起步,残包每帧顺延 1.5s,来多少丢多少。*/
    g_tts_drop_until = timer_get_ms() + 3000;
    g_tts_drop_cnt = 0;
#ifdef TUYA_MUSIC_ENABLE
    if (g_wake_prompt_pending) {
        g_wake_prompt_pending = 0;
        printf("[TUYA] wake alert: WakeHeyTuya.mp3\r\n");
        app_music_tuya_play_wake_prompt();
    }
#endif
}
#endif

static void on_event(tai_ctx_t *ctx, const tai_event_msg_t *msg, void *ud)
{
    demo_ctx_t *dc = (demo_ctx_t *)ud;
    if (msg->event_type == TAI_EVT_END) {
#ifdef TUYA_STREAM_MODE
        /* 长流模式(2026-09-24 修):云端 VAD 每轮在同一条上行流上生成新的
         * vcd-event(id 后缀=轮次时间戳),与会话入口 audio_start 抓的
         * g_turn_event_id 永不相同 → event-id 判属在本模式必然失配。旧重开
         * 循环每次 start 后重抓 current_event_id 恰好掩盖了这点;删重开后
         * 所有 END 被判 stale 吞掉,g_tts_playing 永不清 → 每轮 chat_break
         * 误判"打断TTS"开 3s 丢弃窗 → 新轮回答整段被丢(实测"问了没声音",
         * 且日志零条"回答结束")。长流下 END=轮结束,无条件生效;迟到的旧轮
         * END 只是提前复位统计+清标志,无流可拆,无害。 */
        if (dc) {
            dc->got_done = 1;        /* 兼容独立文本 demo */
        }
#ifdef TUYA_MUSIC_ENABLE
        tuya_music_text_flush();     /* SDK 丢空文本帧:半截流可能等不到显式 END,兜底交付解析 */
#endif
        g_turn_done   = 1;
        g_tts_playing = 0;
        printf("[TUYA-AI] === 回答结束 ===\r\n");
#else
        /* 上行窗口内(audio_start 已发、audio_end 未发)的 END 只认"本轮自己的
         * event-id"(g_turn_event_id,起轮时 tai_current_event_id 读回):那是云端
         * 对纯噪声轮的就地收尾(实测 ~3-4s,纯云停说设计靠它关噪声轮)。其余
         * 一律旧轮残留判弃——2026-09-21 全量实测 813 轮在 frames=1-9 被
         * "cloud-end"秒杀(标注 1.9s 人声只上行 360ms)。见全局区修复状态注释②。*/
        if (g_uplinking &&
            !(g_turn_event_id[0] && msg->event_id && msg->event_id[0] &&
              strcmp(msg->event_id, g_turn_event_id) == 0)) {
            printf("[TUYA-AI] stale END during uplink (%s), ignored\r\n",
                   (msg->event_id && msg->event_id[0]) ? msg->event_id : "-");
        } else if (
            /* ④ 等待期(非上行窗)的旧轮残留判弃:barge-in 打断的上一轮,云端流
             * (文本/TTS)可能拖到新轮 ④ 等待期才收尾;当本轮结束会提前骗退等待,
             * 晚到的音乐 SKILL 无人消费("第一次放歌没反应"根因)。END 的
             * event_id 命中"起轮时快照的旧轮 id" → 只记日志。*/
            msg->event_id && msg->event_id[0] && g_stale_end_bizid[0] &&
            strcmp(msg->event_id, g_stale_end_bizid) == 0) {
            printf("[TUYA-AI] stale END of interrupted turn (%s), ignored\r\n",
                   msg->event_id);
        } else {
            if (dc) {
                dc->got_done = 1;        /* 兼容独立文本 demo */
            }
#ifdef TUYA_MUSIC_ENABLE
            tuya_music_text_flush();     /* SDK 丢空文本帧:半截流可能等不到显式 END,兜底交付解析 */
#endif
            g_turn_done   = 1;
            g_tts_playing = 0;
            printf("[TUYA-AI] === 回答结束 ===\r\n");
        }
#endif /* TUYA_STREAM_MODE */
    } else if (msg->event_type == TAI_EVT_SERVER_VAD) {
        /* 云端 VAD 检测到用户停说(endpointing)。TUYA_SERVER_VAD_ENABLE 模式下,
         * 置位标志让上行循环退出收尾(发 audio_end → 等回复)。开口仍由本地VAD负责。*/
        printf("[TUYA-AI] server-vad (end of speech)\r\n");
#ifdef TUYA_SERVER_VAD_ENABLE
        g_server_vad_stop = 1;
#endif
    } else if (msg->event_type == TAI_EVT_CHAT_BREAK) {
        /* chat_break 有两种来源:
         * 1) 本地 barge-in 的回执(自带被打断旧轮的 event-id,实测 0.3~3s 后到,
         *    常落在新轮上行中)
         * 2) 云端判停说(asr.enableVad 开启后,云端判停只发 chat_break、不发
         *    server-vad)
         * 2026-09-21 全量实测:旧代码无脑置 g_server_vad_stop,自己打断的回执把
         * 523 轮刚起的新轮当"云端判停"杀掉(全天真正 SERVER_VAD 事件 = 0)。
         * 修复:收到先对冲 g_local_break_pend(tuya_break_send 计数),对冲干净才
         * 算云端判停。乱序(判停先到、回执后到)时回执会晚一拍补置位,等价只稍延迟。*/
#ifndef TUYA_STREAM_MODE
        g_tts_playing = 0;
#endif
        OS_ENTER_CRITICAL();
        int _receipt = (g_local_break_pend > 0);
        if (_receipt) {
            g_local_break_pend--;
        }
        OS_EXIT_CRITICAL();
        printf("[TUYA-AI] chat_break (%s, pend=%d)\r\n",
               _receipt ? "receipt" : "cloud", g_local_break_pend);
        /* 阶段2·服务器时间过滤对照(上游 65ce503,只记不改判):breakAttributes.time
         * 与本轮 TTS START 锁存的 g_turn_start_ts 同一服务器时钟轴——≥起点=打断
         * 属本轮(hit),<起点=旧轮在途回执(stale);缺时间/未起轮=no-time/no-start
         * (上游 SDK 语义缺时间 fail-closed,此处仅观察)。与上面 receipt/cloud、
         * event-id 判属三者对照,板上确认一致率后再收编为主判据(UPSTREAM-BASE.md
         * §4 阶段2)。hex 半字对打印避 %llu 雷(见 json_u64_after 注释)。*/
        if (msg->data && msg->len) {
            char payload[384];
            size_t plen = msg->len;
            if (plen >= sizeof(payload)) plen = sizeof(payload) - 1;
            memcpy(payload, msg->data, plen);
            payload[plen] = '\0';
            char *attrs = json_get_object_raw(payload, "breakAttributes");
            unsigned long long brk_ts   = json_u64_after(attrs ? json_find_value(attrs, "time") : NULL);
            unsigned long long start_ts = g_turn_start_ts;
            const char *verdict = (!brk_ts)   ? "no-time"
                                : (!start_ts) ? "no-start"
                                : (brk_ts >= start_ts) ? "hit" : "stale";
            printf("[TUYA-AI] break-time %s: brk=%x%08x start=%x%08x\r\n", verdict,
                   (unsigned)(brk_ts >> 32), (unsigned)brk_ts,
                   (unsigned)(start_ts >> 32), (unsigned)start_ts);
            free(attrs);
        }
#ifdef TUYA_SERVER_VAD_ENABLE
        if (!_receipt) {
            g_server_vad_stop = 1;
#ifdef TUYA_STREAM_MODE
            /* 长流模式·小智对齐:打断只对"正在播的 TTS"生效。云端 asrInterrupt
             * 不知道设备上一轮是否已播完——新话音跟在一轮后面就发,纯云模式下
             * 几乎每句话音都会来一通。2026-09-23 实测:无脑停喇叭+开 3s 排空窗
             * 会把紧随其后的新轮回话整段吃掉(你好之后句句"没反应",回话全被
             * g_tts_drop_until 窗丢弃,残包顺延 1.5s 让整流播不出来)。小智语义
             * 是"设备上有东西在播才停";g_tts_playing 由 on_audio 逐帧维护,只在
             * 旧流 stream-end 后为 0——为 0 即旧流已终结、无残包可排,跳过安全。
             * on_event 与 on_audio 同在 worker 线程,读它无竞态。语音循环按
             * 2(在播:停+排空窗)/1(没播:仅记录)分别处理。
             * ★ 2026-09-30 "打断不灵敏"第二修:g_tts_playing 只是"下行流"标志,
             *   流 END 后喇叭还压着数秒缓冲尾巴(play-gate _pg_busy 的第二臂,
             *   同判据 640)——break 落在排空期判 1 就"nothing to stop",尾巴照
             *   漏(当日实测逐条命中此分支)。cbuf 水位读与主循环同类跨线程单
             *   读,无锁安全;排空窗开不开由消费端按执行时流状态再分。 */
            g_cloud_break_evt = (g_tts_playing || _device_get_play_level() >= 640) ? 2 : 1;
#endif
        }
#endif
    } else if (msg->event_type == TAI_EVT_MCP_CMD) {
        /* Transport callbacks must never call tai_send_* or the audio server.
         * Parse and queue only; tuya_mcp_pump() performs the scheduled board
         * action and response send from the existing session task. */
        printf("[TUYA-MCP] recv len=%u: %.*s\\r\\n",
               (unsigned)msg->len, (int)(msg->len < 512 ? msg->len : 512),
               msg->data ? (const char *)msg->data : "(null)");
        tuya_mcp_on_command(msg->data, msg->len);
    }
}
static void on_disconnect(tai_ctx_t *ctx, const tai_disconnect_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    printf("[TUYA-AI] disconnected: reason=%u close_code=%u\r\n",
           (unsigned)msg->reason, (unsigned)msg->close_code);
    g_exit = 1;   /* 令本次会话语音循环退出,tuya_ai_run 监督循环稍后重连(拿新 token 重 connect) */
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */
void tuya_agentic_demo(void *arg)
{
    const pal_t *pal = tai_pal_ac791n();
    demo_ctx_t dc = {0};

    printf("===== tuya_agentic_demo start =====\r\n");

    /* ① iot-client 初始化(用我们的 PAL)*/
    if (iot_init(pal) != 0) {
        printf("[TUYA] iot_init fail\r\n"); return;
    }

    /* ② 拿设备身份(devid/secret/local_key)。
     *   注意:这三项是涂鸦云在"配网激活"后下发的,不是前端填的;前端只填
     *   产品三件套(PID/UUID/AuthKey)+ 激活 token。详见上方流程说明。*/
    /* TODO(阶段1):证书。cert_bundle_attach / cacert 先留 NULL。
     *   若 TLS 握手因无 CA 失败,在 common/tls.c 里降级 TLS_VERIFY_OPTIONAL
     *   或挂上宿主的 CA 包。*/
    char local_key_buf[32] = {0};   /* 激活后从 iot 拷出;deinit 后还要给 tai_config 用 */
    iot_client_t *iot = NULL;
#if TUYA_USE_ONBOARDING
    /* 正解:三件套 + token → 涂鸦云激活 → 下发 devid/secret/local_key */
    iot_on_boarding_config_t obcfg;
    memset(&obcfg, 0, sizeof(obcfg));
    strncpy((char *)obcfg.uuid,        tuya_trip_uuid(), sizeof(obcfg.uuid) - 1);
    strncpy((char *)obcfg.authkey,     tuya_trip_key(),  sizeof(obcfg.authkey) - 1);
    strncpy((char *)obcfg.product_key, tuya_trip_pk(),   sizeof(obcfg.product_key) - 1);
    obcfg.env              = PROD;
    obcfg.mqtt_disable_tls = false;
    obcfg.mqtt_disable_auto_connect = false;   /* 阶段1上游字段(语义反转):false=init/激活后自动连 MQTT,等效旧 mqtt_auto_connect=1 */
    obcfg.timeout_ms       = 30000;
    obcfg.cert_bundle_attach = NULL;
    obcfg.cacert = NULL;
    obcfg.reset_callback = on_cloud_reset;
    obcfg.ota_confirm_callback = on_ota_confirm;   /* APP 确认升级(protocol 15) */
    iot = iot_client_init_on_boarding_with_token(&obcfg, TUYA_ACTIVATION_TOKEN);
    if (!iot) { printf("[TUYA] on_boarding_with_token fail(token 过期/三件套错?)\r\n"); return; }
    iot_ai_ctrl_set_callback(iot, on_ai_ctrl, NULL);   /* 阶段2·9000 AI控制通道(见 on_ai_ctrl) */
    printf("[TUYA] activated, devid=%s\r\n", iot->devid);
#else
    /* 直连:已预注册设备,直接用三元组 */
    iot_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.region = AY; cfg.env = PROD;
    cfg.mqtt_disable_tls = false; cfg.mqtt_disable_auto_connect = false;
    cfg.cert_bundle_attach = NULL; cfg.cacert = NULL;
    cfg.message_callback = on_mqtt_message;   /* MQTT 常驻:收 DP 下行 */
    cfg.reset_callback = on_cloud_reset;
    cfg.ota_confirm_callback = on_ota_confirm;   /* APP 确认升级(protocol 15) */
    extern const char *tuya_get_effective_sw_ver(void);
    cfg.sw_ver = tuya_get_effective_sw_ver();   /* 上报生效版本:USER区记录优先,无记录回退源码宏(见 tuya_ota.c"版本号管理") */

    strncpy((char *)cfg.devid,      TUYA_DEVID,      sizeof(cfg.devid) - 1);
    strncpy((char *)cfg.secret_key, TUYA_SECRET_KEY, sizeof(cfg.secret_key) - 1);
    strncpy((char *)cfg.local_key,  TUYA_LOCAL_KEY,  sizeof(cfg.local_key) - 1);
    iot = iot_client_init(&cfg);
    if (!iot) { printf("[TUYA] iot_client_init fail\r\n"); return; }
    iot_ai_ctrl_set_callback(iot, on_ai_ctrl, NULL);   /* 阶段2·9000 AI控制通道(见 on_ai_ctrl) */
#endif
    strncpy(local_key_buf, (const char *)iot->local_key, sizeof(local_key_buf) - 1);

    /* ③ ATOP over HTTPS 换 AI 会话 token —— 阶段1 的判定点 */
    char *token = (char *)malloc(4096);
    if (!token) { iot_client_deinit(iot); return; }
    if (iot_client_get_session_token(iot, NULL, token, 4096) != 0 || token[0] == '\0') {
        printf("[TUYA] iot_client_get_session_token fail\r\n");
        free(token); iot_client_deinit(iot); return;
    }
    printf("[TUYA] got session token (len=%d)\r\n", (int)strlen(token));
    tai_bind_session_token(token); /* stm(UDP)后端用;tcp 后端空操作 */

    /* ④ 解析 token */
    tai_conn_params_t cp;
    if (parse_token(token, &cp) != 0) {
        printf("[TUYA] parse_token fail\r\n");
        free(token); iot_client_deinit(iot); return;
    }
    if (cp.biz_code == 0) cp.biz_code = 65537;
    if (cp.biz_tag  == 0) cp.biz_tag  = 119;
    printf("[TUYA] TAI server: %s:%u (SNI %s)\r\n", cp.host, cp.port, cp.tls_sni);
    free(token);
    /* MQTT 常驻:不再 deinit。保留 iot_client 实例让 MQTT 保持连接,
     * 用于接收云端 DP 下行(如自定义DP"运动一下")。
     * iot 和 MQTT 连接的生命周期现在贯穿整个 AI 对话期间。
     * 注意:iot 实例不能在本函数结束后被释放,需要保持可达——
     *   配网路径里 iot 是局部变量,本函数返回后栈释放。但 iot_client_init
     *   内部已把 PAL/连接存到全局,deinit 才会断。不 deinit = MQTT 不断。
     *   如果出问题(SESSION_CLOSE),改回 deinit 即可。*/
    /* iot_client_deinit(iot); */
    /* This legacy one-shot text probe blocks in its own reply wait and has no
     * session scheduler.  Do not advertise MCP here; production MCP lives in
     * tuya_ai_session(), whose regular voice loop services the queue. */
    static const char SESSION_ATTRS[] = "{\"deviceMcp\":{\"supportCustomMCP\":false}}";
#ifdef TUYA_SERVER_VAD_ENABLE
    static const char EVENT_USER_DATA[] =
        "{\"asr.enableVad\":\"true\","
        "\"tts.alternate\":\"true\","
        "\"processing.interrupt\":\"true\"}";
#else
    static const char EVENT_USER_DATA[] =
        "{\"sys.workflow\":\"asr-llm-tts\","
        "\"tts.alternate\":\"true\"}";
#endif

    tai_config_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.host                = cp.host;
    tc.port                = cp.port;
    tc.tls_sni             = cp.tls_sni;
    tc.device_id           = cp.derived_client_id;
    tc.local_key           = local_key_buf;
    tc.protocol_version    = TAI_VER_21;
    tc.client_type         = TAI_CLIENT_DEVICE;
    tc.sign_level          = TAI_SIGN_HMAC_SHA256;
    tc.biz_code            = (uint32_t)cp.biz_code;
    tc.biz_tag             = (uint64_t)cp.biz_tag;
    tc.agent_token         = cp.agent_token;
    tc.session_attrs_json  = SESSION_ATTRS;
    tc.event_user_data_json = EVENT_USER_DATA;
    tc.pal                 = pal;
    tc.on_text             = on_text;
    tc.on_audio            = on_audio;
    tc.on_event            = on_event;
    tc.on_disconnect       = on_disconnect;
    tc.user_data           = &dc;

    void *mem = pal->malloc(tai_ctx_size());
    if (!mem) { printf("[TUYA] OOM tai_ctx\r\n"); return; }
    tai_ctx_t *ctx = tai_ctx_init(mem, &tc);
    if (!ctx) { printf("[TUYA] tai_ctx_init fail\r\n"); pal->free(mem); return; }


    /* ⑥ 建会话 —— 阶段2 判定点 */
    printf("[TUYA] tai_connect...\r\n");
    if (tai_connect(ctx) != TAI_OK) {
        printf("[TUYA] tai_connect fail\r\n");
        tai_ctx_deinit(ctx); pal->free(mem); return;
    }

    /* ⑦ 发文本,等回答 */
    const char *q = "你好,介绍一下你自己";
    printf("[TUYA] send: %s\r\n", q);
    if (tai_send_text(ctx, q, strlen(q)) == TAI_OK) {
        int waited = 0;
        while (!dc.got_done && waited < TUYA_WAIT_MS) {
            msleep(100);
            waited += 100;
        }
        printf("[TUYA] %s\r\n", dc.got_done ? "got reply" : "timeout");
    } else {
        printf("[TUYA] tai_send_text fail\r\n");
    }

    tai_disconnect(ctx);
    tai_ctx_deinit(ctx);
    pal->free(mem);
    printf("===== tuya_agentic_demo end =====\r\n");
}

/* ========================================================================= */
/* 完整流程入口:tuya_agentic_main                                             */
/*   开机读 syscfg → 有三元组:直连 AI;                                         */
/*   没三元组:BLE 配网 → 连 WiFi → on_boarding 激活 → 存三元组 → 连 AI          */
/* ========================================================================= */
#include "syscfg/syscfg_id.h"
#include "wifi/wifi_connect.h"
#include "le_net_cfg_tuya.h"

/* syscfg VM 索引(自定,确认 syscfg_id.h 里没占用即可)*/
#define VM_TUYA_DEVID_IDX     176
#define VM_TUYA_SECRET_IDX    177
#define VM_TUYA_LOCALKEY_IDX  178
#define VM_TUYA_SSID_IDX      179   /* 直连路径开机重连 WiFi 用(配网时一并存)*/
#define VM_TUYA_PWD_IDX       180
#define VM_TUYA_SCHEMAID_IDX  181   /* DP schema_id(激活时云端返回,DP 下行解析需要)*/
#define VM_TUYA_SCHEMA_IDX    182   /* DP schema JSON(激活时云端返回)*/
#define VM_TUYA_REGION_IDX    183   /* region(1B):配网时云端下发(token前缀+激活响应),直连重启据此选 ATOP/MQTT 域名 */
#define VM_TUYA_RESET_STATE_IDX 184 /* 仅本模块使用；先写入，断电后继续清除 */

#define TUYA_RESET_MARK_CLEARING_UNBIND  0x51u
#define TUYA_RESET_MARK_CLEARING_FACTORY 0x52u
#define TUYA_RESET_MARK_CLEARED_UNBIND   0x61u
#define TUYA_RESET_MARK_CLEARED_FACTORY  0x62u
#define TUYA_CLOUD_RESET_STOP_TICKS      200u /* 2s: MQTT recv 最大阻塞 1s，再留一次调度余量 */

static uint8_t tuya_cloud_reset_marker(iot_reset_type_t type, int cleared)
{
    if (type == IOT_RESET_REMOTE_FACTORY) {
        return cleared ? TUYA_RESET_MARK_CLEARED_FACTORY
                       : TUYA_RESET_MARK_CLEARING_FACTORY;
    }
    return cleared ? TUYA_RESET_MARK_CLEARED_UNBIND
                   : TUYA_RESET_MARK_CLEARING_UNBIND;
}

static int tuya_cloud_reset_marker_type(uint8_t marker, iot_reset_type_t *type)
{
    if (marker == TUYA_RESET_MARK_CLEARING_UNBIND ||
        marker == TUYA_RESET_MARK_CLEARED_UNBIND) {
        *type = IOT_RESET_REMOTE_UNBIND;
        return 0;
    }
    if (marker == TUYA_RESET_MARK_CLEARING_FACTORY ||
        marker == TUYA_RESET_MARK_CLEARED_FACTORY) {
        *type = IOT_RESET_REMOTE_FACTORY;
        return 0;
    }
    return -1;
}

static int tuya_cloud_reset_write_marker(uint8_t marker)
{
    uint8_t verify = 0;
    syscfg_write(VM_TUYA_RESET_STATE_IDX, &marker, sizeof(marker));
    return syscfg_read(VM_TUYA_RESET_STATE_IDX, &verify, sizeof(verify)) ==
               sizeof(verify) &&
           verify == marker;
}

/* Credentials are the identity boundary.  Require all bytes to read back as
 * zero, not merely a terminating NUL at byte zero. */
static int tuya_cloud_reset_credentials_cleared(void)
{
    uint8_t devid[32] = {0}, secret[32] = {0}, localkey[32] = {0};
    uint8_t zero[32] = {0};
    int devid_ok = syscfg_read(VM_TUYA_DEVID_IDX, devid, sizeof(devid)) == sizeof(devid) &&
                   memcmp(devid, zero, sizeof(devid)) == 0;
    int secret_ok = syscfg_read(VM_TUYA_SECRET_IDX, secret, sizeof(secret)) == sizeof(secret) &&
                    memcmp(secret, zero, sizeof(secret)) == 0;
    int localkey_ok = syscfg_read(VM_TUYA_LOCALKEY_IDX, localkey, sizeof(localkey)) == sizeof(localkey) &&
                      memcmp(localkey, zero, sizeof(localkey)) == 0;
    if (!devid_ok || !secret_ok || !localkey_ok) {
        printf("[TUYA] cloud reset credential readback failed: devid=%d secret=%d localkey=%d\r\n",
               devid_ok, secret_ok, localkey_ok);
    }
    return devid_ok && secret_ok && localkey_ok;
}

static void tuya_cloud_reset_clear_unbind_state(const char *zero)
{
    /* Unbind preserves Tuya's SSID/password cache for a same-home rebind, but
     * disables the JieLi STA boot path so the next boot always advertises BLE.
     * Schema and region are binding-scoped and must be obtained anew. */
    syscfg_write(VM_TUYA_SCHEMAID_IDX, zero, 64);
    syscfg_write(VM_TUYA_SCHEMA_IDX, zero, 65);
    syscfg_write(VM_TUYA_REGION_IDX, zero, 1);
    wifi_store_mode_info(SMP_CFG_MODE, (char *)zero, (char *)zero);
}

static void tuya_cloud_reset_clear_factory_state(const char *zero)
{
    /* Factory reset removes every application-side network/cache record. */
    syscfg_write(VM_TUYA_SSID_IDX, zero, 65);
    syscfg_write(VM_TUYA_PWD_IDX, zero, 65);
    syscfg_write(VM_TUYA_SCHEMAID_IDX, zero, 64);
    syscfg_write(VM_TUYA_SCHEMA_IDX, zero, 65);
    syscfg_write(VM_TUYA_REGION_IDX, zero, 1);
    wifi_store_mode_info(SMP_CFG_MODE, (char *)zero, (char *)zero);
}

static int tuya_cloud_reset_clear_and_verify(iot_reset_type_t type)
{
    char zero[65] = {0};

    syscfg_write(VM_TUYA_DEVID_IDX, zero, 32);
    syscfg_write(VM_TUYA_SECRET_IDX, zero, 32);
    syscfg_write(VM_TUYA_LOCALKEY_IDX, zero, 32);
    if (type == IOT_RESET_REMOTE_FACTORY) {
        tuya_cloud_reset_clear_factory_state(zero);
    } else {
        tuya_cloud_reset_clear_unbind_state(zero);
    }

    if (!tuya_cloud_reset_credentials_cleared()) {
        return -1;
    }
    return tuya_cloud_reset_write_marker(tuya_cloud_reset_marker(type, 1)) ? 0 : -1;
}

static void tuya_cloud_reset_cpu_reboot(void)
{
    extern void cpu_reset(void);
    cpu_reset();
    printf("[TUYA] ERROR: cpu_reset returned; reset marker remains for recovery\r\n");
}

/* Corrupt local storage is the exceptional case where a usable cloud identity
 * cannot be established.  Keep that boot-recovery policy separate from K6:
 * a physical reset of a healthy, bound device must never take this path. */
static void tuya_clear_invalid_provision_and_reset(void)
{
    printf("[TUYA] clearing invalid local provisioning and rebooting\r\n");
    if (!tuya_cloud_reset_write_marker(
            tuya_cloud_reset_marker(IOT_RESET_REMOTE_FACTORY, 0))) {
        printf("[TUYA] invalid-provision reset marker write failed; credentials retained\r\n");
        return;
    }
    if (tuya_cloud_reset_clear_and_verify(IOT_RESET_REMOTE_FACTORY) != 0) {
        printf("[TUYA] invalid-provision reset verification failed; reboot for recovery\r\n");
    }
    tuya_cloud_reset_cpu_reboot();
}

/* Used only while no iot_client_t exists (for example, an offline boot that is
 * blocked waiting for DHCP).  The K6 callback may use this self-contained path
 * because there is no MQTT/process worker to serialise with. */
static int tuya_local_factory_reset_without_client(void)
{
    printf("[TUYA] K6 local factory reset without cloud client\r\n");
    if (!tuya_cloud_reset_write_marker(
            tuya_cloud_reset_marker(IOT_RESET_REMOTE_FACTORY, 0))) {
        printf("[TUYA] K6 local reset marker write failed; credentials retained\r\n");
        return 0;
    }
    if (tuya_cloud_reset_clear_and_verify(IOT_RESET_REMOTE_FACTORY) != 0) {
        printf("[TUYA] K6 local reset verification failed; reboot for recovery\r\n");
    }
    tuya_cloud_reset_cpu_reboot();
    return 1;
}

/* Called before any client is constructed.  A power loss after the initial
 * marker write is therefore completed while no MQTT/AI worker exists. */
static int tuya_cloud_reset_recover_if_needed(void)
{
    uint8_t marker = 0;
    iot_reset_type_t type;
    int read_len = syscfg_read(VM_TUYA_RESET_STATE_IDX, &marker, sizeof(marker));

    if (read_len != sizeof(marker) || marker == 0 || marker == 0xFF) {
        return 0;
    }
    if (tuya_cloud_reset_marker_type(marker, &type) != 0) {
        printf("[TUYA] invalid cloud-reset marker 0x%x; refusing to use saved identity\r\n",
               marker);
        return -1;
    }
    printf("[TUYA] recovering interrupted cloud %s reset\r\n",
           type == IOT_RESET_REMOTE_FACTORY ? "factory" : "unbind");
    if (tuya_cloud_reset_clear_and_verify(type) != 0) {
        printf("[TUYA] cloud-reset recovery could not verify cleared credentials\r\n");
        return -1;
    }
    if (!tuya_cloud_reset_write_marker(0)) {
        printf("[TUYA] cloud-reset recovery could not clear completion marker\r\n");
        return -1;
    }
    return 0;
}

/* region 枚举转可读名(打日志用;枚举定义在 iot_client.h,AY=中国区...) */
static const char *region_name(iot_region_t r)
{
    switch (r) {
    case AY:   return "AY(中国区)";
    case AZ:   return "AZ(美国区)";
    case UEAZ: return "UEAZ(美东区)";
    case EU:   return "EU(欧洲区)";
    case WEAZ: return "WEAZ(美西区)";
    case IN:   return "IN(印度区)";
    case SG:   return "SG(新加坡区)";
    default:   return "?(未知区)";
    }
}

/* BLE 配网拿到的凭据(tuya_ble_netcfg_start 阻塞返回后用)*/
static tuya_ble_wifi_creds_t s_main_creds;
static void main_prov_cb(const tuya_ble_wifi_creds_t *c)
{
    memcpy(&s_main_creds, c, sizeof(*c));
}

/* opus 帧诊断统计:字节和(sum)+ 非零字节占比(act,0-100)。
 * CBR opus 帧虽是压缩数据,但静音帧的 sum/act 与有声帧仍有可辨差异,
 * 配合"空闲排空时的底噪基线"即可粗判"这一帧是不是静音"。非精确能量,够定位问题。*/
static void opus_frame_stat(const unsigned char *p, int len,
                            unsigned int *out_sum, unsigned int *out_act)
{
    /* 真能量统计:sum = Σ|sample|(int16 绝对值累加),act = avg|sample|/100 封顶 100。
     * ⚠️ 旧实现按【字节】累加(s += p[i]):16bit PCM 负样本高字节恒为 0xFF、低字节是噪声,
     *   导致静音/TTS/说话的 sum 全卡 ~15万、act 全卡 60-90%,区分不出能量,AEC/上行诊断
     *   长期失效(我据此误判过"AEC 没生效")。改成 int16 绝对值累加后:
     *   静音 sum~万级(avg|sample| 几十)、说话 sum~百万级(avg 几千),才有可比性。
     *   (豆包 AEC 材料第五节"sum 须采样点绝对值累加"此条正确;它给的 aec_play_audio 等
     *    API 在杰理 SDK 不存在,output_way/dac_ref_sr 才是真机制。)*/
    unsigned int s = 0, avg, a;
    int nsamp = len / 2, i;
    for (i = 0; i < nsamp; i++) {     /* 小端 int16,逐字节拼避免对齐 fault */
        short v = (short)(((unsigned)p[i * 2]) | (((unsigned)p[i * 2 + 1]) << 8));
        s += (unsigned)(v < 0 ? -(int)v : (int)v);
    }
    avg = nsamp ? s / (unsigned)nsamp : 0;   /* avg |sample|, 0~32767 */
    a = avg / 100u;                          /* /100 封顶:avg=10000(-10dBFS)→100% */
    if (a > 100u) a = 100u;
    if (out_sum) *out_sum = s;
    if (out_act) *out_act = a;
}

#ifdef TUYA_BARGE_IN_ENABLE
/* 能量确认读走的 onset 帧保留进 g_barge_prebuf,barge-in 新上行时补发——否则确认消耗的
 * ~120ms(常是打断 onset,最响那段)丢失,后续上行只收到尾音/静音→ASR 空(实测"明天去哪玩"
 * 被 barge-in 切了却没播报,即此)。g_barge_prefill=有效帧数:起轮能量门置 1(onset),
 * 历史转换(barge_hist_to_prefill)累加;barge prefill 的补发点在上行 prefill 循环。
 * g_barge_prebuf/g_barge_prefill 定义在 music_handoff 前(音乐打断的 hist→prefill 也要用)。*/
/* barge-in 能量确认:VAD 触发时连读 3 帧(≈120ms),3 帧 sum 均≥BARGE_MIN_ENERGY 才算真话音。
 * 滤掉 AEC 残留回声/噪音的瞬时 spike——TTS 念密集数字(金价等)时某个响音爆破会在 1~2 帧内冲过
 * 阈值(实测 2-of-2 被这种 spike 骗过,误切断金价播报),要求 3 帧持续能量才能把 ≤2 帧的 spike 滤掉。
 * 真话音 onset 通常持续 >120ms,正常通过。代价:确认比 2 帧多 40ms。漏判会下轮询(20ms)重试。
 * ★ 读走的帧一律喂 KWS+入历史,不白吃:VAD 开着时本函数每轮询消耗 3 帧,失败即丢的话
 *   引擎/历史只见到 1/4 的语音流(每 4 帧一个 3 帧洞)——词匹配不上、prefill 送云端也是
 *   断的(2026-09-05 21:36 实测:能量 5M+ 的真话音三连空 ASR、KWS 零探针命中)。
 *   成功时帧已在历史里,由 barge_in_prefill_arm→hist_to_prefill 统一转 prefill。*/
static unsigned int g_barge_cfm_maxsum;  /* 最近一次确认读到的最大帧能量:tts_barge_poll 的
                                          * 低门限旁路要用它区分"回声开 VAD"和"压弱的真人声" */
static int barge_in_energy_confirmed(void)
{
    unsigned int s, a, hi = 0, k, n = 0;
    unsigned int sums[3] = {0, 0, 0};
    g_barge_cfm_maxsum = 0;
    for (k = 0; k < 3 && !g_exit; k++) {
        if (_device_get_voice_data(&g_barge_prebuf[k * 1280], 1280) != 1280) break;
        opus_frame_stat(&g_barge_prebuf[k * 1280], 1280, &s, &a);
        sums[k] = s;
        if (s > g_barge_cfm_maxsum) g_barge_cfm_maxsum = s;
        if (s >= BARGE_CONFIRM_ENERGY) hi++;
        n++;
#ifdef TUYA_KWS_ENABLE
        tuya_kws_feed(&g_barge_prebuf[k * 1280], 1280);
#endif
        barge_hist_push(&g_barge_prebuf[k * 1280]);
    }
    if (hi >= 3 && n >= 3) {   /* 3-of-3:滤 ≤2 帧 spike;读不够 3 帧也算失败 */
        printf("[TUYA] barge-in confirm 3/3 (sums=%u,%u,%u)\r\n", sums[0], sums[1], sums[2]);
        return 1;
    }
    return 0;   /* 失败:帧已喂引擎/入历史,不浪费;sums 不打(误触发每秒数次,太吵) */
}
#endif

#if defined(TUYA_BARGE_IN_ENABLE) && defined(TUYA_KWS_ENABLE) && defined(TUYA_MUSIC_ENABLE)
/* TTS 播放期抢答的检测+处置(对齐 music_handoff 的"停播→排空→判后续")。
 * 2026-09-06 实测故事 TTS 中喊唤醒词 5 次全失败,两种死法:
 * ① 能量确认过不了——TTS 连续人声下 AEC 双讲抑制把重叠话音整句压到 60 万
 *   门限下,本地 VAD 明明开着却整句被判"energy low ignored",故事照播、
 *   毫无反应;② 确认过了(往往词已说完)盲目起 prefill 轮——prefill 全是
 *   故事回声残响+被削损的词,ASR 空→云端不回,故事白死+死寂。
 * 修法两层:
 * ① 触发放宽:能量 3/3 之外,加"VAD 连续开 ≥500ms 且期间帧能量峰值 ≥50万"
 *   旁路。★ VAD 会被 TTS 回声打开(回声也是人声形状,AEC 削得了平均能量
 *   削不掉峰值)——2026-09-06 三轮实测:(a)故事前奏纯回声开 VAD 600ms+,
 *   峰值 25.8万;(b)走完"放歌→唤醒停乐→恢复TTS"流程后 AEC 整体劣化(音频
 *   设备被切到 48k/src=1 没回去),故事期回声均值 15-25万、尖峰 52-57万,
 *   恰好过 50万底线,没人说话也把故事假"唤醒"杀掉两次。能量维度上它与
 *   双讲压弱的真人声重叠,没有可靠分界——ignored/触发都打 peak,靠日志校。
 * ② 处置分两级。confirm(3×60万,响亮真人声)→ 立即全套:chat_break+停播
 *   +哑火兜底(原行为)。persistent 旁路 → 先只"本地静音"(清 rbuf 停喇叭,
 *   不 chat_break,云端 TTS 继续进 rbuf)+排空喂引擎+判后续:引擎认出唤醒词
 *   (真词必让引擎 WAKE,回声从不——同日实测 4 真全中/2 假全不中,k27+k08
 *   序列是唯一可靠判别)或有后续话音 → 此刻才 chat_break 按唤醒/命令收尾;
 *   都没有 → 回声假警报,恢复播放(rbuf 里故事还在,跳过静音 ~1s 接着播),
 *   冷却加长 5s 压劣化态 ~2s 一簇的回声。
 * 返回:0=未触发/假警报已恢复播放(调用方继续轮询);非0=已处置完(抢答轮
 * 已布防 g_barge_in=1,或已按唤醒收尾 g_turn_done/g_wake_break 置位回空闲),
 * 调用方 break。*/
#define TUYA_TTS_VAD_PERSIST_MS     500u    /* VAD 连续开旁路的时长门槛:唤醒词 ~0.6s(实测 0.62/0.66s) */
#define TUYA_TTS_PERSIST_MIN_ENERGY 500000u /* 同一旁路的能量底线:实测 TTS 回声窗口峰值 25.8万
                                             * (2026-09-06),取 ~2 倍防更响的段落;它与 60万确认门
                                             * 之间的窄带就是旁路的价值:峰值够高但凑不齐连续 3 帧 */
#define TUYA_TTS_ECHO_COOLDOWN_MS   5000u   /* 旁路假警报恢复播放后的加长冷却:AEC 劣化态回声 ~2s
                                             * 一簇(2026-09-06 放歌流程实测),常规 1s 压不住,会连环
                                             * "静音验证→跳 1s"把故事剪碎 */
#define TUYA_TTS_VERIFY_MUTE_MS     2000u   /* 旁路静音验证的"真静音"时长:光清 rbuf 喇叭几十 ms 就
                                             * 复响(云端 TTS 继续推包,2026-09-06 二轮实测 3 次验证
                                             * 全被复响回声顶成"有后续"误判),必须同时开丢包窗让
                                             * 验证期喇叭真停。覆盖排空 8 帧(~0.3s)+followup 判定
                                             * (≤1.5s);假警报恢复 = 跳过这段(~≤2s)接着讲。*/
static unsigned int g_tts_vad_open_since;   /* TTS 期 VAD 连续开口起点(0=已关);三处轮询点共用 */
static unsigned int g_tts_vad_peak;         /* 本次 VAD 连开期间确认帧的能量峰值(取自 g_barge_cfm_maxsum) */
static int tts_barge_poll(tai_ctx_t *ctx, const char *tag)
{
    if (!get_recoder_state() || !barge_cooldown_expired()) {
        g_tts_vad_open_since = 0;   /* VAD 关/冷却中:连续开计时清零 */
        g_tts_vad_peak = 0;
        return 0;
    }
    if (!g_tts_vad_open_since) {
        g_tts_vad_open_since = timer_get_ms();
        g_tts_vad_peak = 0;
    }
    int confirmed = barge_in_energy_confirmed();   /* 读走的帧已喂引擎+入历史,不白吃 */
    if (g_barge_cfm_maxsum > g_tts_vad_peak) {
        g_tts_vad_peak = g_barge_cfm_maxsum;   /* 连开期间的能量峰值:旁路判"人声"的依据 */
    }
    int persistent = (timer_get_ms() - g_tts_vad_open_since) >= TUYA_TTS_VAD_PERSIST_MS &&
                     g_tts_vad_peak >= TUYA_TTS_PERSIST_MIN_ENERGY;
    if (!confirmed && !persistent) {
        printf("[TUYA] barge-in%s: VAD fired but energy low (peak=%u), ignored\r\n", tag, g_tts_vad_peak);
        return 0;
    }
    if (confirmed) {
        printf("[TUYA] barge-in%s: energy confirm (peak=%u) → chat_break + stop TTS\r\n", tag, g_tts_vad_peak);
        tuya_break_send(ctx);          /* 通知云端中止本轮 TTS */
        g_tts_playing = 0;
        g_tts_drop_until = timer_get_ms() + 3000;   /* 旧轮残包排空窗(同 wake_break_tts):
                                                     * chat_break 够不到更早的轮,残包还会流 ~1.2s */
        g_tts_drop_cnt = 0;
    } else {
        /* persistent 旁路:可能是双讲压弱的真人声,也可能是 AEC 劣化后的回声
         * 尖峰(见头注释①b)——先只本地静音验证,不 chat_break、g_tts_playing
         * 保持 1,验证不过可原样恢复播放。★光清 rbuf 不算静音:云端 TTS 继续
         * 推包,喇叭几十 ms 就复响,复响回声会被 followup 判成"有后续"(实测),
         * 必须同时开丢包窗——验证期喇叭真停,此时 VAD 连续开=真人声。*/
        printf("[TUYA] barge-in%s: VAD persistent (peak=%u) → mute & verify (echo check)\r\n", tag, g_tts_vad_peak);
        g_tts_drop_until = timer_get_ms() + TUYA_TTS_VERIFY_MUTE_MS;
        g_tts_drop_cnt = 0;
    }
    _device_rbuf_clear();   /* confirm=清 cbuf 停喇叭;verify=清 cbuf+丢包窗真静音(g_tts_playing=1 可恢复) */
    g_tts_vad_open_since = 0;
    g_barge_cooldown_until = timer_get_ms() + TUYA_BARGE_COOLDOWN_MS; /* 冷却:压住在途残包二次触发 */
    /* 静音/停播后排 ~8 帧残响喂引擎+入历史:双讲下被削的词尾落在里面,安静后引擎
     * 有机会补认(music_handoff 同款);词尾若在此触发 on_wake,提示音已就地播。*/
    unsigned char _bt[TUYA_OPUS_FRAME_LEN];
    for (int i = 0; i < 8 && !g_exit; i++) {
        if (_device_get_voice_data(_bt, sizeof(_bt)) == sizeof(_bt)) {
            tuya_kws_feed(_bt, sizeof(_bt));
            barge_hist_push(_bt);    /* 残响若是命令尾部,一并进 prefill 补发 */
        }
    }
    int is_wake = 0, wake_play_prompt = 0, is_followup = 0;
    if (g_wake_prompt_pending) {      /* 排空期引擎把词认出来了(on_wake 播放态置的
                                       * pending):提示音还没播,下面补播 */
        g_wake_prompt_pending = 0;
        is_wake = 1;
        wake_play_prompt = 1;
    } else if (g_wake_hit) {          /* on_wake 已就地播过提示音+开吞咽窗(空闲态命中) */
        g_wake_hit = 0;
        is_wake = 1;
    } else if (barge_followup_wait(get_recoder_state() ? 0 : timer_get_ms())) {
        /* 判后续(种子=排空末尾 VAD 已关的起点,排空耗时计入关闭时长),判法见
         * barge_followup_wait:开 300ms=命令,关 350ms=说完了。*/
        is_followup = 1;
    } else if (!confirmed) {
        /* 旁路既没等来引擎认词、也没等来后续话音:回声假警报(见头注释②)。
         * 恢复播放:丢包窗到期后云端包重新进 rbuf 接着播,只跳过静音验证的
         * ~2s;冷却加长,压住劣化态 ~2s 一簇的回声连环再触发。*/
        printf("[TUYA] verify failed: no wake / no follow-up → echo false alarm, resume TTS\r\n");
        g_barge_cooldown_until = timer_get_ms() + TUYA_TTS_ECHO_COOLDOWN_MS;
        return 0;                     /* g_tts_playing 未动:调用方继续轮询播放 */
    }
    if (!confirmed) {                 /* 旁路验证通过(认出词/有后续):此刻才真正作废本轮 */
        tuya_break_send(ctx);
        _device_rbuf_clear();         /* 验证期间(≤1.5s)进了 rbuf 的旧轮音频一并清掉 */
        g_tts_playing = 0;
        g_tts_drop_until = timer_get_ms() + 3000;
        g_tts_drop_cnt = 0;
    }
    if (is_wake) {
        if (wake_play_prompt) {
            printf("[TUYA] wake alert: WakeHeyTuya.mp3\r\n");
            app_music_tuya_play_wake_prompt();
        }
        g_turn_done = 1;
        g_wake_break = 1;             /* 本轮已作废:跳过 ④ 收尾/pending 音乐,回空闲 */
        return 1;
    }
    if (is_followup) {
        printf("[TUYA] follow-up speech → uplink barge turn\r\n");
        barge_hist_to_prefill();      /* 排空前后的话音整体转 prefill:不补发云端只听到尾部 */
        g_barge_in = 1;
        g_barge_prompt_wait = 1;      /* 安全网:起轮失败时 1.5s 内补播,起轮当拍即撤销 */
        g_barge_prompt_deadline = timer_get_ms() + 1500u;
        return 1;                     /* 调用方 break → 主循环跳过收尾直接起新轮 */
    }
    /* confirm 但无后续:多半是没被引擎认出的唤醒词(说得轻/被压狠),哑火补救 */
    printf("[TUYA] no follow-up → drop barge turn (likely wake, prompt via safety-net)\r\n");
    g_barge_prompt_wait = 1;          /* 哑火补救:主循环下一拍播"我在"+开 15s 追问窗 */
    g_barge_prompt_deadline = timer_get_ms();   /* VAD 已关 ≥350ms,到点即播 */
    g_turn_done = 1;
    g_wake_break = 1;
    return 1;
}
#endif

#ifdef TUYA_UPLINK_OPUS_ENABLE
/* 上行编码发送:PCM 整帧(1280B = 640 采样 = 40ms)→ opus 包(CBR 16kbps 下 ~80B)→ TAI。
 * mic 管线/VAD/能量门全工作在 PCM 上,这里是"发出去前"的唯一编码点(含 barge-in
 * onset 补发路径)。编码失败只丢本帧(定点路径实际无失败分支),不废链路。
 * 返回 TAI_OK / 发送错误码,与 tai_send_audio_chunk 一致。*/
static int tuya_uplink_send_frame(tai_ctx_t *ctx, unsigned char *pcm1280)
{
    unsigned char opkt[TUYA_OPUS_PKT_MAX];
    int n = tuya_opus_enc_frame((const short *)pcm1280, opkt, sizeof(opkt));
    if (n <= 0) {
        return TAI_OK;   /* 丢帧保链路:见上,实际不可达 */
    }
    return tai_send_audio_chunk(ctx, opkt, (size_t)n);
}
#endif

/* 单次 AI 会话:get_session_token → tai_connect → 语音循环(直到 on_disconnect
 * 置 g_exit 或发送失败置 g_link_broken)。返回会话存活时长 ms——supervisor
 * (tuya_ai_run)据此复位重连退避。任何失败都【不碰 iot】:激活凭证/MQTT 由
 * supervisor 跨会话持有,这里只回收 tai 侧资源(token/mem/连接)。*/
static unsigned int tuya_ai_session(const pal_t *pal, iot_client_t *iot, const char *local_key)
{
    unsigned int t_start = timer_get_ms();
    char *token = (char *)malloc(4096);
    if (!token) return 0;
    if (iot_client_get_session_token(iot, NULL, token, 4096) != 0 || !token[0]) {
        /* 云端偶发 5xx / 网络未恢复:不 deinit iot,交 supervisor 退避重试 */
        printf("[TUYA] get_session_token fail (will retry)\r\n"); free(token); return 0;
    }
    tai_conn_params_t cp;
    tai_bind_session_token(token); /* stm(UDP)后端用;tcp 后端空操作 */
    if (parse_token(token, &cp) != 0) {
        printf("[TUYA] parse_token fail (will retry)\r\n"); free(token); return 0;
    }
    if (cp.biz_code == 0) cp.biz_code = 65537;
    if (cp.biz_tag  == 0) cp.biz_tag  = 119;
    printf("[TUYA] TAI cfg: biz_code=%ld biz_tag=%ld host=%s:%u sni=%s agentToken=%s\r\n",
           cp.biz_code, cp.biz_tag, cp.host, cp.port, cp.tls_sni,
           cp.agent_token[0] ? "(set)" : "(none)");   /* token 是会话凭证,只打有无不打值 */
    free(token);
    /* MQTT 常驻:不再 deinit。让 MQTT 保持在线,接收云端 DP 下行(如"运动一下"的自定义DP)。
     * 之前注释说"并发冲突/SESSION_CLOSE",经查 MQTT 和 AI 各有独立 TCP 连接,
     * TuyaOpen 也是 MQTT+AI 并存不断开。如果实测出现 SESSION_CLOSE,再改回 deinit。*/
    /* iot_client_deinit(iot); */

    /* 下行 TTS 编码开关 TUYA_DOWNLINK_OPUS_ENABLE(app_config.h,默认关=PCM):
     *   开 = 请求 opus(~2KB/s,治拥挤网卡顿;云端确认支持 codec=111,帧 80B/16kbps/40ms,解码仍在调);
     *   关 = PCM(稳定能播,32KB/s)。上行 ASR 始终 PCM(opus format_mode 无标准裸包,不赌正常 ASR)。*/
#ifdef TUYA_DOWNLINK_OPUS_ENABLE
    /* 音量 MCP 已实现。MCP 回应由 mcp_resp_pump 在 demo 任务发送，不在 worker
     * 回调内重入会话锁，因此 initialize/tools/call 可与 AUDIO 上行并存。 */
    static const char SA[] =
        "{\"deviceMcp\":{\"supportCustomMCP\":true},"
        "\"tts.order.supports\":[{\"format\":\"opus\",\"sampleRate\":16000,"
        "\"bitDepth\":\"16\",\"channels\":1}]}";
#else
    static const char SA[] = "{\"deviceMcp\":{\"supportCustomMCP\":true}}";
#endif
#ifdef TUYA_SERVER_VAD_ENABLE
    /* chatAttributes 保持 8-31 最后一次流式 ASR 成功会话的原始字节(字符串布尔,
     * 无 sys.workflow)。同节点 rtc-ai1-5 对照:该格式云端正常流式返回 ASR;
     * 换成原生 true+workflow 后(9-2)云端对整个 AUDIO 事件零响应(连空 ASR/
     * server-VAD 都没有),疑 STM 通道 schema 严格校验拒绝事件,故回退。 */
    static const char EU[] =
        "{\"asr.enableVad\":\"true\","
        "\"tts.alternate\":\"true\","
        "\"processing.interrupt\":\"true\"}";
#else
    static const char EU[] =
        "{\"sys.workflow\":\"asr-llm-tts\","
        "\"tts.alternate\":\"true\"}";
#endif
    printf("[TUYA] chat attrs: %s\r\n", EU);
    demo_ctx_t dc = {0};
    tai_config_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.host = cp.host; tc.port = cp.port; tc.tls_sni = cp.tls_sni;
    tc.device_id = cp.derived_client_id; tc.local_key = local_key;
    tc.protocol_version = TAI_VER_21; tc.client_type = TAI_CLIENT_DEVICE;
    tc.sign_level = TAI_SIGN_HMAC_SHA256;
    tc.biz_code = (uint32_t)cp.biz_code; tc.biz_tag = (uint64_t)cp.biz_tag;
    tc.agent_token = cp.agent_token;
    tc.session_attrs_json = SA; tc.event_user_data_json = EU;
    tc.pal = pal; tc.on_text = on_text; tc.on_audio = on_audio;
    tc.on_event = on_event; tc.on_disconnect = on_disconnect; tc.user_data = &dc;

    /* A reconnect must not send a reply that belongs to the retired session. */
    tuya_mcp_reset();
    void *mem = pal->malloc(tai_ctx_size());
    if (!mem) return 0;
    tai_ctx_t *ctx = tai_ctx_init(mem, &tc);
    if (!ctx) { pal->free(mem); return 0; }
    printf("[TUYA] tai_connect...\r\n");
    if (tai_connect(ctx) != TAI_OK) {
        printf("[TUYA] tai_connect fail (will retry)\r\n");
        tai_ctx_deinit(ctx); pal->free(mem); return 0;
    }

    /* ===== 阶段3:免唤醒语音对话(本地 VAD 驱动)=====
     * 会话状态标志由 supervisor 在每次尝试前复位;音频流(mic 采集 + DAC 播放)
     * 也由 supervisor 只起一次、跨会话存活(重连期间采集照跑,cbuf 环形覆盖无害)。
     * 循环:本地VAD检测开口 → 上行(PCM 1280B/40ms,opus 时逐帧编码成 ~80B 包)
     * → 停说收尾 → 云端回复(on_audio 播TTS)→ 重新待命。
     * 回声抑制:TTS 期间 g_tts_playing=1 暂停上行,播完冷却 ~300ms 再听。 */
    g_audio_ready        = 1;
    g_tts_playing        = 0;
    g_turn_done          = 1;          /* 视为"上一轮已结束",直接进入听音 */
    g_uplinking          = 0;          /* 上行窗/回执对冲计数:会话起点清零,防上次会话残留 */
    g_local_break_pend   = 0;
    g_audio_frame_logged = 0;
#ifdef TUYA_MUSIC_ENABLE
    g_music_playing      = 0;
    tuya_music_reset();                /* 清上一会话残留的半截文本流/未消费的音乐结果 */
#endif
#ifdef TUYA_UPLINK_OPUS_ENABLE
    /* 上行 opus 编码器:幂等 init(编码器常驻堆,跨会话/重连复用);失败自动回退
     * PCM 上行(仍能对话,只是带宽大),不废会话。 */
    int use_opus_uplink = (tuya_opus_enc_init() == 0);
    g_use_opus_uplink = use_opus_uplink;   /* music_handoff 音乐期上行与主循环同编码形态(见声明注释) */
    if (!use_opus_uplink) {
        printf("[TUYA] opus enc init fail -> uplink fallback PCM\r\n");
    }
#else
    const int use_opus_uplink = 0;
#endif
    printf("[TUYA] voice loop ready (local-VAD, uplink=%s 16k/mono)"
#ifdef TUYA_STREAM_MODE
            " STREAM-MODE"
#endif
#ifdef TUYA_DOWNLINK_OPUS_ENABLE
            " downlink=opus"
#else
            " downlink=pcm"
#endif
#ifdef TUYA_MUSIC_ENABLE
            " +music"
#endif
            "\r\n",
           use_opus_uplink ? "opus" : "pcm");

    /* MQTT 心跳线程由 supervisor(tuya_ai_run)启动,生命周期跨会话。
     * 不能在语音循环里调 iot_client_process——它内部 TLS recv 会阻塞语音线程。*/

    /* ===== 对照实验:文本通道探针(音频流已起,若云端回 TTS 可顺带验证下行播放)=====
     *   文本正常回复 → 会话通,问题锁定在音频上行(云端 ASR 不认我们的 opus)
     *   文本回空/超时 → 会话或 biz_code/agent 配置问题(语音也跟着废)
     * ⚠️ 默认关闭(TUYA_PROBE_ENABLE 未定义):探针会阻塞 ~2s 等云端文本回复,期间不收音,
     *   拖慢开机到可对话的延迟。功能已验证通,仅调试会话问题时手动开启。*/
#ifdef TUYA_PROBE_ENABLE
    {
        const char *probe = "你好";
        printf("[TUYA] PROBE send-text: \"%s\"\r\n", probe);
        dc.got_done = 0; g_turn_done = 0;
        if (tai_send_text(ctx, probe, strlen(probe)) == TAI_OK) {
            int w = 0;
            while (!dc.got_done && w < 10000) { msleep(100); w += 100; }
            printf("[TUYA] PROBE result: %s (waited %dms)\r\n",
                   dc.got_done ? "GOT_REPLY" : "TIMEOUT", w);
        } else {
            printf("[TUYA] PROBE: tai_send_text fail\r\n");
        }
        _device_wbuf_clear();   /* 探针期间 mic 采到环境音/自身 TTS,清掉再进语音循环 */
    }
#endif

    /* ★ aligned(4) 不能省:char 数组对齐默认 1,LTO 后 alloca 仍是 align 1,栈布局恰把它放
     *   到奇地址时,转 short* 进 opus 编码器(biquad 做 16bit load)→ pi32v2 misalign_err
     *   崩溃(2026-08-31 UDP 构建实测:寄存器 R0=奇地址 + 最终 IR alloca align 1 双证实;
     *   TCP 旧构建没崩纯属栈布局运气)。所有要转 short* 进 opus 的缓冲都必须 4 对齐。*/
    unsigned char abuf[TUYA_OPUS_FRAME_LEN] __attribute__((aligned(4)));
    /* 上行诊断:本轮帧计数/有效帧数 + 空闲排空的底噪基线 */
    unsigned int uplink_frames = 0, uplink_active = 0;
#ifdef TUYA_CLOUD_OPEN_ENABLE
    /* 纯云端 telemetry:open_e=开流帧能量(长流=0,仅方案C 有)、stop=收流/事件
     * 切换原因(长流恒 cloud-end)、turn_start_ms=当前事件时长计时。*/
    unsigned int turn_start_ms = 0, turn_open_e = 0;
    const char *turn_stop = "-";
#ifndef TUYA_STREAM_MODE
    int turn_had_tts = 0;             /* 收尾冷却时长依据(1s/2s),仅方案C 轮次制用 */
#endif
#endif
    unsigned int start_fails = 0;      /* audio_start 连续失败计数:≥5(≈1s)判链路死 */
#ifndef TUYA_STREAM_MODE
    unsigned int idle_cnt = 0, idle_sum = 0, idle_act_sum = 0;   /* 空闲排空统计 */
#endif
#ifndef TUYA_STREAM_MODE
    /* 自适应开门底噪跟踪(见 TUYA_OPEN_GATE_K 注释):idle 排空帧累积成批,
     * 环内 8 批取中位数=稳态底噪,gate=clip(4万,1.8×底噪,60万)。
     * 长流模式不用(v3:会话入口常开流,零本地能量测量)。 */
    static unsigned int idlef_cnt = 0, idlef_sum = 0;
    static unsigned int idlef_ring[TUYA_OPEN_GATE_NBAT];
    static unsigned int idlef_n = 0, idlef_idx = 0;
    static unsigned int g_idle_floor = 0;
    static unsigned int g_open_gate = TUYA_OPEN_ENERGY_MIN;   /* 首批样本前=安静闸 4万 */
#endif
#ifdef TUYA_SERVER_VAD_ENABLE
    /* STM 当前不能稳定区分 server-VAD 指令。历史成功轮的 ASR 是流式返回，
     * 无需等待云端 stop；本地 VAD 已做 600ms debounce，直接 fin 可避免事件
     * 长时间不闭合及用户再次说话把2秒静音计数清零。 */
    #define LOCAL_SILENCE_TIMEOUT_FRAMES  1
#ifndef TUYA_STREAM_MODE
    unsigned int silence_frames = 0;   /* 长流永不主动收流,不计数(仅方案C 用) */
#endif
#endif

#ifdef TUYA_STREAM_MODE
#if !defined(TUYA_SERVER_VAD_ENABLE) || !defined(TUYA_CLOUD_OPEN_ENABLE)
#error "TUYA_STREAM_MODE needs TUYA_SERVER_VAD_ENABLE + TUYA_CLOUD_OPEN_ENABLE (app_config.h)"
#endif
    /* ===== 长流模式 v3(2026-09-23 定稿,纯云端,完全对齐小智;开关与决策依据见
     * app_config.h 头注释)=====
     * 会话建立即开流,audio_start 只在会话入口发一次,逐帧上传
     * 直到会话退出(g_exit/断链),TTS 播放期间照常逐帧上传(连续上流正是云端
     * 打断判定的输入)。设备侧零信号判定——没有关态、没有本地 VAD 触发、没有
     * 能量门、没有收流计时器,唯一职责=把 mic 帧搬上云 + 执行云端指令(打断=
     * 停喇叭);开口/停说/话音段切分/打断判定 100% 云端(成本已明确不考虑,
     * 24/7 上行)。
     * 云端 END/CHAT_BREAK 一概不碰上行流(不 end、不 restart,同事件续推)——
     * 官方文档《VAD 与打断处理》明文:云端 VAD 模式 start 全会话只调 1 次,
     * 收到回合结束信号后调 audio_end 是错误用法(主动结束当前 Event,云端截断
     * 用户语音)。旧实现"END 后立刻 end+start 重开"每空轮换一个新事件,云 VAD
     * 无噪声自适应历史(新事件=抢首字最灵敏态)→ 残渣顶开段 → 空ASR → END →
     * 再重开的自持风暴,2026-09-24 定罪删除(实测佐证:每条空 ASR 的 event-id
     * 各不相同,即此循环的指纹)。
     * KWS/本地 barge-in/话音头 prefill 等本地判定一概不用(全部交云;KWS 本就
     * 未启用;常开流不存在"话音头没赶上"问题,prefill 无用武之地)。
     * ★ 9.22 实测根因对照:轮次制下噪声起轮把云端 VAD 事件花掉(持续噪声无静默隙,
     *   VAD 被钉在触发态、无 falling edge,轮中真话音拿不到新 begin)→10dB 检出率
     *   钉死 72%;长流(竞品同涂鸦云)90.3%。噪声场景误检(云端 VAD 是绝对电平门,
     *   我们 10dB 底噪 17.7万 高于它的门)预计仍差——那是上行电平/降噪问题,不是
     *   架构问题,下一变量(前端处理链)再攻。
     * 日志标记与方案C 同格式(speak-start / turn: open_e=),统计脚本可直接沿用;
     * 新增标记:speak-stop: server-vad (stream continues)=云端VAD话音段结束(流不断),
     * cloud barge-in=云端打断停播,turn: stop=cloud-end=云端事件切换。
     * ★ 2026-09-24 追加 TUYA_STREAM_PLAYBACK_GATE(app_config.h 开关),修订上文
     *   "播放期照常逐帧上传"与"零能量门"两处断言:TTS 播放期 AEC 残差(中位
     *   ~12万/最坏 38.4万)全量直达云 VAD(远场级灵敏),9.23 晚场 80.2% 空轮
     *   在播放期。播放期扣帧,3 帧 ≥60万(方案C 同门)确认真人插话才开门并
     *   回溯补发话音头,真声停 1s 重扣;非播放期仍零门零计时,轮次判定依旧
     *   100% 云端——这是对"云 VAD 听得见自家喇叭"的工程折中,不是回到方案C。*/
    {
    /* 方案C 的本地打断/开流判定件套(tuya_break_send/barge_in_energy_confirmed/
     * barge_cooldown_expired/barge_hist_push/barge_in_prefill_arm)与开口冷却
     * g_open_cooldown_until 在长流模式下不再被调用(判定全交云端、无冷却):
     * 显式引用一次压掉 -Wunused-function/-Wunused-variable,别给构建添告警噪音。
     * (TUYA_STREAM_PLAYBACK_GATE 下 barge_hist_push/barge_in_prefill_arm 被播放
     *  期回声闸复用,不再压——见下方主循环闸状态机。) */
    (void)tuya_break_send; (void)barge_in_energy_confirmed; (void)barge_cooldown_expired;
#ifdef TUYA_STREAM_PLAYBACK_GATE
    (void)g_open_cooldown_until;
#else
    (void)barge_hist_push; (void)barge_in_prefill_arm; (void)g_open_cooldown_until;
#endif
    /* ---- 会话建立即开流(小智同款):清掉开机以来积压的 mic 帧从"此刻"起流;
     *      audio_start 失败原地重试,连续 ≥5 次(≈1s)=TCP 半死,报废会话交
     *      supervisor 重连 ---- */
    _device_wbuf_clear();
    printf("[TUYA] speak-start: uplink begin (session start)\r\n");
    while (!g_exit && !g_link_broken &&
           tai_send_audio_start(ctx,
#ifdef TUYA_UPLINK_OPUS_ENABLE
                                use_opus_uplink ? TAI_AUDIO_OPUS : TAI_AUDIO_PCM,
#else
                                TAI_AUDIO_PCM,
#endif
                                1, 16, 16000) != TAI_OK) {
        printf("[TUYA] tai_send_audio_start fail\r\n");
        if (++start_fails >= 5) {   /* 连续失败:TCP 半死,别原地空转 */
            printf("[TUYA] audio_start failed x%u, link dead\r\n", start_fails);
            g_link_broken = 1;
        }
        msleep(200);
    }
    if (!g_exit && !g_link_broken) {
        start_fails = 0;
        turn_open_e = 0;            /* 开流帧能量:会话入口开流无触发帧(仅方案C 有值) */
        turn_stop = "-";
        turn_start_ms = timer_get_ms();
        uplink_frames = 0; uplink_active = 0;
        g_turn_done = 0;            /* 清会话入口置的"上一轮已结束"标记与残留 */
        g_server_vad_stop = 0;
        g_cloud_break_evt = 0;
#ifdef TUYA_STREAM_PLAYBACK_GATE
        s_play_gate_open = 0; s_play_gate_confirm = 0;   /* 回声闸归位:上一会话
        s_play_gate_quiet = 0; s_play_gate_hold = 0;     * 可能死在播放期开门态 */
        s_play_gate_blind = 0; s_play_gate_was_busy = 0; s_play_gate_last_hi = 0;
#endif
        OS_ENTER_CRITICAL();
        strncpy(g_stale_end_bizid, g_answer_bizid, sizeof(g_stale_end_bizid) - 1);
        g_stale_end_bizid[sizeof(g_stale_end_bizid) - 1] = '\0';
        OS_EXIT_CRITICAL();
#if defined(TUYA_TRANSPORT_STM_ENABLE) && TUYA_TRANSPORT_STM_ENABLE
        g_turn_event_id[0] = '\0';
#else
        OS_ENTER_CRITICAL();
        {
            const char *_eid = tai_current_event_id(ctx);
            strncpy(g_turn_event_id, _eid ? _eid : "", sizeof(g_turn_event_id) - 1);
            g_turn_event_id[sizeof(g_turn_event_id) - 1] = '\0';
            g_uplinking = 1;
        }
        OS_EXIT_CRITICAL();
#endif
        g_tts_drop_until = 0;   /* 新流起:关闭旧流残包排空窗 */
    }
    while (!g_exit && !g_link_broken) {
        tai_log_flush();       /* 每帧节拍刷库日志(与方案C 上行循环同款) */
        mcp_resp_pump(ctx);    /* MCP initialize/response 与 AUDIO 上行并存(TCP) */
        int n = _device_get_voice_data(abuf, TUYA_OPUS_FRAME_LEN);
        if (n == TUYA_OPUS_FRAME_LEN) {
            unsigned int fs, fa;
            int send_now = 1;
            opus_frame_stat(abuf, TUYA_OPUS_FRAME_LEN, &fs, &fa);
#ifdef TUYA_STREAM_PLAYBACK_GATE
            /* ---- TTS 播放期回声闸(方案2,2026-09-24):长流把本地门拆了,AEC
             *      残差(中位 ~12万/最坏 38.4万)全量直达云 VAD(远场级灵敏),
             *      9.23 晚场 80.2% 空轮在播放期。播放期扣帧入 barge 历史,连续
             *      3 帧 ≥60万(方案C 同门:残差最坏 38.4万的 1.6×,真人确认帧
             *      115.9万 的 1/1.9)判真人插话→开门并回溯补发话音头(8万 闸裁
             *      回声头,防"打断只收到尾音"),之后逐帧实时上行供云端打断
             *      判定;真声停 25 帧(<40万)重扣,防 TTS 尾音残差续漏。
             *      ★ v2(2026-09-24 箱测"自说自话永动机"定位后两处修正):
             *      ① 播放判据加下行 cbuf 水位——g_tts_playing 只是"下行流"标志,
             *        下载跑赢实时播放,流 END 后喇叭还压着数秒缓冲尾巴(箱内实测
             *        END 后 4-5s 残差 26万 直上云,云听了就接话,永动机燃料);
             *        缓冲排空(≈DAC 播完,方案C 同判据 640)才算播完。
             *      ② 起播盲窗:起播头 8 帧(~320ms)是 AEC 未收敛爆发期(工位
             *        实测起播后 80ms 即 3×60万 自开门),丢帧不喂历史不攒确认;
             *        上一帧刚有人声(≥60万)则不盲——快速对话里答话追着插话,
             *        别把真人话头剪了。
             *      真播完即归位,非播放期零改动,云端轮次判定路径完全不变。 ---- */
            int _pg_busy = (g_tts_playing || _device_get_play_level() >= 640);
            if (_pg_busy && !s_play_gate_was_busy) {
                s_play_gate_blind = s_play_gate_last_hi ? 0 : 8;   /* 起播沿→上盲窗 */
            }
            s_play_gate_was_busy = _pg_busy;
            s_play_gate_last_hi = (fs >= BARGE_CONFIRM_ENERGY);
            if (_pg_busy && s_play_gate_blind > 0) {
                s_play_gate_blind--;            /* 盲窗帧:扣掉丢弃,不喂历史不攒确认 */
                send_now = 0;
            } else if (_pg_busy) {
                barge_hist_push(abuf);          /* 扣帧先入 45 帧回溯历史 */
                s_play_gate_hold++;
                if (!s_play_gate_open) {
                    /* 扣帧态:攒确认,不上云;确认帧经 prefill 回溯补发,不丢 */
                    if (fs >= BARGE_CONFIRM_ENERGY) {
                        if (++s_play_gate_confirm >= 3) {
                            unsigned int k;
                            s_play_gate_open = 1;
                            s_play_gate_quiet = 0;
                            barge_in_prefill_arm();   /* 历史→prebuf,8万 闸裁回声头 */
                            printf("[TUYA] play-gate: voice confirmed, flush %u prefill\r\n",
                                   g_barge_prefill);
                            for (k = 0; k < g_barge_prefill && !g_link_broken; k++) {
#ifdef TUYA_UPLINK_OPUS_ENABLE
                                int _ps = use_opus_uplink
                                          ? tuya_uplink_send_frame(ctx, &g_barge_prebuf[k * 1280])
                                          : tai_send_audio_chunk(ctx, &g_barge_prebuf[k * 1280],
                                                                 TUYA_OPUS_FRAME_LEN);
#else
                                int _ps = tai_send_audio_chunk(ctx, &g_barge_prebuf[k * 1280],
                                                               TUYA_OPUS_FRAME_LEN);
#endif
                                if (_ps != TAI_OK) {
                                    g_link_broken = 1;  /* 同主发送:断链即报废会话 */
                                } else {
                                    uplink_frames++;
                                }
                            }
                            g_barge_prefill = 0;
                            if (g_link_broken)
                                break;          /* 断链:与主发送失败同路径退出循环 */
                        }
                    } else {
                        s_play_gate_confirm = 0;
                    }
                    send_now = 0;   /* 未开门:帧留历史,不上云 */
                    if (s_play_gate_hold % 25 == 0)   /* 播放期 ~1s 一条扣帧遥测 */
                        printf("[TUYA] play-gate: hold sum=%u (confirm=%u/3)\r\n",
                               fs, s_play_gate_confirm);
                } else {
                    /* 开门态:实时上行;真声停 25 帧(<40万=残差最坏之上)重扣 */
                    if (fs < TUYA_PLAY_GATE_REGATE_ENERGY) {
                        if (++s_play_gate_quiet >= 25) {
                            s_play_gate_open = 0;
                            s_play_gate_confirm = 0;
                            s_play_gate_quiet = 0;
                            printf("[TUYA] play-gate: re-close after 1s quiet\r\n");
                            send_now = 0;   /* 重扣帧起回闸,不再实时上云 */
                        }
                    } else {
                        s_play_gate_quiet = 0;
                    }
                }
            } else {
                /* 真播完归位(流结束且缓冲排空):闸状态全清,回到常开流(纯云端基线) */
                if (s_play_gate_open)
                    printf("[TUYA] play-gate: TTS over, uplink stays open\r\n");
                s_play_gate_open = 0;
                s_play_gate_confirm = 0;
                s_play_gate_quiet = 0;
                s_play_gate_hold = 0;
                s_play_gate_blind = 0;
            }
#endif
            if (send_now) {
                /* ---- 逐帧上传:非播放期照常(小智同款,停流会毁云端打断检测,
                 *      连续上流就是打断判定的输入);播放期仅真人插话确认后放行 ---- */
#ifdef TUYA_UPLINK_OPUS_ENABLE
                int _snd = use_opus_uplink ? tuya_uplink_send_frame(ctx, abuf)
                                           : tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN);
#else
                int _snd = tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN);
#endif
                if (_snd != TAI_OK) {
                    printf("[TUYA] tai_send_audio_chunk fail\r\n");
                    g_link_broken = 1;   /* 发送失败=链路断,快速报废本会话(supervisor 重连) */
                    break;
                }
                uplink_frames++;
                if (fa > 3) uplink_active++;   /* fact>3≈有效话音帧,与方案C 同度量 */
                if (uplink_frames % 25 == 1)   /* 40ms/帧,25帧≈1s 一条 */
                    printf("[TUYA] uplink f#%u sum=%u act=%u%%\r\n", uplink_frames, fs, fa);
            }
        }
        /* 云端判打断(回调只置标志:2=有东西在响/1=静音)。≤40ms 轮询延迟,人耳无感。
         * 2 且流还活着(g_tts_playing=1):停喇叭+清 rbuf+开 3s 残包排空窗(on_audio
         * 在窗内丢弃云端不再发的旧 TTS 流)。2 但流已 END(=2 判据的 cbuf 排空尾巴
         * 臂,或 END 恰落在回调→消费的 ≤40ms 间隙):只清 rbuf 即静音——流已终结无
         * 残包可排,新轮回话最快 ~0.6s 后到,开窗会把它吃掉(2026-09-23 "你好后没
         * 反应"根因)。1:仅记录(云端 asrInterrupt 不知道设备是否播完,静音期开窗
         * 同样吃新轮回话)。 */
        if (g_cloud_break_evt) {
            int _evt = g_cloud_break_evt;
            g_cloud_break_evt = 0;
            if (_evt == 2 && g_tts_playing) {
                printf("[TUYA] cloud barge-in: stop TTS (frames=%u)\r\n", uplink_frames);
                _device_rbuf_clear();
                g_tts_drop_until = timer_get_ms() + 3000;
                g_tts_drop_cnt = 0;
                g_tts_playing = 0;
            } else if (_evt == 2) {
                printf("[TUYA] cloud barge-in: stop drain tail (frames=%u)\r\n", uplink_frames);
                _device_rbuf_clear();   /* 尾巴静音即止;无残包,不开排空窗 */
            } else {
                printf("[TUYA] cloud break: no TTS playing, nothing to stop (frames=%u)\r\n",
                       uplink_frames);
            }
        }
        /* 云端 VAD 话音段结束(endpointing):信息性记录,流不断——下一句话音由
         * 同一条流继续上传,这正是长流模式治"噪声轮吃掉 VAD 事件"的要点。 */
        if (g_server_vad_stop) {
            g_server_vad_stop = 0;
            printf("[TUYA] speak-stop: server-vad (stream continues, frames=%u active=%u)\r\n",
                   uplink_frames, uplink_active);
        }
        /* 云端 END = 轮结束,不是事件拆除 —— 对齐小智 realtime:END 后不关流、
         * 不重开,同一事件继续推帧,轮次判定全由 server 内部 VAD 负责。
         * 旧实现"立刻 end+start 重开"已定罪删除(2026-09-24):每条空 ASR 云端
         * 回一次 END,我们就换一个新事件,云 VAD 在新事件上无噪声自适应历史、
         * 处于抢首字最灵敏态,残渣再次顶开段 → 空ASR → END → 再重开,自持出
         * 1-2s 一条的空 ASR 风暴(实测每条 eventId 都不同即此循环)。
         * event-id 不再变化,旧事件迟到 END ack 的 stale 判属天然失效,无副作用。
         * 风险备忘:若实测连问二/三轮时第二问哑了(END 真拆事件),再退回
         * 带能量门的重开方案。 */
        if (g_turn_done) {
            g_turn_done = 0;
            turn_stop = "cloud-end";
            /* 与方案C 同格式 telemetry(统计脚本沿用):open_e=开流帧能量(长流
             * 恒 0)/stop=轮结束原因/frames=本轮总帧数/act=有效帧占比/dur=时长 */
            printf("[TUYA] turn: open_e=%u stop=%s frames=%u act=%u%% dur=%ums (event kept open)\r\n",
                   turn_open_e, turn_stop, uplink_frames,
                   uplink_frames ? (uplink_active * 100u) / uplink_frames : 0u,
                   timer_get_ms() - turn_start_ms);
            uplink_frames = 0; uplink_active = 0;
            turn_open_e = 0; turn_stop = "-";
            turn_start_ms = timer_get_ms();
            g_tts_drop_until = 0;
        }
#ifdef TUYA_MUSIC_ENABLE
        /* 音乐技能交接:云回了音乐 SKILL 且当前无播报 → music_handoff 交接
         * app_music(阻塞至整首完/被云端信号停乐)。2026-10-08 纯云形态:音乐期
         * 逐帧照常上行在交接循环内完成(与主循环同形态),停乐由云端裁决
         * (chat_break/新轮回话两臂,详见 music_handoff);出口清一次录音积压只为
         * 丢掉交接瞬间的旧行残帧,不构成帧级门。 */
        if (tuya_music_pending() && !g_link_broken && !g_tts_playing &&
            _device_get_play_level() == 0) {
            music_handoff(ctx);
            _device_wbuf_clear();
            continue;
        }
        if (g_player_restore_pending && !g_tts_playing && _device_get_play_level() == 0) {
            /* music_handoff 推迟的播放器恢复:常开流没有"起轮点",在主循环里等
             * 喇叭空闲的时机恢复,保证回话 TTS 前就位 */
            g_player_restore_pending = 0;
            printf("[TUYA-MUSIC] restore tts player\r\n");
            _device_net_audio_play(1);
        }
#endif
    }
    /* 会话收尾(退出/断链):关窗,流由 tai_disconnect 就地关闭;supervisor 重连后
     * 新会话入口自动重新开流。 */
    OS_ENTER_CRITICAL();
    g_uplinking = 0;
    OS_EXIT_CRITICAL();
    }
#else
    while (!g_exit) {
        /* ① 等待:未在播放 TTS 且本地 VAD 检测到开口。
         *   空闲时【持续排空】录音 cbuf(_device_get_voice_data 内部 mdelay(60) 按帧节拍),
         *   保证 VAD 触发时缓冲里没有积压旧音频——上行直接发"此刻"的实时语音。
         *   ⚠️ 绝不能在 VAD 触发时再 clear():那会把刚触发到的那句语音一并清掉,
         *      之前正是这样导致云端 ASR 收到静音、回空文本。*/
        int mcp_text_poll = 0;
        g_wake_break = 0;   /* 唤醒打断标记只在本轮迭代内生效(跳过 ④ 收尾/pending 音乐) */
#if TUYA_TRANSPORT_STM_ENABLE && TUYA_STM_MCP_VIA_TEXT
        /* MCP response 经 TEXT 发出后，必须先等伪回复并 chat_break，再允许 AUDIO。
         * 否则用户恰好开口会让两个事件重叠，复现“只回空内容、语音无 ASR”。 */
        if (s_mcp_text_reply_pending) {
            if ((s_mcp_text_break_request ||
                 (int)(timer_get_ms() - s_mcp_text_reply_deadline) >= 0) &&
                !s_mcp_text_break_sent) {
                int break_rc = tuya_break_send(ctx);
                _device_rbuf_clear();
                g_tts_playing = 0;
                g_turn_done = 1;
                s_mcp_text_break_request = 0;
                s_mcp_text_break_sent = 1;
                s_mcp_text_reply_deadline = timer_get_ms() + 1200;
                printf("[TUYA-MCP] TEXT pollution break rc=%d\r\n", break_rc);
            }
            if (s_mcp_text_break_sent &&
                (int)(timer_get_ms() - s_mcp_text_reply_deadline) >= 0) {
                s_mcp_text_reply_pending = 0;
                s_mcp_text_break_sent = 0;
                printf("[TUYA-MCP] TEXT pollution drained, voice enabled\r\n");
            } else {
                mcp_text_poll = 1;
            }
        }
#endif
#ifdef TUYA_BARGE_IN_ENABLE
        /* barge-in:TTS 期间也听(不再被 g_tts_playing 门控),靠 AEC 去回声保证 VAD 不被喇叭误触发。
         * AEC 不行时这里会让回声触发 VAD→TTS 动辄自断,误触发多就关 TUYA_BARGE_IN_ENABLE 先调 AEC。*/
        /* ★ 孤儿 TTS 打断(2026-09-17 晚 20:13 天气轮实测空洞):TTS 音频断续下发时,
         *   首包播完缓冲排空会把主循环晾回 idle,后续音频仍在边到边播。idle 分支原本
         *   既不起轮(下面 play_level 门,防回声幽灵轮)也没有 barge-in 轮询——"喇叭
         *   在播+循环在 idle"期间用户说话被 idle drain 直接倒掉(实测平均 137万 的
         *   真人声整段丢弃,天气播报 9s 完全打不断)。补:喇叭仍在播且 VAD 开时跑与
         *   ④/排空期同款的 3/3 能量确认;确认即 chat_break+清 rbuf 停播+转 barge-in
         *   轮(下面 start_turn 对刚停播的轮放行,g_barge_in 使其跳过起轮能量门)。*/
        int orphan_tts_stopped = 0;
        if (_device_get_play_level() >= 640 && get_recoder_state() && barge_cooldown_expired()) {
            if (!barge_in_energy_confirmed()) {
                printf("[TUYA] barge-in (orphan TTS): VAD fired but energy low, ignored\r\n");
            } else {
                printf("[TUYA] barge-in (orphan TTS): VAD while idle-playing → chat_break + stop\r\n");
                tuya_break_send(ctx);          /* 老轮流可能未 END(还在滴),照 ④ 通知云端中止 */
                _device_rbuf_clear();         /* 清播放 cbuf,立刻停喇叭 */
                g_tts_playing = 0;
                g_barge_in = 1;
                barge_in_prefill_arm();       /* 确认帧已在历史,打断话音从历史补发 */
                g_barge_cooldown_until = timer_get_ms() + TUYA_BARGE_COOLDOWN_MS;
                orphan_tts_stopped = 1;
            }
        }
        /* ★ 播放缓冲非空(孤儿 TTS:旧轮 END 把等待骗退/兜底恢复后音频晚到)时不起轮:
         * 喇叭还在播,AEC 劣化态回声就能把 VAD 顶开→幽灵上行轮把故事回声发给云端
         * (2026-09-06 实测 [TTS!] 标记)。唤醒词不受影响(idle 排空照喂引擎),缓冲
         * 排空即恢复起轮;带 g_barge_in 的抢答轮必已清 rbuf,不会被此门误拦。*/
#ifdef TUYA_CLOUD_OPEN_ENABLE
        /* 纯云端开口:本地 VAD 不再是开口必要条件,除"正在播"(AEC 劣化态防
         * 回声自起轮)外全放行,门只剩下方 TUYA_OPEN_ENERGY_MIN 绝对静音闸;
         * 是否有效语音、何时停说全部交云端 ASR/VAD 裁定。KWS 唤醒窗同样不再
         * 拦截(见下方 #ifndef),提示音/排空喂音等唤醒 UX 不变。*/
        int start_turn = mcp_text_poll ? 0 :
                         (orphan_tts_stopped || _device_get_play_level() < 640);
#else
        int start_turn = mcp_text_poll ? 0 :
                         (get_recoder_state() && (orphan_tts_stopped || _device_get_play_level() < 640));
#endif
#ifdef TUYA_CLOUD_OPEN_ENABLE
        /* 防翻滚/AEC 自激冷却窗:上一轮收尾后的短暂窗口内不开新轮(barge-in
         * 能量确认过的抢答轮豁免);打点处在循环底。*/
        if (start_turn && !g_barge_in &&
            (int)(timer_get_ms() - g_open_cooldown_until) < 0) {
            start_turn = 0;
        }
#endif
        /* 普通轮 turn-start 能量门:无唤醒词+单麦开麦,任何持续声响都触发 VAD→设备自言自语。
         * VAD 触发后再核 1 帧能量(近场话音够响),达标才起轮;否则当噪音丢弃。
         * ★ 话音头回溯(2026-09-20):VAD 判定窗(~100ms)+轮询间隔内开口的话音帧
         *   已被 idle 排空消费,旧实现只回补本帧 1 帧 onset→上行音频首字缺声母。
         *   现在 idle 排空帧进 barge 滚动历史(见下方 idle drain),门帧也先入历史,
         *   过门后走 barge_in_prefill_arm() 按 8 万能量门回溯,把判定窗内话音头整段
         *   捞回 prefill 补发——与 barge-in 同一机制。门帧先入历史再判能量,不过门
         *   也留在历史里(后续被能量闸砍掉),保历史连续。
         * barge-in 轮(g_barge_in)已在 ④/drain 做过能量确认,这里跳过直接起轮。*/
        if (start_turn && !g_barge_in) {
            unsigned char _p[TUYA_OPUS_FRAME_LEN];
            unsigned int _s, _a;
            if (_device_get_voice_data(_p, sizeof(_p)) == TUYA_OPUS_FRAME_LEN) {
                barge_hist_push(_p);   /* 门帧入历史:过门即成回溯窗的最新帧 */
                opus_frame_stat(_p, TUYA_OPUS_FRAME_LEN, &_s, &_a);
#ifdef TUYA_CLOUD_OPEN_ENABLE
#ifdef TUYA_KWS_ENABLE
                tuya_kws_feed(_p, TUYA_OPUS_FRAME_LEN);   /* 哑门下空闲帧逐帧过此门:过门帧
                                        进 prebuf 后不再经任何喂音点(与组合模式同因),不过门
                                        帧也不再走 idle 排空——不在此无条件喂会隔帧漏喂,
                                        撕裂 KWS 唤醒滑窗(唤醒命中率腰斩) */
#endif
                if (_s >= g_open_gate) {   /* 安静=4万哑门不变,噪声=1.8×底噪抬门(见 TUYA_OPEN_GATE_K) */
                    turn_open_e = _s;
#else
                if (_s >= BARGE_MIN_ENERGY) {
#endif
                    barge_in_prefill_arm();   /* 历史能量回溯→prefill(含门帧,替代旧的单帧 onset) */
#ifdef TUYA_KWS_ENABLE
#ifndef TUYA_CLOUD_OPEN_ENABLE
                    tuya_kws_feed(_p, TUYA_OPUS_FRAME_LEN);   /* onset 帧在进 prebuf 后
                                            不再经过任何喂音点,这里补上保 KWS 流连续 */
#endif
#endif
                } else {
                    start_turn = 0;   /* 噪音/远场,拒起轮(治自言自语) */
#ifdef TUYA_CLOUD_OPEN_ENABLE
                    if (g_open_gate > TUYA_OPEN_ENERGY_MIN) {   /* 噪声抬门期才打,限流 2s 一条 */
                        static unsigned int _rej_last;
                        if ((int)(timer_get_ms() - _rej_last) >= 2000) {
                            _rej_last = timer_get_ms();
                            printf("[TUYA] open blocked by noise gate: e=%u gate=%u\r\n",
                                   _s, g_open_gate);
                        }
                    }
#endif
                }
            } else {
                start_turn = 0;
            }
        }
#ifdef TUYA_KWS_ENABLE
        /* 唤醒门: 引擎不可用(无 token 回退)时 tuya_kws_awake() 恒真,行为与
         * 现在完全一致;可用时只在唤醒窗内放行起轮,窗外吞掉——帧照常在下面
         * 的 idle 分支排空并喂 KWS,唤醒词窗口由引擎命中/起轮/答毕三个点续期。*/
        if (g_wake_swallow) {   /* 词尾吞咽窗(TuyaOpen input_reset 语义),见 on_wake */
            int vad_now = get_recoder_state();
            if (!vad_now || (int)(timer_get_ms() - g_wake_swallow_deadline) >= 0) {
                g_wake_swallow = 0;
                printf("[TUYA] wake tail drained (%s)\r\n",
                       vad_now ? "900ms cap" : "vad closed");
            }
        }
        /* barge-in 轮豁免唤醒窗与吞咽窗:用户已经能量确认在说话,窗过期(长 TTS/
         * 音乐播超 15s 后窗早已失效)不能把打断后的命令拦回 idle 排空——
         * 2026-09-05 实测:故事 TTS 中打断,"给我放一首周杰伦的歌"整句被吞。*/
#ifndef TUYA_CLOUD_OPEN_ENABLE
        if (start_turn && !g_barge_in &&
            (g_wake_swallow || !tuya_kws_awake())) start_turn = 0;
        if (start_turn) tuya_kws_window_kick();
#endif
        g_wake_hit = 0;   /* 走到唤醒门=空闲路径,on_wake 已就地处理(提示音已播/吞咽窗已开) */
        if (g_wake_suppress_barge) {
            if ((int)(timer_get_ms() - g_wake_suppress_barge_until) >= 0) {
                g_wake_suppress_barge = 0;   /* 时限已过:之后的抢答轮是全新事件 */
            } else if (start_turn && g_barge_prefill > 1) {
                /* 只拦"带历史 prefill 的抢答轮"(prefill>1):里面装的是刚被识别的
                 * 唤醒词。用户下一句真话音只带 1 帧 onset,不受影响,照常起轮。*/
                printf("[TUYA] wake cancels barge turn (prefill %u discarded)\r\n",
                       g_barge_prefill);
                g_wake_suppress_barge = 0;
                g_barge_prefill = 0;
                g_barge_in = 0;
                start_turn = 0;   /* 回空闲:提示音已播,吞咽窗吃词尾,15s 窗已开 */
            }
        }
#ifdef TUYA_MUSIC_ENABLE
        if (g_barge_prompt_wait) {   /* 抢答停乐/停TTS 后的哑火补救,见 music_handoff/
                                        * tts_barge_poll 注释:无后续=多半是唤醒词,补播+开窗 */
            if (start_turn) {
                g_barge_prompt_wait = 0;   /* 用户接着下了命令:正常起轮,不补提示音 */
            } else if ((int)(timer_get_ms() - g_barge_prompt_deadline) >= 0) {
                g_barge_prompt_wait = 0;
                if (!get_recoder_state()) {   /* VAD 已关:没有后续话音=刚才那声是唤醒词 */
                    printf("[TUYA] music barge w/o follow-up → wake prompt\r\n");
                    printf("[TUYA] wake alert: WakeHeyTuya.mp3\r\n");
                    app_music_tuya_play_wake_prompt();
                    g_last_wake_alert = timer_get_ms();   /* 盖章:on_wake 迟到补中不再重播 */
                    tuya_kws_window_kick();   /* 视作唤醒:打开 15s 追问窗 */
                }
            }
        }
#endif
#endif
        if (!start_turn) {
#else
        if (g_tts_playing || !get_recoder_state()) {
#endif
#ifdef TUYA_BARGE_IN_ENABLE
            g_barge_prefill = 0;   /* idle 期间任何 pending barge-in prefill 都已过期,清掉 */
#endif
#ifdef TUYA_MUSIC_ENABLE
            /* 兜底:音乐已挂起但主流程没接住(如旧轮残留 END 把 ④ 等待提前骗退、
             * SKILL 回包又晚到——2026-09-05"第一次放歌没反应")→ idle 里直接
             * 接管播放。g_tts_playing=1(TTS 播报中)不抢 DAC,等它排空后的
             * 正常路径/idle 下一拍再接。*/
            if (tuya_music_pending() && !g_link_broken && !g_tts_playing) {
                music_handoff(ctx);
                continue;   /* 播完/被打断后回循环顶,保持 idle 节奏 */
            }
#endif
            unsigned char _drain[TUYA_OPUS_FRAME_LEN];
            int tn = _device_get_voice_data(_drain, sizeof(_drain));   /* 排空 mic 缓冲,不排会积压旧音频 */
            /* MQTT 心跳由独立线程维持(见 tuya_ai_run 入口的 tuya_mqtt_keepalive_task),
             * 不在这里调 iot_client_process——它内部的 TLS recv 会阻塞语音循环线程。*/
            if (tn == TUYA_OPUS_FRAME_LEN) {   /* 攒够整帧才统计,得到稳定的静音底噪基线 */
                unsigned int s, a;
                opus_frame_stat(_drain, TUYA_OPUS_FRAME_LEN, &s, &a);
                /* ★ 帧进 barge 滚动历史(2026-09-20):VAD 判定窗内被排空的话音头帧由此
                 *   留底,起轮能量门过门后回溯补发(见 turn-start 能量门),治空闲起轮
                 *   首字缺声母。成本与 TTS/音乐期排空相同(memmove 57KB@25Hz≈1.4MB/s),
                 *   空闲 CPU ~77% 余量充足;静音帧后续被 8 万能量闸砍掉,不进上行。*/
                barge_hist_push(_drain);
                idle_cnt++; idle_sum += s; idle_act_sum += a;
                if (idle_cnt >= 16) {   /* 16帧≈1s,汇总打印一次 */
                    printf("[TUYA] idle drain: %u frames, avg_sum=%u avg_act=%u%%\r\n",
                           idle_cnt, idle_sum / idle_cnt, idle_act_sum / idle_cnt);
                    idle_cnt = 0; idle_sum = 0; idle_act_sum = 0;
                }
                /* ★ 自适应开门底噪采样(2026-09-22):TTS 播放期不入样(g_tts_playing
                 *   时回声残留会抬底噪,安静期 3.7k~8.9k 尚可、噪声期无谓);16 帧
                 *   均值成批入环,最近 8 批取中位数=稳态底噪,门限随之收敛。中位数
                 *   抗话音突发污染(idle 排空会吃到话音头帧),环境噪声变化 ~3s 适应。*/
                if (!g_tts_playing) {
                    idlef_cnt++; idlef_sum += s;
                    if (idlef_cnt >= 16) {
                        unsigned int _i, _j, _k, _g;
                        unsigned int _sorted[TUYA_OPEN_GATE_NBAT];
                        idlef_ring[idlef_idx] = idlef_sum / idlef_cnt;
                        idlef_idx = (idlef_idx + 1) % TUYA_OPEN_GATE_NBAT;
                        if (idlef_n < TUYA_OPEN_GATE_NBAT) idlef_n++;
                        for (_i = 0; _i < idlef_n; _i++) _sorted[_i] = idlef_ring[_i];
                        for (_i = 1; _i < idlef_n; _i++) {   /* 插入排序,n≤8 开销可忽略 */
                            _k = _sorted[_i];
                            for (_j = _i; _j > 0 && _sorted[_j - 1] > _k; _j--)
                                _sorted[_j] = _sorted[_j - 1];
                            _sorted[_j] = _k;
                        }
                        g_idle_floor = _sorted[idlef_n / 2];   /* 中位数(偶数取上中位) */
                        _g = g_idle_floor / 1000u * TUYA_OPEN_GATE_K;
                        if (_g < TUYA_OPEN_ENERGY_MIN) _g = TUYA_OPEN_ENERGY_MIN;
                        if (_g > TUYA_OPEN_GATE_CAP) _g = TUYA_OPEN_GATE_CAP;
                        if (_g != g_open_gate) {
                            unsigned int _ref = g_open_gate / 10u + 1u;
                            if (_g > g_open_gate + _ref || _g + _ref < g_open_gate)
                                printf("[TUYA] open-gate: floor=%u gate=%u (was %u)\r\n",
                                       g_idle_floor, _g, g_open_gate);   /* ±10%以上才打,防抖动刷屏 */
                            g_open_gate = _g;
                        }
                        idlef_cnt = 0; idlef_sum = 0;
                    }
                }
#ifdef TUYA_KWS_ENABLE
                tuya_kws_feed(_drain, tn);   /* 排空的 40ms 帧喂唤醒词引擎 */
#endif
            }
            tai_log_flush();   /* idle 排空节拍:顺带刷出 stm 库延迟日志(唯一 printf 出口在本任务) */
            mcp_resp_pump(ctx);   /* MCP initialize 多在空闲期到:这里就是回应的实际发送点 */
            continue;
        }
#ifdef TUYA_BARGE_IN_ENABLE
        /* barge-in 起新上行前不再排 mic cbuf。录音写侧在满时丢最旧帧、保留最新
         * 话音；这里若再按水位排空会把抢答正文当 stale 吞掉。确认帧由 prebuf 先发，
         * cbuf 中的后续帧紧接着由实时上行消费。g_barge_in 在这里清除。*/
        if (g_barge_in) {
            unsigned int olvl = _device_get_voice_level();
            /* cbuf 已由录音写侧维护成“最新窗口”，这里不再人为丢帧。旧实现按水位
             * 再排最多 6 帧，即使把帧放进 prebuf 也会额外等待约 240ms，并把抢答
             * 话音延迟到 audio_start 之后才发送。直接保留现有帧给实时上行即可。*/
            if (olvl > TUYA_OPUS_FRAME_LEN * 3) {
                printf("[TUYA] barge-in: keep mic backlog %u B for uplink\r\n", olvl);
            }
            g_barge_in = 0;
        }
#endif
#ifdef TUYA_MUSIC_ENABLE
        if (g_player_restore_pending) {   /* music_handoff 推迟的播放器恢复在此消费:
                                           * 起轮时恢复,回话 TTS 前必定就位(~1.5s 后
                                           * 才到);提示音播 mp3 期间不开 opus,不抢 DAC */
            g_player_restore_pending = 0;
            printf("[TUYA-MUSIC] restore tts player at turn start\r\n");
            _device_net_audio_play(1);
        }
#endif
        g_turn_done = 0;
        /* 快照"起轮前正在应答的轮 id":本轮 ④ 等待期若收到与之匹配的 END,
         * = 被打断旧轮的残留收尾,on_event 里判弃(见 g_stale_end_bizid 注释)。
         * 回调线程会读它,拷贝进临界区防撕裂。*/
        OS_ENTER_CRITICAL();
        strncpy(g_stale_end_bizid, g_answer_bizid, sizeof(g_stale_end_bizid) - 1);
        g_stale_end_bizid[sizeof(g_stale_end_bizid) - 1] = '\0';
        OS_EXIT_CRITICAL();
        uplink_frames = 0; uplink_active = 0;
#ifdef TUYA_CLOUD_OPEN_ENABLE
        turn_start_ms = timer_get_ms();   /* dur telemetry 计时起点(本地无硬顶,轮长全由云端定) */
        turn_stop = "-";                  /* 每轮重置停说原因 */
#endif
#ifdef TUYA_SERVER_VAD_ENABLE
        g_server_vad_stop = 0;   /* 清掉上轮残留的云端VAD标志 */
        silence_frames = 0;       /* 清掉上轮残留的静音兜底计数 */
#endif
        printf("[TUYA] speak-start: uplink begin (cbuf_level=%u B)\r\n", _device_get_voice_level());
        if (tai_send_audio_start(ctx,
#ifdef TUYA_UPLINK_OPUS_ENABLE
                                 use_opus_uplink ? TAI_AUDIO_OPUS : TAI_AUDIO_PCM,   /* opus=111:云端 ASR 走 opus 解码 */
#else
                                 TAI_AUDIO_PCM,
#endif
                                 1, 16, 16000) != TAI_OK) {
#ifdef TUYA_BARGE_IN_ENABLE
            g_barge_prefill = 0;   /* 本轮没起上行,别让 prefill 漏到下一轮 */
#endif
            printf("[TUYA] tai_send_audio_start fail\r\n");
            if (++start_fails >= 5) {   /* 连续失败:TCP 半死(未触发 on_disconnect),别原地空转 */
                printf("[TUYA] audio_start failed x%u, link dead\r\n", start_fails);
                g_link_broken = 1;
                break;
            }
            msleep(200);
            continue;
        }
        start_fails = 0;
        /* 本轮 event-id 刚由 audio_start 生成(tai_current_event_id 读回复制),
         * on_event 上行窗内凭它区分"本轮噪声收尾 END"与"旧轮残留 END"。
         * 先拷贝后开窗(临界区内),回调线程不会读到半写状态。
         * STM 传输无 current_event_id 对应物(tai_* 被重定向到 tstm_*):
         * 该构建下上行窗判属不启用,END 归因退回原快照逻辑。*/
#if defined(TUYA_TRANSPORT_STM_ENABLE) && TUYA_TRANSPORT_STM_ENABLE
        g_turn_event_id[0] = '\0';
#else
        OS_ENTER_CRITICAL();
        {
            const char *_eid = tai_current_event_id(ctx);
            strncpy(g_turn_event_id, _eid ? _eid : "", sizeof(g_turn_event_id) - 1);
            g_turn_event_id[sizeof(g_turn_event_id) - 1] = '\0';
            g_uplinking = 1;
        }
        OS_EXIT_CRITICAL();
#endif
        g_tts_drop_until = 0;   /* 新轮起:关闭作废轮残包排空窗,本轮回话照常播 */
#ifdef TUYA_BARGE_IN_ENABLE
        /* barge-in 轮:先补发能量确认时读走的 onset 帧(最响那段),再进实时上行。
         * 否则 onset 丢失、上行只收尾音/静音→ASR 空→打断后无播报。*/
        if (g_barge_prefill) {
            unsigned int k;
            for (k = 0; k < g_barge_prefill; k++) {
                /* prefill 帧不再补喂 KWS:确认帧已在 barge_in_prefill_arm 喂过,
                 * 历史/残响帧在各自消费点喂过——重复喂会撕乱引擎的滑窗。*/
#ifdef TUYA_UPLINK_OPUS_ENABLE
                if (use_opus_uplink) {
                    tuya_uplink_send_frame(ctx, &g_barge_prebuf[k * 1280]);   /* onset 帧同样走编码 */
                } else
#endif
                {
                    tai_send_audio_chunk(ctx, &g_barge_prebuf[k * 1280], 1280);
                }
            }
            /* 2026-09-20 起两条路径同源:空闲起轮与 barge-in 的补发帧都来自历史回溯
             * (上方的 barge history -> prefill 打印),此处只报总数,不再按帧数猜路径。*/
            printf("[TUYA] turn-start: prepended %u frame(s)\r\n", g_barge_prefill);
            g_barge_prefill = 0;
        }
#endif

        /* 回复开始后不要立即截断上行：本轮若尚无实时帧，g_tts_playing 很可能仍是
         * 上一轮/启动 MCP 污染回复的尾部状态。至少发出 3 帧实时音频后才允许按新 TTS
         * 收尾，避免出现“audio start + prepended 1 frame，但实时上行 0 帧”的空轮。*/
        while (!g_exit) {
            if (g_tts_playing && uplink_frames >= 3 && barge_cooldown_expired()) {
#ifdef TUYA_CLOUD_OPEN_ENABLE
                turn_stop = "tts-start";   /* 云端已开始回话=隐式判停 */
#endif
                break;
            }
            tai_log_flush();   /* 每帧节拍刷库日志:首包发送时段正是引擎线程日志高发窗口 */
            /* 不在 AUDIO 事件期间发送 MCP/TEXT。STM 的 TEXT 与 AUDIO 共用会话事件通道，
             * initialize 若在上行中抵达，此处发 TEXT 会与音频事件重叠并卡住发送线程。
             * response 留在 s_mcp_resp，audio_end 后的等待循环再安全发送。 */
            int n = _device_get_voice_data(abuf, TUYA_OPUS_FRAME_LEN);
            if (n == TUYA_OPUS_FRAME_LEN) {
                unsigned int fsum, fact;
                opus_frame_stat(abuf, TUYA_OPUS_FRAME_LEN, &fsum, &fact);
#ifdef TUYA_KWS_ENABLE
                /* KWS 全程在线(含上行:TuyaOpen 驱动层在 UPLOAD/THINK 状态同样喂)。
                 * 不喂的后果(2026-09-05 实测):追问窗内(答毕续 15s)用户喊"嘿tuya",
                 * VAD 直接起轮把整句上行,云端 ASR 收到裸唤醒词→回"你是想说涂鸦吗"。
                 * 喂引擎后词识别完整即命中→chat_break 作废本轮(词前半已上行,
                 * 压掉云端回包)+吞咽窗吃词尾+提示音已由 on_wake 播,回空闲。
                 * 副作用(TuyaOpen 同款):唤醒词+问题连说时本轮作废,问题接缝处
                 * 丢零点几秒,由 900ms 吞咽兜底后的重听补回。*/
                tuya_kws_feed(abuf, TUYA_OPUS_FRAME_LEN);
                if (g_wake_hit) {
                    g_wake_hit = 0;
                    printf("[TUYA] wake breaks uplink → chat_break, discard turn (frames=%u)\r\n",
                           uplink_frames);
                    tuya_break_send(ctx);   /* 词前半已上行:作废本轮,云端不再回话 */
                    g_wake_break = 1;      /* 跳过④收尾 mic 排空/pending 音乐丢弃 */
                    g_tts_drop_until = timer_get_ms() + 3000;   /* 本轮/旧轮回话残包排空,见 on_audio */
                    g_tts_drop_cnt = 0;
                    break;
                }
#endif

#ifdef TUYA_UPLINK_LATENCY_DEBUG
                /* 测 发送(+opus编码) 耗时 + 前后 cbuf 水位 + TTS 上下文。
                 * 正常: 编码 ~1ms + send 5-15ms,cbuf 水位接近 0(每帧 mdelay40 消费节拍)。
                 * 异常: send 几十~几百ms,cbuf 水位涨(锁竞争/网络抖动/被抢占)。
                 * 只在超阈值时打印,避免刷屏;阈值见文件顶 UPLINK_LAT_WARN_MS。*/
                unsigned int _lvl_pre = _device_get_voice_level();
                unsigned int _t0 = timer_get_ms();
                int _tts_flag = g_tts_playing;   /* 快照:send 时 TTS 是否在播(锁竞争假说关键信号)*/
#ifdef TUYA_UPLINK_OPUS_ENABLE
                int _snd_rt = use_opus_uplink ? tuya_uplink_send_frame(ctx, abuf)
                                              : tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN);
#else
                int _snd_rt = tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN);
#endif
                unsigned int _dt = timer_get_ms() - _t0;
                unsigned int _lvl_post = _device_get_voice_level();
                if (_snd_rt != TAI_OK) {
                    printf("[TUYA] tai_send_audio_chunk fail\r\n");
                    g_link_broken = 1;   /* 发送失败=链路断,快速报废本会话(supervisor 重连) */
                    break;
                }
                /* 超时 或 水位异常高 才打印 */
                if (_dt >= UPLINK_LAT_WARN_MS || _lvl_pre >= UPLINK_LAT_CBUF_WARN) {
                    printf("[LAT] send=%ums lvl=%u→%u (+%u) %s\r\n",
                           _dt, _lvl_pre, _lvl_post, _lvl_post - _lvl_pre,
                           _tts_flag ? "[TTS!]" : "");
                }
#else
#ifdef TUYA_UPLINK_OPUS_ENABLE
                if (use_opus_uplink) {
                    if (tuya_uplink_send_frame(ctx, abuf) != TAI_OK) {
                        printf("[TUYA] tai_send_audio_chunk fail\r\n");
                        g_link_broken = 1;   /* 发送失败=链路断,快速报废本会话(supervisor 重连) */
                        break;
                    }
                } else if (tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN) != TAI_OK) {
                    printf("[TUYA] tai_send_audio_chunk fail\r\n");
                    g_link_broken = 1;
                    break;
                }
#else
                if (tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN) != TAI_OK) {
                    printf("[TUYA] tai_send_audio_chunk fail\r\n");
                    g_link_broken = 1;   /* 发送失败=链路断,快速报废本会话(supervisor 重连) */
                    break;
                }
#endif
#endif

                uplink_frames++;
                if (fact > 3) uplink_active++;   /* 新度量下 fact=avg|sample|/100;>3 即 avg>300(远超 idle 底噪 avg<50)≈有效话音帧 */
                if (uplink_frames % 25 == 1)     /* 40ms/帧,25帧≈1s 一条:压测长跑防串口刷屏拖慢上行节拍 */
                    printf("[TUYA] uplink f#%u sum=%u act=%u%%\r\n", uplink_frames, fsum, fact);
            }
#ifdef TUYA_SERVER_VAD_ENABLE
            /* 云端VAD模式:停说完全由云端裁决(TAI_EVT_SERVER_VAD / cloud END),
             * 纯云试验分支无任何本地兜底;组合分支(#else)保留本地 1 帧即时兜底。
             * 开口仍由哑门+冷却窗负责(循环顶部),这里只管"停说"。*/
            if (g_server_vad_stop) {
                printf("[TUYA] speak-stop: server-vad (frames=%u active=%u)\r\n",
                       uplink_frames, uplink_active);
#ifdef TUYA_CLOUD_OPEN_ENABLE
                turn_stop = "server-vad";
#endif
                break;
            }
            /* STM 下行无法可靠区分 SERVER_VAD；本地 VAD 已经过去抖，判停后立即
             * 结束事件。仍优先接受可识别的 chat_break/server-vad 标志。 */
#ifdef TUYA_CLOUD_OPEN_ENABLE
            /* 纯云端停说:本地零兜底,轮何时结束完全由云端裁决——
             * server-vad(话音结束事件,上方)/cloud END(纯噪声轮就地收尾,实测 ~3-4s)。
             * 3s 静音/15s 硬顶已移除:两者要么零触发,要么把"云端到底会不会裁、
             * 多久才裁"的观测截尾,A/B 期间持续上流正是要测的东西(2KB/s,链路实测无代价)。*/
            if (g_turn_done) {
                printf("[TUYA] speak-stop: cloud-end (frames=%u active=%u)\r\n",
                       uplink_frames, uplink_active);
                turn_stop = "cloud-end";
                break;
            }
#else
            if (!get_recoder_state()) {
                if (++silence_frames >= LOCAL_SILENCE_TIMEOUT_FRAMES) {
                    printf("[TUYA] speak-stop: local-vad (frames=%u active=%u)\r\n",
                           uplink_frames, uplink_active);
                    break;
                }
            } else {
                silence_frames = 0;
            }
#endif
#else
            /* 本地VAD模式:本地VAD直接判停说(原逻辑) */
            if (!get_recoder_state()) {     /* enc VAD 已 debounce 判定停说(stop 阈值) */
                printf("[TUYA] speak-stop: uplink end (frames=%u active=%u)\r\n",
                       uplink_frames, uplink_active);
                break;
            }
#endif
        }
        /* 尾部冲刷(2026-09-17 晚):VAD 判停后立刻 audio_end 会把 mic cbuf 里还压着
         * 的尾音(≤0.5s)留在本地——实测"你好涂鸦"被掐成"你好。"(丢"涂鸦")、"你
         * 说什么"掐成"你对"。audio_end 前把已缓冲的帧冲完(上限 8 帧=320ms;纯冲
         * 已录音、不等待新帧、无能量判定),云端拿到完整句尾再收 payloads-end。
         * ★ 2026-09-20 opus 上行漏改点修复:此处曾直发 1280B 裸 PCM,云 libopus 报
         *   corrupted stream(gRPC INVALID_ARGUMENT)废掉整轮 ASR——音乐打断轮 cbuf
         *   有积压时是全场唯一走到此处的路径,实测 21 包话音全好、却死在这 8 帧裸包。*/
        if (!g_link_broken && !g_exit) {
            unsigned char _tb[TUYA_OPUS_FRAME_LEN];
            unsigned int _tn = 0;
            while (_tn < 8 && _device_get_voice_level() >= TUYA_OPUS_FRAME_LEN) {
                if (_device_get_voice_data(_tb, sizeof(_tb)) != TUYA_OPUS_FRAME_LEN) {
                    break;
                }
#ifdef TUYA_UPLINK_OPUS_ENABLE
                int _tr = use_opus_uplink ? tuya_uplink_send_frame(ctx, _tb)
                                          : tai_send_audio_chunk(ctx, _tb, TUYA_OPUS_FRAME_LEN);
#else
                int _tr = tai_send_audio_chunk(ctx, _tb, TUYA_OPUS_FRAME_LEN);
#endif
                if (_tr != TAI_OK) {
                    printf("[TUYA] tail flush send fail\r\n");
                    g_link_broken = 1;
                    break;
                }
                _tn++;
            }
            if (_tn) {
                uplink_frames += _tn;
                printf("[TUYA] tail flush: %u frames\r\n", _tn);
            }
        }
        {
            int end_rc = tai_send_audio_end(ctx);
            printf("[TUYA] audio-end rc=%d frames=%u active=%u\r\n",
                   end_rc, uplink_frames, uplink_active);
#ifdef TUYA_CLOUD_OPEN_ENABLE
            /* 每轮一行 A/B telemetry:过门能量/停说原因/帧数/有效帧占比/轮时长 */
            printf("[TUYA] turn: open_e=%u stop=%s frames=%u act=%u%% dur=%ums\r\n",
                   turn_open_e, turn_stop, uplink_frames,
                   uplink_frames ? (uplink_active * 100u) / uplink_frames : 0u,
                   timer_get_ms() - turn_start_ms);
            turn_open_e = 0;   /* reset after print: gate re-stamps next turn, barge turns read 0 */
#endif
            if (end_rc != TAI_OK) {
                g_link_broken = 1;
            }
        }
        g_uplinking = 0;   /* 上行窗关闭(audio_end 已发/链路已断):此后 END 走④快照比对/正常收尾 */

        /* ④ 等本轮回复结束(云端 TTS 播完,TAI_EVT_END 置 g_turn_done)再回 ①。
         * barge-in 抢答轮进入这里时用户通常还没说完；不能因旧轮 chat_break 已把
         * g_turn_done 置位而直接跳过等待，否则后续话音无人消费。*/
#ifdef TUYA_BARGE_IN_ENABLE
        if (g_barge_in) {
            g_turn_done = 0;
        }
#endif
        int w = 0;
        s_barge_hist_cnt = 0;   /* 新一轮播放等待:历史从本轮起算,防上一轮尾巴混入打断补发 */
        while (!g_exit && !g_link_broken && !g_turn_done && w < TUYA_WAIT_MS) {
#ifdef TUYA_CLOUD_OPEN_ENABLE
            if (g_tts_playing) turn_had_tts = 1;   /* 收尾冷却时长依据:TTS 轮 1s,空轮 2s */
#endif
            /* 云端沉默兜底:TTS 一直没来(g_tts_playing 未置位且下行 cbuf 无数据)
             * 就只等 10s 回听音——STM 首轮实测云端无响应,原 60s 干等让设备像死机,
             * 后续说话全被忽略(2026-08-31)。TTS 只要来过(g_tts_playing 置位过,
             * 播放期间会持续为 1),60s 上限内继续等播完。*/
            if (!g_tts_playing && _device_get_play_level() == 0 && w >= 10000) {
                printf("[TUYA] no TTS in 10s (cloud silent?), back to listening\r\n");
                break;
            }
#ifdef TUYA_KWS_ENABLE
            /* KWS 全程在线:等回复期间也喂唤醒词(有整帧才读,不阻塞——保住
             * 20ms 轮询节拍和 w+=20 计时)。命中即打断本轮,优先于 barge-in:
             * 唤醒词是明确意图,普通 barge-in 还要 3 帧能量确认。*/
            if (_device_get_voice_level() >= TUYA_OPUS_FRAME_LEN) {
                unsigned char _wk[TUYA_OPUS_FRAME_LEN];
                if (_device_get_voice_data(_wk, sizeof(_wk)) == TUYA_OPUS_FRAME_LEN) {
                    tuya_kws_feed(_wk, sizeof(_wk));
                    barge_hist_push(_wk);   /* 播放期帧进 barge 历史:打断时补发命令头部 */
                }
            }
            if (g_wake_hit) {
                g_wake_hit = 0;
                wake_break_tts(ctx);
                break;
            }
#endif
#ifdef TUYA_BARGE_IN_ENABLE
            /* barge-in:TTS 播放期间本地 VAD 检测到说话 → 打断。靠 AEC 保证 VAD 不是被回声触发。*/
            if (g_tts_playing && get_recoder_state() && barge_cooldown_expired()) {
#if defined(TUYA_KWS_ENABLE) && defined(TUYA_MUSIC_ENABLE)
                if (tts_barge_poll(ctx, "")) {   /* 停TTS→排空喂引擎→判后续(见其注释):
                                                  * 有后续=抢答轮已布防;无后续/引擎补中=
                                                  * 已按唤醒收尾(提示音/哑火补救接手) */
                    break;
                }
#else
                if (!barge_in_energy_confirmed()) {
                    printf("[TUYA] barge-in: VAD fired but energy low (AEC残留/噪音?), ignored\r\n");
                } else {
                    printf("[TUYA] barge-in: VAD during TTS → chat_break + stop TTS\r\n");
                    tuya_break_send(ctx);          /* 通知云端中止本轮 TTS */
                    _device_rbuf_clear();         /* 清播放 cbuf,立刻停 TTS 喇叭 */
                    g_tts_playing = 0;
                    g_barge_in = 1;
                    barge_in_prefill_arm();       /* 打断前的话音(循环里已消费的)从历史补发,
                                                     否则云端只听到确认点之后的尾部 */
                    g_barge_cooldown_until = timer_get_ms() + TUYA_BARGE_COOLDOWN_MS; /* 冷却:压住老轮在途 TTS 二次触发 */
                    break;
                }
#endif
            }
#endif
            tai_log_flush();   /* 等待期刷库日志:云端回话/出错(WARN+)集中在本阶段 */
            mcp_resp_pump(ctx);
            msleep(20); w += 20;   /* ④ 轮询 50→20ms,压低 barge-in 检测延迟 */
        }
        /* 回复期间(TTS 播放/等云端)录音 cbuf 积压了环境音/TTS 回采,这里清掉——
           此刻没有要保留的用户语音,清它是安全的(与"VAD触发时清"不同,那才会吞掉话音)。
           barge-in 时用户正在说话,【不能】清缓冲也不能冷却,跳过直接进新一轮上行。*/
        if (!g_barge_in && !g_link_broken && !g_wake_break) {   /* 链路断:不等 TTS 到达/排空;唤醒打断:本轮已作废 */
            /* 等 DAC 真正播完再听:云端 EVT_END(本轮发完)时,下行 jitter buffer 里可能还压着
             * 几秒 TTS,喇叭仍在播。若这时去听,麦克风采到正在播的 TTS→当新问题→自说自话。
             * 改成等下行 cbuf(pcm_cbuff_r)排空(≈DAC 播完)再听,从根上消除"云端发完≠喇叭播完"。
             * 然后排空 mic cbuf ~300ms,清掉播放期间积压的回声/混响尾巴。*/
            /* ★ 先等 TTS 真正开始下发(cbuf 有数据),再等它排空。
             *   修复 bug:云端有时"先发 event:end 再发音频",EVT_END 时 cbuf 是空的,
             *   原来的 while(play_level>640) 会立刻判"播完了"跳过 → 回到循环顶 →
             *   喇叭随后播 TTS 触发 VAD → TTS 被截断("没说完")。先等 cbuf 出现数据,
             *   确认本轮 TTS 确实到达,再进入排空等待。超时 3s:云端无音频(纯文本/异常)时不卡死。
             * barge-in 在此阶段也必须检测:用户改口(如说错了换第二个问题)不等 TTS 播完,
             *   不论 TTS 是否在播,只要本地确认是话音就打断当前轮、发新一轮。*/
            unsigned int wait_start = 0;
            while (!g_exit && _device_get_play_level() == 0 && wait_start < 3000) {
                tai_log_flush();
                mcp_resp_pump(ctx);
                msleep(20); wait_start += 20;
#ifdef TUYA_KWS_ENABLE
                /* KWS 全程在线:TTS 未到达阶段同样可被唤醒词打断(用户重新唤醒)。*/
                if (_device_get_voice_level() >= TUYA_OPUS_FRAME_LEN) {
                    unsigned char _wk[TUYA_OPUS_FRAME_LEN];
                    if (_device_get_voice_data(_wk, sizeof(_wk)) == TUYA_OPUS_FRAME_LEN) {
                        tuya_kws_feed(_wk, sizeof(_wk));
                        barge_hist_push(_wk);   /* 播放期帧进 barge 历史:打断时补发命令头部 */
                    }
                }
                if (g_wake_hit) {
                    g_wake_hit = 0;
                    wake_break_tts(ctx);
                    break;
                }
#endif
#ifdef TUYA_BARGE_IN_ENABLE
                /* TTS 未到达阶段也检测 barge-in:用户改口不等 TTS,确认话音即打断当前轮。*/
                if (get_recoder_state() && barge_cooldown_expired()) {
#if defined(TUYA_KWS_ENABLE) && defined(TUYA_MUSIC_ENABLE)
                    if (tts_barge_poll(ctx, " (pre-TTS)")) {   /* 处置同 ④:停轮+判后续 */
                        break;
                    }
#else
                    if (barge_in_energy_confirmed()) {
                        printf("[TUYA] barge-in (pre-TTS): cancel before audio arrives\r\n");
                        tuya_break_send(ctx);
                        _device_rbuf_clear();
                        g_tts_playing = 0;
                        g_barge_in = 1;
                        barge_in_prefill_arm();   /* 同 ④:打断前话音从历史补发 */
                        g_barge_cooldown_until = timer_get_ms() + TUYA_BARGE_COOLDOWN_MS;
                        break;
                    }
#endif
                }
#endif
            }
            unsigned int wait_ms = 0;
            while (!g_exit && !g_wake_break && _device_get_play_level() > 640 && wait_ms < 35000) {  /* 等 32s 缓冲排空(≈喇叭真播完);35s 是兜底,正常排完就提前退 */
                tai_log_flush();
                mcp_resp_pump(ctx);
                msleep(20); wait_ms += 20;
#ifdef TUYA_KWS_ENABLE
                /* KWS 全程在线:TTS 实际播放期(喇叭在响)喂唤醒词,命中即打断。
                 * 2026-09-05 实测:此阶段 KWS 无帧可吃,连喊几声"嘿tuya"全被
                 * barge-in 当普通抢答上行了,提示音也没播——本块就是修它。*/
                if (_device_get_voice_level() >= TUYA_OPUS_FRAME_LEN) {
                    unsigned char _wk[TUYA_OPUS_FRAME_LEN];
                    if (_device_get_voice_data(_wk, sizeof(_wk)) == TUYA_OPUS_FRAME_LEN) {
                        tuya_kws_feed(_wk, sizeof(_wk));
                        barge_hist_push(_wk);   /* 播放期帧进 barge 历史:打断时补发命令头部 */
                    }
                }
                if (g_wake_hit) {
                    g_wake_hit = 0;
                    wake_break_tts(ctx);
                    break;
                }
#endif
#ifdef TUYA_BARGE_IN_ENABLE
                /* barge-in 也要在 play-drain-wait 里检测!云端 EVT_END 后 ④ 已退出,但喇叭还在放
                 * 缓冲里的 TTS。这段期间用户说话必须能打断(否则大缓冲=大窗口无法打断)。*/
                if (get_recoder_state() && barge_cooldown_expired()) {
#if defined(TUYA_KWS_ENABLE) && defined(TUYA_MUSIC_ENABLE)
                    if (tts_barge_poll(ctx, " (drain)")) {   /* 处置同 ④:停播+判后续 */
                        break;
                    }
#else
                    if (!barge_in_energy_confirmed()) {
                        printf("[TUYA] barge-in (drain): VAD fired but energy low, ignored\r\n");
                    } else {
                        printf("[TUYA] barge-in (drain): VAD during TTS drain → chat_break + stop\r\n");
                        tuya_break_send(ctx);
                        _device_rbuf_clear();
                        g_tts_playing = 0;
                        g_barge_in = 1;
                        barge_in_prefill_arm();   /* 同 ④:打断前话音从历史补发 */
                        g_barge_cooldown_until = timer_get_ms() + TUYA_BARGE_COOLDOWN_MS;
                        break;
                    }
#endif
                }
#endif
                /* AEC 诊断:播放 TTS 时读 mic(post-AEC 输出),看回声消没消掉。
                 * act 高(>50%)=回声还在→AEC 没消;低(<20%)=消了→AEC 生效。每 1s 打一次。
                 * 不发上行,只读丢弃——纯诊断,不影响正常流程。*/
                if (wait_ms > 0 && (wait_ms % 1000) == 0) {
                    unsigned char _aec_t[TUYA_OPUS_FRAME_LEN];
                    if (_device_get_voice_data(_aec_t, sizeof(_aec_t)) == TUYA_OPUS_FRAME_LEN) {
                        unsigned int es, ea;
                        opus_frame_stat(_aec_t, TUYA_OPUS_FRAME_LEN, &es, &ea);
                        printf("[AEC-DBG] TTS playing, mic post-AEC: sum=%u act=%u%% rec=%d\r\n", es, ea, get_recoder_state());
                    }
#ifdef TUYA_UPLINK_LATENCY_DEBUG
                    /* TTS 播放期间 mic cbuf 水位监控:验证"TTS 期间堆积"假说。
                     * cbuf 容量 = SAMPLE_RATE*CHANNEL*1 = 16000B(0.5 秒)。
                     * 水位涨 = 上行循环没在消费(此时本就在 play-drain,正常不消费);
                     * 水位接近 16000 = 快溢出,新话音会被环形覆盖(丢音风险)。*/
                    unsigned int _wlvl = _device_get_voice_level();
                    if (_wlvl >= UPLINK_LAT_CBUF_WARN) {
                        printf("[LAT] TTS drain: mic cbuf=%u/%uB (%u%% full)%s\r\n",
                               _wlvl, 16000u, (_wlvl * 100u) / 16000u,
                               _wlvl >= 16000u ? " [OVERFLOW!]" : "");
                    }
#endif
                }
            }
            /* 唤醒打断:词尾由吞咽窗在 idle 排;barge-in 打断:confirm 后 cbuf 里压的正是
             * 抢答正文(此路径 break 只跳出排空循环,拦不到这里之前就起轮),此处再吞
             * 8 帧 ≈320ms 会把句中话音吃掉——2026-09-20 实测"给我讲个故事吧"被打断
             * 路径吞成"给我故事吧"(speak-start cbuf=0、arm→起轮 273ms 即此吞咽窗)。*/
            if (!g_wake_break && !g_barge_in) {
                unsigned char _pd[TUYA_OPUS_FRAME_LEN];
                for (int i = 0; i < 8 && !g_exit; i++) {  /* 8 × ~40ms ≈ 300ms 排空 mic */
                    /* ★ 帧进 barge 历史(2026-09-20):用户在 TTS 刚结束的这 300ms 里
                     *   追问的话音头会落在此窗——只丢不存则追问轮同样缺首字,回溯
                     *   留底逻辑与 idle 排空一致(静音/残响帧被 8 万能量闸砍掉)。*/
                    if (_device_get_voice_data(_pd, sizeof(_pd)) == TUYA_OPUS_FRAME_LEN) {
                        barge_hist_push(_pd);
                    }
                }
#ifdef TUYA_KWS_ENABLE
                tuya_kws_window_kick();   /* 答毕(TTS 播完)续窗:短时间内可直接追问 */
#endif
            }
        }
#ifdef TUYA_MUSIC_ENABLE
        /* ⑤ 音乐技能交接:本轮云端回了音乐 SKILL(试听 mp3 URL,解析见 tuya_music.c)。
         * 上面的 TTS 排空已把"正在为您播放…"播完 → 停我们的 TTS 解码器让出 DAC →
         * 交给 app_music 网络解码(net_download,https 自动 TLS)→ 等整首播完
         * (dec_end 回调清 g_music_playing)→ 重启 TTS 播放器,回 ① 继续听音。
         * ★ 播放期间不上行(音乐回采不该进 ASR),但可 barge-in 停乐:照搬 TTS drain
         *   的"VAD+能量门"双确认(见下面等待循环)。AEC 对连续音乐的效果未验证,
         *   故开头 1s 不设防、每秒打一次 mic 能量基线([MUSIC-DBG]),实测误触发
         *   ("音乐自己把自己打断")就调 BARGE_MIN_ENERGY。
         * ★ DAC 交接前后各等 300ms:两个解码器共享 DAC,边停边开会踩到
         *   subdevice_dac 的格式重配断言(2026-08-27 提示音教训)。*/
        if (tuya_music_pending() && (g_barge_in || g_wake_break)) {
            /* barge-in/唤醒词 打断了"正在为您播放…":用户已改口/重新唤醒,音乐请求
             * 过时,丢弃。不丢会把用户新话音压在整首歌后面才被听到。*/
            printf("[TUYA-MUSIC] pending music dropped (barge-in/wake)\r\n");
            tuya_music_clear_pending();
        }
        music_handoff(ctx);   /* 正常路径:④ TTS 排空后交接;idle 分支另有兜底调用点 */
#endif
        /* g_barge_in 不在此清:改由 barge-in 新上行起点(循环顶 drain-stale 处)清,让标记贯穿。*/
#ifdef TUYA_CLOUD_OPEN_ENABLE
        /* 纯云端开口防翻滚冷却(打点处):本轮收尾后短暂关开口窗——TTS 轮 1s
         * (AEC 尾/喇叭自激防护),空轮 2s(连续噪声下的翻滚率控制);barge-in
         * 抢答轮在起轮处豁免,不受此窗限制。*/
        g_open_cooldown_until = timer_get_ms() + (turn_had_tts ? 1000 : 2000);
        turn_had_tts = 0;
#endif
        if (g_link_broken) break;   /* 链路断:退出语音循环,本会话收尾交 supervisor 重连 */
    }
#endif /* !TUYA_STREAM_MODE(方案C 轮次循环原样保留,一键切回:注释 app_config.h 的 TUYA_STREAM_MODE 重编译) */

    /* 音频流不在此停(supervisor 下次会话复用):tai 已 deinit 不会再有 on_audio
     * 回包,g_audio_ready 留 1 只在有会话时起作用。*/
    tai_disconnect(ctx);
    tai_ctx_deinit(ctx);
    pal->free(mem);
    return timer_get_ms() - t_start;
}

/* AI 会话监督循环(断线自愈,压测/长时间运行核心):会话断开(on_disconnect /
 * 发送失败 / 建连失败)后退避重建会话:重新 get_session_token → tai_connect。
 * 退避 5s→10s→…→60s 封顶;会话曾健康存活>60s 再断,退避复位 5s。
 * WiFi 断线由 SDK 层自动重连(见 tuya_mqtt_keepalive_task 注释),这里只管
 * AI 会话层。MQTT 心跳线程与音频流都在此启动且跨会话存活:重连等待期间采集
 * 照跑,cbuf 环形覆盖旧数据(无害),会话恢复后 idle-drain 自然消费到新鲜帧。*/
static int tuya_cloud_reset_requested(void)
{
    return s_cloud_reset_type == IOT_RESET_REMOTE_UNBIND ||
           s_cloud_reset_type == IOT_RESET_REMOTE_FACTORY;
}

static int tuya_local_factory_reset_requested(void)
{
    return s_local_factory_reset_requested != 0;
}

/* Return non-zero after ownership has transferred to a terminal reset path.
 * The only unbounded operation in the old implementation was waiting for the
 * MQTT task to exit.  Do not free iot while it may still be in process(); on a
 * bounded-wait timeout reboot with the durable marker and finish on next boot. */
static int tuya_cloud_reset_supervise(iot_client_t *iot)
{
    iot_reset_type_t type = (iot_reset_type_t)s_cloud_reset_type;

    if (!tuya_cloud_reset_requested()) {
        return 0;
    }
    if (!tuya_cloud_reset_write_marker(tuya_cloud_reset_marker(type, 0))) {
        printf("[TUYA] cloud reset marker write failed; identity retained\r\n");
        s_cloud_reset_type = TUYA_CLOUD_RESET_NONE;
        return 0;
    }

    printf("[TUYA] cloud reset supervisor: stop MQTT before deinit\r\n");
    g_mqtt_ka_run = 0;
    for (unsigned int tick = 0;
         tick < TUYA_CLOUD_RESET_STOP_TICKS && !s_mqtt_ka_exited;
         tick++) {
        os_time_dly(1);
    }
    if (!s_mqtt_ka_exited) {
        printf("[TUYA] MQTT stop timed out; rebooting to complete marked reset\r\n");
        tuya_cloud_reset_cpu_reboot();
        return 1;
    }

    iot_client_deinit(iot);
    if (tuya_cloud_reset_clear_and_verify(type) != 0) {
        printf("[TUYA] cloud reset clear verification failed; reboot for recovery\r\n");
        tuya_cloud_reset_cpu_reboot();
        return 1;
    }

    printf("[TUYA] local %s reset state cleared; reboot to BLE provisioning\r\n",
           type == IOT_RESET_REMOTE_FACTORY ? "factory" : "unbind");
    tuya_cloud_reset_cpu_reboot();
    return 1;
}

/* Called only by the task that owns iot. K6 never reaches this function
 * directly, so a key event cannot race iot_client_process(), deinit, or VM
 * writes. */
static int tuya_local_factory_reset_supervise(iot_client_t *iot)
{
    if (!tuya_local_factory_reset_requested()) {
        return 0;
    }

    s_local_factory_reset_requested = 0;
    if (wifi_tuya_network_is_ready()) {
        /* This emits the signed tuya.device.reset request but intentionally
         * does not receive or evaluate its result.  A local physical reset
         * must remain available even if the AP, DNS, TLS or cloud is down. */
        printf("[TUYA] K6 reset: sending cloud device-removal notification\r\n");
        (void)iot_client_factory_reset_notify(iot);
    } else {
        printf("[TUYA] K6 reset: network offline; skipping cloud notification\r\n");
    }

    printf("[TUYA] K6 local factory reset: clearing local state\r\n");
    /* The cloud can also emit protocol 11 for this operation.  Physical K6
     * remains factory policy and does not wait for the notification response. */
    s_cloud_reset_type = IOT_RESET_REMOTE_FACTORY;
    return tuya_cloud_reset_supervise(iot);
}

static void tuya_ai_run(const pal_t *pal, iot_client_t *iot, const char *local_key)
{
    s_mqtt_ka_exited = 0;
    g_mqtt_ka_run = 1;
    if (thread_fork("tuya_mqtt_ka", 5, 6 * 1024, 0, 0,
                    tuya_mqtt_keepalive_task, iot) != 0) {
        printf("[TUYA] MQTT keepalive task start failed\r\n");
        s_mqtt_ka_exited = 1;
    }

    audio_stream_init(16000, 16, 1);
    start_audio_stream();

#ifdef TUYA_KWS_ENABLE
    tuya_kws_init();   /* 唤醒词引擎(幂等,只初始化一次;失败自动回退常听) */
#endif

    unsigned int backoff_ms = 5000;
    int offline_logged = 0;
    while (1) {
        /* An actual cloud removal notification is authoritative. If it races
         * a K6 press, consume it first and do not send a second reset request. */
        if (tuya_cloud_reset_requested()) {
            s_local_factory_reset_requested = 0;
            if (tuya_cloud_reset_supervise(iot)) return;
            continue;
        }
        /* K6 never waits for a network recovery.  If online, the supervisor
         * makes one send-only cloud notification; otherwise it immediately
         * continues with the same local factory-reset state machine. */
        if (tuya_local_factory_reset_requested()) {
            if (tuya_local_factory_reset_supervise(iot)) {
                return;
            }
            continue;
        }
        /* Session-token and TAI connection retries require a DHCP-ready
         * network as well.  Retain the client and resume after Wi-Fi recovers. */
        if (!wifi_tuya_network_is_ready()) {
            if (!offline_logged) {
                printf("[TUYA] network offline, AI reconnect paused\r\n");
                offline_logged = 1;
            }
            while (!wifi_tuya_network_is_ready()) {
                os_time_dly(100);
            }
            printf("[TUYA] network restored, resuming AI reconnect\r\n");
            offline_logged = 0;
            backoff_ms = 5000;
            continue;
        }
        /* 每次会话尝试前复位跨线程标志(tai ctx 尚未创建,on_disconnect 无竞态) */
        g_exit = 0; g_link_broken = 0;
        if (tuya_cloud_reset_requested()) {
            s_local_factory_reset_requested = 0;
            if (tuya_cloud_reset_supervise(iot)) return;
            continue;
        }
        if (tuya_local_factory_reset_requested()) {
            if (tuya_local_factory_reset_supervise(iot)) return;
            continue;
        }
        g_tts_playing = 0; g_turn_done = 1;
#ifdef TUYA_BARGE_IN_ENABLE
        g_barge_in = 0; g_barge_prefill = 0;
#endif
        printf("[TUYA] session attempt\r\n");
        unsigned int lived_ms = tuya_ai_session(pal, iot, local_key);
        if (tuya_cloud_reset_requested()) {
            s_local_factory_reset_requested = 0;
            if (tuya_cloud_reset_supervise(iot)) return;
            continue;
        }
        if (tuya_local_factory_reset_requested()) {
            if (tuya_local_factory_reset_supervise(iot)) return;
            continue;
        }
        if (lived_ms > 60000) backoff_ms = 5000;   /* 会话曾健康存活,按首次失败退避 */
        /* 会话报废收尾:清残留 TTS 让喇叭立刻安静,重连后从干净状态起听 */
        _device_rbuf_clear();
        printf("[TUYA] session lost (lived %us), retry in %ums\r\n",
               lived_ms / 1000, backoff_ms);
        for (unsigned int waited_ms = 0;
             waited_ms < backoff_ms && !tuya_cloud_reset_requested() &&
             !tuya_local_factory_reset_requested();
             waited_ms += 100) {
            msleep(100);
        }
        if (tuya_cloud_reset_requested()) {
            s_local_factory_reset_requested = 0;
            if (tuya_cloud_reset_supervise(iot)) return;
            continue;
        }
        if (tuya_local_factory_reset_requested()) {
            if (tuya_local_factory_reset_supervise(iot)) return;
            continue;
        }
        if (backoff_ms < 60000) backoff_ms *= 2;
    }
}

/* 配网等待期循环播报"请配置网络",每 30s 一次,避免用户以为设备死机。
 * tuya_ble_netcfg_start 阻塞,故用独立线程周期播报;配网完成/失败/超时置
 * s_prov_prompt_run=0,线程在 ~0.1s 内退出。NetCfgEnter.mp3 是 app_music 现有提示音。*/
static volatile int s_prov_prompt_run;

#define TUYA_NETWORK_READY_TIMEOUT_MS  30000u
#define TUYA_NETWORK_READY_POLL_MS       100u

/* accept_current_ready is used only by an already-provisioned boot: an
 * existing DHCP lease for its saved network is sufficient.  A fresh BLE
 * provisioning flow must observe a newer DHCP generation. */
static int tuya_wait_for_network_ready(u32 start_generation, int accept_current_ready)
{
    u32 waited_ms = 0;

    while (waited_ms < TUYA_NETWORK_READY_TIMEOUT_MS) {
        u32 generation = wifi_get_tuya_network_ready_generation();
        if (wifi_tuya_network_is_ready() &&
            (accept_current_ready || generation != start_generation)) {
            printf("[TUYA] network ready: generation %u -> %u\r\n",
                   start_generation, generation);
            return 0;
        }
        msleep(TUYA_NETWORK_READY_POLL_MS);
        waited_ms += TUYA_NETWORK_READY_POLL_MS;
    }
    printf("[TUYA] network ready timeout\r\n");
    return -1;
}

/* A saved, valid provision must survive a temporary AP outage.  After the
 * bounded startup wait, keep cloud startup paused until DHCP eventually
 * recovers instead of clearing the device triplet. */
static void tuya_wait_for_network_available(u32 start_generation)
{
    if (tuya_wait_for_network_ready(start_generation, 1) == 0) {
        return;
    }

    printf("[TUYA] network still offline; cloud startup paused\r\n");
    while (!wifi_tuya_network_is_ready()) {
        os_time_dly(100);
    }
    printf("[TUYA] network available after offline wait\r\n");
}

/* Wi-Fi/DHCP 就绪不代表涂鸦云端激活、凭据落盘和 App 的 MQTT 同步均已完成。
 * 配网过程保持此标志，使 app_music 不会把 NET_EVENT_CONNECTED 当作成功。 */
static volatile int s_tuya_provisioning_active;

int tuya_agentic_provisioning_active(void)
{
    return s_tuya_provisioning_active;
}

/* A DHCP lease only proves that some AP supplied an address.  The generic
 * Wi-Fi recovery path can otherwise reconnect a remembered AP while a Tuya
 * provisioning attempt is active.  Compare the current STA association with
 * the exact UTF-8 bytes supplied over BLE before accepting that lease. */
static int tuya_provisioning_ssid_matches_current_sta(void)
{
    struct wifi_mode_info info = {0};
    size_t configured_len = strlen(s_main_creds.ssid);
    size_t associated_len;

    info.mode = STA_MODE;
    wifi_get_mode_cur_info(&info);
    if (info.mode != STA_MODE || !info.ssid) {
        printf("[TUYA] provisioning associated SSID unavailable after DHCP\r\n");
        return 0;
    }

    associated_len = strlen(info.ssid);
    if (configured_len != associated_len ||
        memcmp(info.ssid, s_main_creds.ssid, configured_len) != 0) {
        printf("[TUYA] provisioning SSID mismatch: configured_len=%u associated_len=%u\r\n",
               (unsigned int)configured_len, (unsigned int)associated_len);
        return 0;
    }

    printf("[TUYA] provisioning SSID verified: bytes=%u\r\n",
           (unsigned int)configured_len);
    return 1;
}

/* No Tuya identity has been persisted yet, so a clean CPU reset is sufficient
 * to begin a new BLE provisioning attempt.  Keep the activity guard set if a
 * platform reset unexpectedly returns: an old remembered SSID must still not
 * be accepted as this attempt's network. */
static void tuya_restart_ble_provisioning_after_wifi_failure(const char *reason)
{
    extern void cpu_reset(void);

    printf("[TUYA] provisioning Wi-Fi %s; reboot to BLE provisioning\r\n", reason);
    cpu_reset();
    printf("[TUYA] ERROR: cpu_reset returned; provisioning remains active\r\n");
}

/* syscfg_write 没有可依赖的错误返回约定；以读回的字节数和内容确认必需数据
 * 已经落盘。所有条目不通过时都不能解除配网活动态、更不能播成功音。 */
static int tuya_write_vm_and_verify(u16 index, const void *data, u16 len, const char *name)
{
    unsigned char readback[65];

    if (len > sizeof(readback)) {
        printf("[TUYA] VM verify buffer too small: %s len=%u\r\n",
               name, (unsigned int)len);
        return -1;
    }

    syscfg_write(index, (void *)data, len);
    if (syscfg_read(index, readback, len) != len || memcmp(readback, data, len) != 0) {
        printf("[TUYA] VM persistence verify failed: %s\r\n", name);
        return -1;
    }
    return 0;
}

static int tuya_save_required_provision_data(const iot_client_t *iot)
{
    unsigned char region_byte = (unsigned char)iot->region;
    int failed = 0;

    failed |= tuya_write_vm_and_verify(VM_TUYA_DEVID_IDX, iot->devid, 32, "devid");
    failed |= tuya_write_vm_and_verify(VM_TUYA_SECRET_IDX, iot->secret_key, 32, "secret_key");
    failed |= tuya_write_vm_and_verify(VM_TUYA_LOCALKEY_IDX, iot->local_key, 32, "local_key");
    failed |= tuya_write_vm_and_verify(VM_TUYA_SSID_IDX, s_main_creds.ssid, 65, "ssid");
    failed |= tuya_write_vm_and_verify(VM_TUYA_PWD_IDX, s_main_creds.password, 65, "password");
    failed |= tuya_write_vm_and_verify(VM_TUYA_REGION_IDX, &region_byte, 1, "region");

    if (failed) {
        return -1;
    }

    printf("[TUYA] required provisioning data persisted, region=%s\r\n",
           region_name(iot->region));
    return 0;
}

static void tuya_prov_prompt_task(void *arg)
{
    extern void app_music_play_netcfg_prompt(void);
    while (s_prov_prompt_run) {
        app_music_play_netcfg_prompt();   /* 先播"请配置网络",别让用户干等 */
        for (int i = 0; i < 300 && s_prov_prompt_run; i++) {
            os_time_dly(10);   /* 100ms × 300 = 30s;100ms 粒度查退出标志 */
        }
    }
}

/* 把涂鸦配网/直连用的 ssid/pwd 同步到杰理 wifi 模块的存储(VM)。
 * 修复 bug:杰理 app_music 的 wifi_return_sta_mode() 会读 VM 里的 ssid 重连,
 * 若 VM 残留旧 ssid(如换网络/换路由器后),会覆盖涂鸦配的 ssid 导致断网。
 * 涂鸦每次连 WiFi 后调本函数,让杰理 VM 和涂鸦保持一致,wifi_return_sta_mode
 * 读到的就是涂鸦配的正确 ssid。*/
static void tuya_sync_wifi_to_jl(const char *ssid, const char *pwd)
{
    if (ssid && ssid[0] && ssid[0] != 0xFF) {
        wifi_store_mode_info(STA_MODE, (char *)ssid, (char *)pwd);
        printf("[TUYA] synced wifi to JL store: ssid_len=%u\r\n",
               (unsigned int)strlen(ssid));
    }
}

static int tuya_vm_string_valid(const char *value, int read_len, size_t value_size)
{
    return read_len > 0 && value[0] != '\0' &&
           (unsigned char)value[0] != 0xFF &&
           memchr(value, '\0', value_size) != NULL;
}

/* A Wi-Fi password may intentionally be empty for an open network, but its
 * VM record still needs to be present, non-erased, and NUL-terminated. */
static int tuya_vm_wifi_password_valid(const char *value, int read_len, size_t value_size)
{
    return read_len > 0 && (unsigned char)value[0] != 0xFF &&
           memchr(value, '\0', value_size) != NULL;
}

/* ========================================================================= */
/* 涂鸦 SDK 日志重定向:把 SDK 的日志输出从 fprintf(stderr) 改成 printf(UART)。*/
/*                                                                           */
/* 背景:涂鸦 SDK 默认 log_default_handler 用 fprintf(stderr,...) 输出日志。  */
/*   但杰理 AC791N 的 newlib stdio 没有完整初始化,stderr 指向的 FILE 结构体  */
/*   悬空(buffer/write 函数指针无效),fprintf(stderr) 会访问野指针崩溃。       */
/*   之前用 log_set_level(0) 关掉所有日志规避,但导致 SDK 层日志全不可见      */
/*   (云端VAD配置响应、SERVER_VAD事件接收等都没法调试)。                      */
/*                                                                           */
/* 方案:注册自定义 log handler,用 vsnprintf 格式化到本地 buffer 后用 printf  */
/*   输出。printf 在杰理上重定向到 UART(串口),不会崩溃。加互斥锁防多线程    */
/*   并发(SDK 的 on_event/on_audio 回调在 worker 线程,主循环在 agentic 线程)。*/
/* ========================================================================= */
#include "tai_log.h"   /* log_set_handler / LOG_* 枚举 */

/* 日志级别控制:改这个值即可控制 SDK 日志输出量。
 * LOG_ERROR=1(只看错误) / LOG_WARN=2(+警告) / LOG_INFO=3(+信息) / LOG_DEBUG=4(全开)
 * 调试云端VAD等问题时设 LOG_DEBUG;平时设 LOG_WARN 减少刷屏。*/
/* 日志级别:LOG_INFO(3) 能看到激活/连接/ASR等关键流程。
 * 杰理 printf 把 64 位长度符(z/ll)当双槽读,曾致激活崩机;kit 内 %zu/%llu
 * 已于 2026-10-06 全部扫成 %u(见 UPSTREAM-BASE.md),LOG_DEBUG 的包日志
 * 路径现在也安全,需要细查时可放开。*/
#define TUYA_SDK_LOG_LEVEL  LOG_INFO

static OS_MUTEX tuya_log_mutex;   /* 防多线程并发输出交叉 */
static int tuya_log_mutex_inited;

static void tuya_log_redirect(log_level_t level, const char *fmt, va_list args)
{
    extern void putbyte(char a);   /* include_lib/system/generic/printf.h */

    static const char level_char[] = {'-', 'E', 'W', 'I', 'D'};
    char lc = (level >= 0 && level <= 4) ? level_char[level] : '?';

    /* 格式化到本地 buffer(vsnprintf 安全:超长截断不溢出)。
     * SDK 单条日志通常 < 200 字节,256 够用。*/
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt ? fmt : "", args);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;   /* 截断 */

    /* F‡(2026-10-06):输出不经 printf,改 putbyte 逐字节直写 UART。
     * 【定案】激活期 axi_rd_inv 五连崩真凶 = kit 日志里的 %zu/%.4s:杰理
     * printf 把 'z'/'ll' 当 64 位长度符,va_arg 吃双槽 → 参数流错位一格 →
     * 后面的 %.4s 把栈垃圾当指针解引用 → 非法 AXI 读。判别链:F′(整体旁路)
     * 绿;F‡(留 vsnprintf)遇 Token 行(%zu)2/2 崩且字节未出 → vsnprintf
     * 定罪;9/30"格式符曾跑绿"反例经 git 证伪(stage-1 Token 行是 %u,
     * 13 处 %zu 全在冷路径未执行)。printf 输出机制(lbuf/flush)洗清嫌疑。
     * 2026-10-06 已扫 kit 全部格式符(%zu→%u 35 处、%llu→(unsigned)%u 6 处,
     * overlay UPSTREAM-BASE.md 有记)。putbyte 通道暂留,是否回退 printf
     * 输出待崩机案板测定案后另议。
     * putbyte:对 '\n' 自动展开 CRLF、吞裸 '\r',故结尾只发 '\n'。
     * 互斥保留:putbyte 无锁,防多任务行内交叉。*/
    if (tuya_log_mutex_inited) os_mutex_pend(&tuya_log_mutex, 0);
    {
        char line[280];   /* 13 前缀 + 255 buf + 余量 */
        const char *q;
        char *p = line;
        *p++ = '['; *p++ = 'T'; *p++ = 'U'; *p++ = 'Y'; *p++ = 'A';
        *p++ = '-'; *p++ = 'S'; *p++ = 'D'; *p++ = 'K'; *p++ = '/';
        *p++ = lc;  *p++ = ']'; *p++ = ' ';
        memcpy(p, buf, (size_t)n);
        p += n;
        *p++ = '\n';
        for (q = line; q < p; q++) putbyte(*q);
    }
    if (tuya_log_mutex_inited) os_mutex_post(&tuya_log_mutex);
}

void tuya_clear_provision_and_reset(void);

void tuya_agentic_main(void *arg)
{
    const pal_t *pal = tai_pal_ac791n();

    /* 等 BT/WiFi 控制器初始化完成(late_initcall 跑得太早,BT 还没起来)。
     * 实测 BT_STATUS_INIT_OK 在开机 ~2.5s 到达,等 4s 留足裕量即可。原 15s 过度保守,
     * 导致"提示配网后要干等十几秒 BLE 才广播、App 才扫到"。netcfg_start 本身是瞬时的。*/
    printf("[TUYA] waiting ~4s for BT/WiFi init...\r\n");
    os_time_dly(400);    /* 400 * 10ms = 4s */

    printf("===== tuya_agentic_main start =====\r\n");
    {
        extern const char *tuya_get_effective_sw_ver(void);
        printf("[TUYA] fw ver: base=%s effective=%s\r\n",
               TUYA_FIRMWARE_VERSION, tuya_get_effective_sw_ver());
    }

    /* ===== [阶段2] 授权区加载(2026-09-28 首烧实测后启用):成功→三元组注入点用区域码;
     * 失败→回退顶部默认宏(STRICT=0,开发板行为不变;STRICT=1 在"首次配网"消费点拦截)。
     * 实现独立在 tuya_auth_region.c(fs/fs.h 与 newlib stdio.h 冲突,不能并入本文件)。===== */
    tuya_auth_region_probe();   /* 诊断行:区地址/头16B(阶段1遗留,保留) */
    if (tuya_auth_region_load(&g_tuya_triplet) == 0) {
        g_tuya_triplet_valid = 1;
        printf("[TUYA] triplet source: USER region @0x5FE000\r\n");
    } else {
        g_tuya_triplet_valid = 0;
#if TUYA_AUTH_REGION_STRICT
        printf("[TUYA] triplet source: NONE (region invalid; STRICT blocks provisioning)\r\n");
#else
        printf("[TUYA] triplet source: built-in macros (region empty/invalid)\r\n");
#endif
    }

#if TUYA_AUTH_RW_TEST == 1
    /* [一次性验证钩子] erase 通路板上验证(2026-09-28 已收官):load 成功后擦 4K+FF 回读+probe 复证。
     * 只在本次开机读到有效授权码时执行,擦一次区即空,重启后 g_tuya_triplet_valid=0 自然跳过。
     * 验证完把 app_config.h 的 TUYA_AUTH_RW_TEST 置 0。*/
    if (g_tuya_triplet_valid) {
        printf("[TUYA_AUTH] RW_TEST: erasing zone...\r\n");
        tuya_auth_region_erase();
        tuya_auth_region_probe();   /* 复证:头16B 应全 FF */
    }
#elif TUYA_AUTH_RW_TEST == 2
    /* [一次性验证钩子] write 通路板上验证(2026-09-28 收尾):区无效→用顶部默认宏自写号
     * (擦4K→写96B→回读逐字节比对),随后重 load 并把来源切成 USER 区——写→读→消费
     * 全链一次开机走完。已写号的开机 load 直接成功,本分支不执行(不重擦不重写);
     * 因此 K6/复位后若再现 "write OK" = 区被清过、保留失败——判定就看这一行。验证完置回 0。*/
    if (!g_tuya_triplet_valid) {
        tuya_auth_region_t t;
        memset(&t, 0, sizeof(t));
        strncpy(t.product_key, TUYA_PRODUCT_KEY, sizeof(t.product_key) - 1);
        strncpy(t.uuid,        TUYA_UUID,        sizeof(t.uuid) - 1);
        strncpy(t.auth_key,    TUYA_AUTH_KEY,    sizeof(t.auth_key) - 1);
        printf("[TUYA_AUTH] RW_TEST: self-provision zone from built-in macros...\r\n");
        if (tuya_auth_region_write(&t) == 0 && tuya_auth_region_load(&g_tuya_triplet) == 0) {
            g_tuya_triplet_valid = 1;
            printf("[TUYA] triplet source: USER region @0x5FE000 (self-provisioned)\r\n");
        }
    }
#endif

    if (iot_init(pal) != 0) { printf("[TUYA] iot_init fail\r\n"); return; }

    /* 注册自定义日志 handler:把 SDK 日志从 fprintf(stderr)(会崩溃)重定向到 printf(UART)。
     * 原来用 log_set_level(0) 关掉所有日志规避崩溃,但导致云端VAD配置响应等无法调试。
     * 现在用 tuya_log_redirect 替代,既能看日志又不会崩溃。*/
    os_mutex_create(&tuya_log_mutex);
    tuya_log_mutex_inited = 1;
    log_set_handler(tuya_log_redirect);
    log_set_level(TUYA_SDK_LOG_LEVEL);   /* 见上方宏,改它即可控制日志量 */

    if (tuya_cloud_reset_recover_if_needed() != 0) {
        printf("[TUYA] cloud-reset recovery incomplete; refusing to start with saved identity\r\n");
        return;
    }

    char devid[32] = {0}, secret[32] = {0}, localkey[32] = {0};
    /* 仅 devid 存在不等于身份有效：三个凭据都必须完整且非空，否则直接进入
     * 既有 BLE 配网入口，绝不尝试带半套/已清除凭据连云。*/
    int have = syscfg_read(VM_TUYA_DEVID_IDX, devid, sizeof(devid)) == sizeof(devid) &&
               syscfg_read(VM_TUYA_SECRET_IDX, secret, sizeof(secret)) == sizeof(secret) &&
               syscfg_read(VM_TUYA_LOCALKEY_IDX, localkey, sizeof(localkey)) == sizeof(localkey) &&
               devid[0] != 0 && devid[0] != 0xFF &&
               secret[0] != 0 && secret[0] != 0xFF &&
               localkey[0] != 0 && localkey[0] != 0xFF;
    if (have) {
        printf("[TUYA] already provisioned, devid=%s\r\n", devid);

        /* 直连路径必须自己重连 WiFi:开机 wifi 子系统落回 SMP_CFG_MODE 监听(没联网),
         * 直接 iot_client_init 连 AI 云必失败→"说话没反应"。读配网时一并存的 ssid/password,
         * 复用首次配网那套 wifi_enter_sta_mode + 轮询 SUCC + 等 DHCP(见下方首次配网段)。*/
        int secret_len = sizeof(secret);
        int localkey_len = sizeof(localkey);
        char ssid[65] = {0}, pwd[65] = {0};
        int ssid_len = syscfg_read(VM_TUYA_SSID_IDX, ssid, sizeof(ssid));
        int pwd_len = syscfg_read(VM_TUYA_PWD_IDX,  pwd,  sizeof(pwd));
        if (!tuya_vm_string_valid(secret, secret_len, sizeof(secret)) ||
            !tuya_vm_string_valid(localkey, localkey_len, sizeof(localkey)) ||
            !tuya_vm_string_valid(ssid, ssid_len, sizeof(ssid)) ||
            !tuya_vm_wifi_password_valid(pwd, pwd_len, sizeof(pwd))) {
            /* Only a corrupt/missing local provision justifies clearing it.
             * Association failures and DHCP timeouts never enter this path. */
            printf("[TUYA] invalid stored provisioning data -> reset to BLE provisioning\r\n");
            tuya_clear_invalid_provision_and_reset();
            return;
        }

        printf("[TUYA] already provisioned; verifying Wi-Fi before cloud startup\r\n");
        u32 network_generation = wifi_get_tuya_network_ready_generation();
        if (!wifi_tuya_network_is_ready()) {
            /* Tuya's persisted credentials are authoritative here.  They let
             * startup recover even if the separate JieLi Wi-Fi VM is stale. */
            wifi_enter_sta_mode(ssid, pwd);
        }
        tuya_wait_for_network_available(network_generation);
        tuya_sync_wifi_to_jl(ssid, pwd);

        iot_client_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        /* region 从 VM 恢复(配网时下发存的),不硬编码——设备不预知会被部署到哪个区,
         * region 决定 ATOP/MQTT 域名(schema 拉取/OTA/AI token/MQTT 接入)。
         * 读不到(旧固件升级/异常)或值非法时兜底中国区 AY。*/
        {
            uint8_t vm_region = AY;
            if (syscfg_read(VM_TUYA_REGION_IDX, &vm_region, 1) <= 0 || vm_region > SG) {
                printf("[TUYA] VM region absent/invalid, fallback AY\r\n");
                vm_region = AY;
            }
            cfg.region = (iot_region_t)vm_region;
            printf("[TUYA] region=%s (from VM)\r\n", region_name(cfg.region));
        }
        cfg.env = PROD;
        cfg.mqtt_disable_tls = false; cfg.mqtt_disable_auto_connect = false;
        cfg.cert_bundle_attach = NULL; cfg.cacert = NULL;
        cfg.reset_callback = on_cloud_reset;
    cfg.ota_confirm_callback = on_ota_confirm;   /* APP 确认升级(protocol 15) */
        extern const char *tuya_get_effective_sw_ver(void);
        cfg.sw_ver = tuya_get_effective_sw_ver();   /* 上报生效版本:USER区记录优先,无记录回退源码宏(见 tuya_ota.c"版本号管理") */

        strncpy((char *)cfg.devid, devid, sizeof(cfg.devid) - 1);
        strncpy((char *)cfg.secret_key, secret, sizeof(cfg.secret_key) - 1);
        strncpy((char *)cfg.local_key, localkey, sizeof(cfg.local_key) - 1);

        /* DP schema 恢复:只恢复 schema_id(非空才能触发云端查询),不恢复 schema。
         * 原因:schema JSON 601 字节,VM 单 entry 有大小限制(~68B),直接存会截断→JSON 不完整。
         * 改为:开机时 schema_id 恢复(小,32B 够),schema 留空→iot_dp_schema_check_update 从云端全量拉取。*/
        static char vm_schema_id[64] = {0};
        if (syscfg_read(VM_TUYA_SCHEMAID_IDX, vm_schema_id, sizeof(vm_schema_id)) > 0
            && vm_schema_id[0] != 0 && vm_schema_id[0] != 0xFF) {
            cfg.schema_id = vm_schema_id;
            printf("[TUYA] restored schema_id=%s\r\n", vm_schema_id);
        }
        /* cfg.schema 不从 VM 恢复(会截断),靠下方 iot_dp_schema_check_update 从云端拉 */

        iot_client_t *iot = iot_client_init(&cfg);
        if (iot) {
            s_tuya_cloud_client_ready = 1;
            iot_ai_ctrl_set_callback(iot, on_ai_ctrl, NULL);   /* 阶段2·9000 AI控制通道(见 on_ai_ctrl) */
            /* 连 AI 之前先检查涂鸦云 OTA(此时 iot_client 活着,且 AI 会话还没起,
             * 无并发冲突;OTA 走 ATOP over HTTPS,与 MQTT 串行无妨)。
             * 有升级则下载烧写并自动重启(不返回);无升级则正常连 AI。*/
#if TUYA_OTA_ENABLE
            extern int tuya_ota_check_and_upgrade(iot_client_t *client);
            if (tuya_ota_check_and_upgrade(iot) == 1) {
                return;   /* 升级成功,等待自动重启,不再连 AI */
            }
#endif
            /* DP:从涂鸦云端拉取 DP schema(设备知道自己在平台配了哪些 DP),
             * 并注册 DP 下行回调(云端下发 DP 值时触发)。
             * 不拉 schema → iot_dp_dispatch_downlink 无法解析 DP 下发(不知道 dp_id 对应什么类型)。*/
            {
                extern void on_dp_downlink(uint8_t dp_id, const iot_dp_value_t *value, void *user_data);
                extern void on_dp_schema_update(const char *schema_id, const char *new_schema, void *user_data);
                iot_dp_set_callback(iot, (iot_dp_callback_t)on_dp_downlink, NULL);
                iot_dp_set_schema_update_callback(iot, (iot_schema_update_callback_t)on_dp_schema_update, NULL);
                printf("[TUYA] pulling DP schema from cloud...\r\n");
                int dp_ret = iot_dp_schema_check_update(iot);
                printf("[TUYA] DP schema check_update ret=%d schema=%s len=%d\r\n",
                       dp_ret, iot->schema ? "NON-NULL" : "NULL",
                       iot->schema ? (int)strlen(iot->schema) : 0);
                /* 若 check_update 未拉到 schema(version 门控/云端无更新),
                 * 当前 loose 模式会吞掉所有 DP 下行。打印状态辅助排查。*/
            }
            tuya_ai_run(pal, iot, localkey);
        }
        else { printf("[TUYA] iot_client_init fail\r\n"); }
        return;
    }

    /* ---- 首次:BLE 配网 ---- */
#if TUYA_AUTH_REGION_STRICT
    /* 量产:漏烧码设备(授权区无效)拦在配网前,明确报错不广播(方案§5.3/§7)。
     * 已激活设备(VM 有 devid)走上方直连路径,不会到这,不受影响。 */
    if (!g_tuya_triplet_valid) {
        printf("[TUYA_AUTH] STRICT: region has no valid triplet, refuse provisioning\r\n");
        return;
    }
#endif
    s_tuya_provisioning_active = 1;
    printf("[TUYA] no devid, start BLE provisioning...\r\n");
    s_prov_prompt_run = 1;   /* 启动"请配置网络"循环播报(每 30s),配网完成会停 */
    thread_fork("tuya_prov_prompt", 6, 4 * 1024, 0, 0, tuya_prov_prompt_task, NULL);
    int prov_ret = tuya_ble_netcfg_start("TUYA", tuya_trip_pk(), tuya_trip_uuid(), tuya_trip_key(), main_prov_cb);
    s_prov_prompt_run = 0;   /* 配网完成/失败/超时,停循环播报 */
    os_time_dly(15);         /* ~150ms:让 prompt 线程看到标志退出,别让它播报到连 WiFi/激活阶段 */
    if (prov_ret != 0) {
        printf("[TUYA] BLE provisioning failed/timeout\r\n"); return;
    }
    tuya_ble_netcfg_stop();   /* 配网完停 BLE,释放内存给 WiFi/TLS */
    printf("[TUYA] BLE done: ssid_len=%u token_len=%u\r\n",
           (unsigned int)strlen(s_main_creds.ssid),
           (unsigned int)strlen(s_main_creds.token));

    /* ---- 连 WiFi(配网给的 ssid/密码)---- */
    u32 network_generation = wifi_get_tuya_network_ready_generation();
    wifi_enter_sta_mode(s_main_creds.ssid, s_main_creds.password);
    if (tuya_wait_for_network_ready(network_generation, 0) != 0) {
        /* No cloud activation attempt is made without a new DHCP lease.
         * Restart cleanly so the user can submit corrected BLE credentials. */
        tuya_restart_ble_provisioning_after_wifi_failure("unavailable");
        return;
    }
    if (!tuya_provisioning_ssid_matches_current_sta()) {
        /* Never activate, persist, or announce success for a remembered AP
         * that happened to reconnect during this BLE provisioning attempt. */
        tuya_restart_ble_provisioning_after_wifi_failure("SSID mismatch");
        return;
    }

    /* ---- on_boarding 激活 ----
     * (杰理 wifi 存储同步已挪到激活成功之后,见下方 tuya_sync_wifi_to_jl)*/
    iot_on_boarding_config_t ob;
    memset(&ob, 0, sizeof(ob));
    strncpy((char *)ob.uuid,        tuya_trip_uuid(), sizeof(ob.uuid) - 1);
    strncpy((char *)ob.authkey,     tuya_trip_key(),  sizeof(ob.authkey) - 1);
    strncpy((char *)ob.product_key, tuya_trip_pk(),   sizeof(ob.product_key) - 1);
    ob.env = PROD; ob.mqtt_disable_tls = false; ob.mqtt_disable_auto_connect = false; ob.timeout_ms = 30000;
    ob.cert_bundle_attach = NULL; ob.cacert = NULL;
    ob.reset_callback = on_cloud_reset;
    ob.ota_confirm_callback = on_ota_confirm;   /* APP 确认升级(protocol 15) */
    extern const char *tuya_get_effective_sw_ver(void);
    ob.sw_ver = tuya_get_effective_sw_ver();   /* 上报生效版本:USER区记录优先,无记录回退源码宏(见 tuya_ota.c"版本号管理") */
    iot_client_t *iot = iot_client_init_on_boarding_with_token(&ob, s_main_creds.token);
    if (!iot) { printf("[TUYA] on_boarding_with_token fail\r\n"); return; }
    if (!iot->devid[0] || !iot->secret_key[0] || !iot->local_key[0]) {
        printf("[TUYA] activation returned incomplete required credentials\r\n");
        return;
    }
    s_tuya_cloud_client_ready = 1;
    iot_ai_ctrl_set_callback(iot, on_ai_ctrl, NULL);   /* 阶段2·9000 AI控制通道(见 on_ai_ctrl) */
    printf("[TUYA] activated, devid=%s\r\n", iot->devid);

    /* 同步到杰理 wifi 存储:防 app_music 的 wifi_return_sta_mode 读到旧 ssid 覆盖。
     * 之前换网络后,杰理 VM 里残留旧 ssid(GJ1)覆盖了涂鸦配的 ssid,导致断网连不上 AI。
     * ⚠️ 2026-10-06 从"激活之前"挪到"激活成功之后":wifi_store_mode_info 内部起
     *   _rpc worker 做 VM flash 擦写,原先紧贴激活打印块执行,板上两次同点
     *   axi_rd_inv 崩机(疑 VM 写 vs 双核 XIP 取指竞态,VM 写距崩点仅 47ms)。
     *   挪开后与激活 HTTPS 窗口隔开数秒;若需挪回,务必保留与激活打印块的间隔。*/
    tuya_sync_wifi_to_jl(s_main_creds.ssid, s_main_creds.password);

    /* ---- 持久化三元组 + WiFi 凭据(下次开机直连)----
     * 三元组连涂鸦 AI 云;ssid/password 供直连路径开机重连 WiFi(否则开机离线,连 AI 必失败)。*/
    char lk[32] = {0};
    strncpy(lk, (const char *)iot->local_key, sizeof(lk) - 1);
    if (tuya_save_required_provision_data(iot) != 0) {
        printf("[TUYA] required provisioning data was not persisted\r\n");
        return;
    }
    /* DP schema:激活时云端在 response 里返回 schema_id + schema JSON。
     * 存到 VM,下次开机直连时读出来恢复→iot_dp_schema_check_update 才能工作→DP 下行才能解析。*/
    if (iot->schema_id[0] != '\0') {
        syscfg_write(VM_TUYA_SCHEMAID_IDX, iot->schema_id, sizeof(iot->schema_id));
        printf("[TUYA] saved schema_id=%s\r\n", iot->schema_id);
    } else {
        printf("[TUYA] WARNING: schema_id is empty after activation!\r\n");
    }
    if (iot->schema && iot->schema[0] != '\0') {
        int slen = (int)strlen(iot->schema);
        syscfg_write(VM_TUYA_SCHEMA_IDX, iot->schema, slen + 1);
        printf("[TUYA] saved schema len=%d: %.80s\r\n", slen, iot->schema);
    } else {
        printf("[TUYA] WARNING: schema body is NULL/empty after activation! "
               "(cloud did not return schema in activate response)\r\n");
    }
    /* ---- hold MQTT 让 App 判定配网成功 ----
     * on_boarding 已建好 MQTT(涂鸦IoT云,设备上线绑定)。App 判"配网成功"靠它,需保持
     * 几秒让云端同步给 App。之后 tuya_ai_run 保持 MQTT 常驻不断开(MQTT 与 AI 是各自
     * 独立的 TCP 连接,可并存;tuya_mqtt_ka 线程维持心跳并收 DP 下行)。
     * ⚠️ 原 8s 过长(配网后用户要干等 8s 才能说话);涂鸦 App 同步配网成功通常 1~2s,
     *   改 3s 足够,缩短开机到可对话的延迟。*/
    printf("[TUYA] hold MQTT ~3s for app to confirm provisioning...\r\n");
    msleep(3000);
    s_tuya_provisioning_active = 0;
    extern void app_music_play_netcfg_success(void);
    app_music_play_netcfg_success();

    /* ---- 连 AI 之前先检查涂鸦云 OTA(与直连路径保持一致)----
     * 配网首次激活后云端一般无待升级固件,但保持检查可应对"激活即升级"场景。
     * 有升级则下载烧写并自动重启;无升级则正常连 AI。*/
#if TUYA_OTA_ENABLE
    {
        extern int tuya_ota_check_and_upgrade(iot_client_t *client);
        if (tuya_ota_check_and_upgrade(iot) == 1) {
            printf("===== tuya_agentic_main end (OTA, wait reboot) =====\r\n");
            return;   /* 升级成功,等待自动重启,不再连 AI */
        }
    }
#endif

    /* DP:注册 DP 下行回调(配网路径也要加,和直连路径保持一致)。
     * on_boarding 时云端已返回 schema 并已存 VM(上方 saved schema_id/schema),
     * 但 iot_client 本身已有 schema_id/schema(从 activate_device 返回的),
     * 可直接注册回调,无需再 check_update。*/
    {
        extern void on_dp_downlink(uint8_t dp_id, const iot_dp_value_t *value, void *user_data);
        iot_dp_set_callback(iot, (iot_dp_callback_t)on_dp_downlink, NULL);
        printf("[TUYA] DP callback registered (provisioning path)\r\n");
    }

    /* ---- 连 AI ---- */
    tuya_ai_run(pal, iot, lk);
    printf("===== tuya_agentic_main end =====\r\n");
}

/* 长按 KEY_PHOTO(K6) 时由 app_music 调用。存在 iot_client_t 时，按键任务
 * 只请求 Agentic 主任务发送一次 tuya.device.reset 并复用可恢复清除状态机；
 * 不等待云端响应。尚无 client（包括离线启动）时走独立本地路径，确保 K6
 * 始终可以恢复出厂。
 * 板上 Reset/Update 是硬件键(固件读不到),电源键是自锁拨动开关(按不出长按),
 * 故重置入口放在 K6 长按(原本是空槽位)。*/
void tuya_clear_provision_and_reset(void)
{
    if (s_local_factory_reset_requested) {
        printf("[TUYA] K6 reset already pending\r\n");
        return;
    }
    s_local_factory_reset_requested = 1;
    g_exit = 1;  /* make an active TAI session return to its supervisor */
    if (!s_tuya_cloud_client_ready) {
        if (!tuya_local_factory_reset_without_client()) {
            s_local_factory_reset_requested = 0;
        }
        return;
    }
    printf("[TUYA] K6 reset queued\r\n");
}

/* boot 自启动:系统初始化后 fork tuya_agentic_main
 * ⚠️ 前提:BLE 控制器、WiFi 子系统此时已就绪;若 BLE 起不来,把该 initcall
 *    挪到 WiFi/BT 初始化完成之后,或由按键/事件触发。*/
static int tuya_agentic_main_init(void)
{
    /* 16K→24K(2026-10-06 激活崩机排查:顺手排除栈溢出假说;激活打印链
     * printf 格式化深度 + 阶段2日志双城 buf[256],留足余量成本低)。*/
    return thread_fork("tuya_agentic", 4, 24 * 1024, 0, 0, tuya_agentic_main, NULL);
}
late_initcall(tuya_agentic_main_init);
