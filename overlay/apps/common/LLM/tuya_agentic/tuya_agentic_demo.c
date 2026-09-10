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
#include "tuya_ble_prov.h"        /* tuya_ble_wifi_creds_t(BLE 配网结果类型)*/
#ifdef TUYA_MUSIC_ENABLE
#include "tuya_music.h"           /* 音乐 SKILL 文本流重组+解析(实现 tuya_music.c) */
/* 实现见 apps/wifi_story_machine/app_music.c(导出模式同 app_music_play_netcfg_prompt):
 * net_download(https 自动 TLS)→ mp3 解码;on_dec_end 在播完/出错停机时回调。*/
int  app_music_tuya_play_url(const char *url, void (*on_dec_end)(int));
void app_music_tuya_music_stop(void);
int  app_music_tuya_music_busy(void);   /* 网络音乐仍占用(下载/解码中):等待循环感知失败退出 */
void app_music_tuya_play_wake_prompt(void); /* "嘿tuya"唤醒应答提示音(WakeHeyTuya.mp3) */
#endif
#ifdef TUYA_UPLINK_OPUS_ENABLE
#include "tuya_opus_enc.h"        /* 上行 opus 软编码(libopus 1.4 定点,实现 tuya_opus_enc.c) */
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
#define TUYA_PRODUCT_KEY    "YOUR_PID_HERE"              /* PID */
#define TUYA_UUID           "YOUR_UUID_HERE"          /* 设备 UUID */
#define TUYA_AUTH_KEY       "YOUR_AUTHKEY_HERE"  /* 授权码 AuthKey */

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

#define TUYA_WAIT_MS    60000
#define TUYA_BARGE_COOLDOWN_MS  1000   /* barge-in 后冷却(ms):此窗口内忽略老轮 chat_break 后在途 TTS 残响引起的二次触发(竞态) */
/* 起轮单帧能量门:Σ|int16|/帧。仅用于空闲起轮(无 TTS,底噪低),100k 够用;
 * 真话音 onset 实测 110万~220万,回声/噪音远低于此。 */
#define BARGE_MIN_ENERGY        100000u
/* 播放中 barge-in 3帧确认门(每帧都要≥此值)。2026-09-04 深圳天气轮日志:
 * AEC 残留骗过 3/3 确认三次(各帧 sum 最高仅 38.4万)→ 天气播报被掐、碎片
 * 轮错乱;真人插话确认帧全部 ≥115.9万。取 60万:误触发余量 1.6×,真人余量
 * 1.9×。贴耳小声插话若失灵,降到 50万;TTS 仍被误掐则升到 80万。 */
#define BARGE_CONFIRM_ENERGY    600000u

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
static volatile int g_mqtt_ka_run;        /* MQTT 心跳线程运行标志:生命周期=整个 tuya 流程,不跟 AI 会话共存亡 */
static int g_audio_frame_logged;          /* 下行首帧帧长只打印一次,供核对 opus_cbr_pktlen */
#ifdef TUYA_SERVER_VAD_ENABLE
static volatile int g_server_vad_stop;    /* 云端VAD(TAI_EVT_SERVER_VAD)通知停说:上行循环据此收尾。on_event 在 worker 线程置位,主循环读 */
#endif
#if TUYA_STM_MCP_VIA_TEXT
/* STM 暂用 TEXT 承载 MCP response，云端会误把 JSON 当作一轮用户文本并生成
 * NLG/TTS。下一轮 AUDIO 前由 demo 任务 chat_break 隔离这轮污染。 */
static volatile unsigned s_mcp_text_reply_pending;
static volatile unsigned s_mcp_text_break_request;
static volatile unsigned s_mcp_text_break_sent;
static volatile unsigned s_mcp_text_reply_deadline;
#endif
static volatile unsigned int g_barge_cooldown_until; /* barge-in 冷却到期 ms 时间戳;此前的 g_tts_playing 视为老轮在途残响,忽略(0=始终过期) */
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
static void tuya_mqtt_keepalive_task(void *arg)
{
    extern int  iot_client_message_connect(iot_client_t *client);    /* src/iot_client_message.h 未进公共头 */
    extern void iot_client_message_disconnect(iot_client_t *client);
    iot_client_t *iot = (iot_client_t *)arg;
    if (!iot) return;
    int failing = 0;              /* 连续失败中标记(成功清零),配合 fail_since_ms 计失败持续时长 */
    unsigned int fail_since_ms = 0;
    while (g_mqtt_ka_run) {
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
            while (g_mqtt_ka_run && iot_client_message_connect(iot) != 0) {
                printf("[TUYA] mqtt reconnect fail, retry in %ums\r\n", backoff);
                msleep(backoff);
                if (backoff < 60000) backoff *= 2;
            }
            if (g_mqtt_ka_run) printf("[TUYA] mqtt reconnected\r\n");
            failing = 0;
        }
        os_time_dly(1);
    }
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
#if TUYA_STM_MCP_VIA_TEXT
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
     * audio_start 处显式清零,新轮回话不受影响。*/
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
/* MCP 回应延迟发送(防死锁,与 stm 库日志环形缓冲同一套路)                      */
/* ------------------------------------------------------------------------- */
/* TAI_EVT_MCP_CMD 回调在 stm 引擎线程上下文执行(tstm_on_data_recv 直接派发)。
 * 若在回调里同步 tai_send_mcp_response → 从引擎线程再进库的发送路径,而 demo
 * 任务此刻可能正在发音频包:两条路径在库内同一会话锁上互等 → 双双永久卡死
 * (2026-08-31 第五轮实测:MCP initialize 恰在音频上行中到达,demo 任务从第 3
 * 帧起再无任何输出,无 exception,VAD 线程日志照常——轮 4 两次 MCP 都在空闲
 * 期到达所以没踩到)。对策:回调只把回应存进 pending,由 demo 任务在 20ms 级
 * 循环节拍处 mcp_resp_pump() 真正发送。TCP 模式回调同样不在 demo 任务,一并
 * 走延迟(云端对 ≤60ms 的回应延迟无感)。 */
static char s_mcp_resp[224];
static volatile unsigned s_mcp_resp_len;      /* 0=无待发 */
static volatile unsigned s_mcp_resp_drops;    /* 新回应覆盖未发走旧回应的次数 */

static void mcp_resp_defer(const char *resp, unsigned len)
{
    OS_ENTER_CRITICAL();
    if (s_mcp_resp_len) {
        s_mcp_resp_drops++;    /* 上一条还没发走就被覆盖:云端按序等回应,极少发生 */
    }
    if (len > sizeof(s_mcp_resp) - 1) {
        len = sizeof(s_mcp_resp) - 1;
    }
    memcpy(s_mcp_resp, resp, len);
    s_mcp_resp[len] = 0;
    s_mcp_resp_len = len;
    OS_EXIT_CRITICAL();
}

/* demo 任务循环节拍处调用(与 tai_log_flush 同批):把待发 MCP 回应真正发出 */
static void mcp_resp_pump(tai_ctx_t *ctx)
{
    char buf[sizeof(s_mcp_resp)];
    unsigned len, n, drops;
    int rc;

    if (!s_mcp_resp_len) {
        return;
    }
    OS_ENTER_CRITICAL();
    len = s_mcp_resp_len;
    n = (len < sizeof(buf)) ? len : sizeof(buf) - 1;
    memcpy(buf, s_mcp_resp, n);
    drops = s_mcp_resp_drops;
    s_mcp_resp_drops = 0;
    s_mcp_resp_len = 0;        /* 先取走:发送失败不重试,云端超时会重发请求 */
    OS_EXIT_CRITICAL();
    buf[n] = 0;
    rc = tai_send_mcp_response(ctx, buf);
#if TUYA_STM_MCP_VIA_TEXT
    if (rc == TAI_OK) {
        s_mcp_text_reply_pending = 1;
        s_mcp_text_break_request = 0;
        s_mcp_text_break_sent = 0;
        s_mcp_text_reply_deadline = timer_get_ms() + 5000;
    }
#endif
    printf("[TUYA-MCP] resp(%u B) rc=%d%s\r\n", len, rc,
           drops ? " (pending overwritten!)" : "");
}

#define TUYA_OPUS_FRAME_LEN 1280   /* PCM 16k/16bit/mono 40ms = 1280B/帧(原 opus 180B 改 PCM) */
static void opus_frame_stat(const unsigned char *p, int len,    /* 定义在后,先声明给 music_handoff 用 */
                            unsigned int *out_sum, unsigned int *out_act);

/* ---- barge-in 话音历史:播放循环滚动缓存最近 ~1.8s 已消费帧 ----
 * TTS/音乐播放期间循环必须持续读帧防 cbuf 溢出(只喂 KWS/丢弃),而能量
 * barge-in 确认只要 3 帧(~120ms)——用户从开口到确认之间的命令头部全丢在
 * 循环里。2026-09-05 实测:音乐中喊"给我放一首周杰伦的歌",云端只收到尾部
 * "伦的歌。"。barge-in 命中时把历史线性化进 g_barge_prebuf 作上行 prefill
 * 补发,云端才能听到完整命令。push 每帧 memmove 57KB@25Hz≈1.4MB/s,可承受;
 * 线性化只在 barge 命中(罕见)时执行一次。*/
#define BARGE_HIST_FRAMES   45    /* 45*40ms=1.8s 滚动窗口 */
#define BARGE_PREBUF_FRAMES 60    /* prebuf 容量:hist45+确认3+裕量 */
static unsigned char g_barge_prebuf[1280 * BARGE_PREBUF_FRAMES] __attribute__((aligned(4)));   /* 转 short* 进 opus 编码,align 1 全局无偶地址保证,同 abuf */
static unsigned int g_barge_prefill;
static unsigned char s_barge_hist[1280 * BARGE_HIST_FRAMES] __attribute__((aligned(4)));
static unsigned int  s_barge_hist_cnt;    /* 有效帧数(≤45, newest 在尾部) */

/* 播放循环每读一帧调一次:进历史环形窗(满则挤掉最旧) */
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

/* 历史整体(旧→新)作为上行 prefill,接在 prebuf 已有帧(确认帧)之后,复位历史。
 * 调用点:barge-in 确认命中后、停乐排空后——下一轮上行把它们补在最前。*/
static void barge_hist_to_prefill(void)
{
    if (s_barge_hist_cnt == 0) {
        return;
    }
    memmove(g_barge_prebuf + s_barge_hist_cnt * 1280, g_barge_prebuf,
            g_barge_prefill * 1280);
    memcpy(g_barge_prebuf, s_barge_hist, s_barge_hist_cnt * 1280);
    g_barge_prefill += s_barge_hist_cnt;
    s_barge_hist_cnt = 0;
    printf("[TUYA] barge history -> prefill %u frames\r\n", g_barge_prefill);
}

/* TTS barge-in 确认命中收尾:确认帧已在 barge_in_energy_confirmed 读帧时喂 KWS
 * +入历史(成功/失败都进,保语音流连续),这里只剩历史→prefill 的线性化。*/
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
/* ------------------------------------------------------------------------- */
/* ⑤ 音乐技能交接:本轮云端回了音乐 SKILL(试听 mp3 URL,解析见 tuya_music.c)。
 * TTS 排空把"正在为您播放…"播完 → 停我们的 TTS 解码器让出 DAC → 交给
 * app_music 网络解码(net_download,https 自动 TLS)→ 等整首播完
 * (dec_end 回调清 g_music_playing)→ 重启 TTS 播放器,回 ① 继续听音。
 * ★ 播放期间不上行(音乐回采不该进 ASR),但可 barge-in 停乐:照搬 TTS drain
 *   的"VAD+能量门"双确认(见下面等待循环)。AEC 对连续音乐的效果未验证,
 *   故开头 1s 不设防、每秒打一次 mic 能量基线([MUSIC-DBG]),实测误触发
 *   ("音乐自己把自己打断")就调 BARGE_MIN_ENERGY。
 *   唤醒词同样可停乐(KWS 全程在线,同一循环喂帧,先于 barge-in 判)。
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
             * _device_get_voice_data 内部按帧节拍(~40ms)阻塞,顺带排空 mic:
             * 音乐回采不积压,播完立刻干净听音(不会把音乐尾巴当新问题)。
             * barge-in(TUYA_BARGE_IN_ENABLE,判据照搬 barge_in_energy_confirmed):
             *   VAD 在线 + 3 帧连续(120ms) sum≥BARGE_MIN_ENERGY 才停乐,断一帧
             *   就重数——滤掉音乐瞬态拍子。若 AEC 把音乐消得够低,音乐回声到不了
             *   门槛,只有贴脸的人声能过;实测过不了关就调门槛。
             *   开头 25 帧(1s)不设防:避开 DAC 交接瞬态和曲首重拍。*/
            unsigned int mstart = timer_get_ms();
            s_barge_hist_cnt = 0;   /* 历史从本曲起算:打断补发只含音乐期间的话音 */
            int mbarge = 0, mwake = 0;           /* 停乐原因: barge-in 抢答打断 / 唤醒词打断 */
#ifdef TUYA_BARGE_IN_ENABLE
            unsigned int mframes = 0, mhi = 0;   /* 帧计数 / 连续达标帧数 */
            unsigned int msums[3] = {0, 0, 0};
#endif
            while (!g_exit && !g_link_broken && g_music_playing &&
                   app_music_tuya_music_busy() &&
                   timer_get_ms() - mstart < 600000) {
                unsigned char _mt[TUYA_OPUS_FRAME_LEN];
                if (_device_get_voice_data(_mt, sizeof(_mt)) != sizeof(_mt)) {
                    continue;               /* 读不够一帧:等下一拍再来 */
                }
                tai_log_flush();            /* 音乐长循环(可达10min)也保持库日志节拍 */
                mcp_resp_pump(ctx);
                barge_hist_push(_mt);       /* 音乐期帧进 barge 历史:打断时补发命令头部 */
#ifdef TUYA_KWS_ENABLE
                /* KWS 全程在线:音乐播放期也喂唤醒词(帧已到手不浪费)。命中即停乐
                 * (TuyaOpen wakeup 回调的 player_stop 语义),提示音等恢复 TTS 播放器
                 * 后再播(on_wake 已置 pending,见循环后)。*/
                tuya_kws_feed(_mt, sizeof(_mt));
                if (g_wake_hit) {
                    g_wake_hit = 0;
                    printf("[TUYA-MUSIC] wake stops music\r\n");
                    mwake = 1;
                    break;
                }
#endif
#ifdef TUYA_BARGE_IN_ENABLE
                mframes++;
                unsigned int mes, mea;
                opus_frame_stat(_mt, sizeof(_mt), &mes, &mea);
                if ((mframes % 25) == 0) {  /* 每 ~1s 打能量基线:调门槛看这个 */
                    printf("[MUSIC-DBG] playing, mic post-AEC: sum=%u act=%u%% rec=%d\r\n",
                           mes, mea, get_recoder_state());
                }
                if (mframes > 25 && get_recoder_state() && mes >= BARGE_CONFIRM_ENERGY) {
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
            if (g_music_playing) {           /* 超时/失败/打断/退出:强停,防 DAC 被占死 */
                printf("[TUYA-MUSIC] stop (%s)\r\n",
#ifdef TUYA_BARGE_IN_ENABLE
                       mbarge ? "barge-in" :
#endif
                       mwake ? "wake" :
                       app_music_tuya_music_busy() ? "timeout/exit" : "download/decode fail");
                app_music_tuya_music_stop();
                g_music_playing = 0;
            }
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
#ifdef TUYA_KWS_ENABLE
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
                _device_net_audio_play(1);   /* 正常播完/下载失败(无提示音、无抢答):
                                              * 当场恢复播放器,原行为 */
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
    tai_chat_break(ctx);              /* 通知云端中止本轮 TTS */
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
        /* 旧轮残留 END 判弃:barge-in 打断的上一轮,云端流(文本/TTS)可能拖到
         * 新轮 ④ 等待期才收尾(实测晚 ~0.2s)。把它当本轮结束会提前骗退等待,
         * 本轮晚到的音乐 SKILL 等回复无人消费 → "第一次放歌没反应,第二次才播"。
         * END 的 event_id 命中"起轮时快照的旧轮 id" → 判为旧轮残留,只记日志。*/
        if (msg->event_id && msg->event_id[0] && g_stale_end_bizid[0] &&
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
    } else if (msg->event_type == TAI_EVT_SERVER_VAD) {
        /* 云端 VAD 检测到用户停说(endpointing)。TUYA_SERVER_VAD_ENABLE 模式下,
         * 置位标志让上行循环退出收尾(发 audio_end → 等回复)。开口仍由本地VAD负责。*/
        printf("[TUYA-AI] server-vad (end of speech)\r\n");
#ifdef TUYA_SERVER_VAD_ENABLE
        g_server_vad_stop = 1;
#endif
    } else if (msg->event_type == TAI_EVT_CHAT_BREAK) {
        /* chat_break 有两种来源:
         * 1) 本地 barge-in 的 tai_chat_break 回执(打断 TTS)
         * 2) 云端 VAD 检测到说话结束(云端自动下发的打断标识)
         * 云端确认:开了 asr.enableVad 后,云端判停说只发 chat_break,不发 server-vad。
         * 所以在 TUYA_SERVER_VAD_ENABLE + 正在上行(说话中) 时,chat_break = 云端判停说。*/
        printf("[TUYA-AI] chat_break\r\n");
        g_tts_playing = 0;
#ifdef TUYA_SERVER_VAD_ENABLE
        g_server_vad_stop = 1;   /* 上行中收到 = 云端VAD判停说; TTS中收到 = barge-in回执(两者都OK) */
#endif
    } else if (msg->event_type == TAI_EVT_MCP_CMD) {
        /* MCP 命令:云端智能体识别意图后下发(如自定义DP"运动一下")。
         * 数据在 msg->data(JSON-RPC 格式),含 method/params 等。
         * 这里打印完整内容,看云端有没有下发 DP 及其值。*/
        printf("[TUYA-MCP] recv len=%u: %.*s\r\n",
               (unsigned)msg->len, (int)(msg->len < 512 ? msg->len : 512),
               msg->data ? (const char *)msg->data : "(null)");
        /* 回应必须【原样回带请求的 id】(云端按 id 匹配响应;原实现硬编码
         * id=1,云端 initialize 的 id 是字符串时间戳,回 id=1 会被当无关包丢弃)。
         * 载荷可能不带 NUL 结尾,先拷进局部缓冲再解析。*/
        char pbuf[256];
        unsigned pl = (msg->data && msg->len) ? msg->len : 0;
        if (pl > sizeof(pbuf) - 1) pl = sizeof(pbuf) - 1;
        memcpy(pbuf, msg->data ? msg->data : "", pl);
        pbuf[pl] = 0;
        char rid[36] = "1";   /* 找不到 id 时兜底用 1 */
        const char *idk = strstr(pbuf, "\"id\"");
        if (idk) {
            const char *p = idk + 4;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ':') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                const char *e = p;
                if (*e == '"') {            /* 字符串 id:含引号整体回带 */
                    for (e++; *e && *e != '"' && e - p < 30; e++) {}
                    if (*e == '"') e++;
                } else {                    /* 数字 id */
                    while (*e >= '0' && *e <= '9' && e - p < 30) e++;
                }
                if (e > p && (size_t)(e - p) < sizeof(rid)) {
                    memcpy(rid, p, e - p);
                    rid[e - p] = 0;
                }
            }
        }
        char resp[192];
        int rn = snprintf(resp, sizeof(resp),
                          "{\"jsonrpc\":\"2.0\",\"id\":%s,"
                          "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"\"}]}}",
                          rid);
        if (rn > 0 && rn < (int)sizeof(resp)) {
            /* ★不能在这里直接 tai_send_mcp_response:本回调跑在引擎线程,
             * 同步发包会与 demo 任务的音频上行在库内会话锁上互等卡死
             * (见上方 mcp 回应延迟发送注释)。只存,demo 任务节拍处发。*/
            mcp_resp_defer(resp, (unsigned)rn);
            printf("[TUYA-MCP] resp(id=%s) deferred\r\n", rid);
        }
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
    strncpy((char *)obcfg.uuid,        TUYA_UUID,        sizeof(obcfg.uuid) - 1);
    strncpy((char *)obcfg.authkey,     TUYA_AUTH_KEY,    sizeof(obcfg.authkey) - 1);
    strncpy((char *)obcfg.product_key, TUYA_PRODUCT_KEY, sizeof(obcfg.product_key) - 1);
    obcfg.env              = PROD;
    obcfg.mqtt_disable_tls = false;
    obcfg.mqtt_auto_connect = 1;
    obcfg.timeout_ms       = 30000;
    obcfg.cert_bundle_attach = NULL;
    obcfg.cacert = NULL;
    iot = iot_client_init_on_boarding_with_token(&obcfg, TUYA_ACTIVATION_TOKEN);
    if (!iot) { printf("[TUYA] on_boarding_with_token fail(token 过期/三件套错?)\r\n"); return; }
    printf("[TUYA] activated, devid=%s\r\n", iot->devid);
#else
    /* 直连:已预注册设备,直接用三元组 */
    iot_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.region = AY; cfg.env = PROD;
    cfg.mqtt_disable_tls = false; cfg.mqtt_auto_connect = 1;
    cfg.cert_bundle_attach = NULL; cfg.cacert = NULL;
    cfg.message_callback = on_mqtt_message;   /* MQTT 常驻:收 DP 下行 */
    extern const char *tuya_get_effective_sw_ver(void);
    cfg.sw_ver = tuya_get_effective_sw_ver();   /* 上报生效版本:VM>源码基线 */

    strncpy((char *)cfg.devid,      TUYA_DEVID,      sizeof(cfg.devid) - 1);
    strncpy((char *)cfg.secret_key, TUYA_SECRET_KEY, sizeof(cfg.secret_key) - 1);
    strncpy((char *)cfg.local_key,  TUYA_LOCAL_KEY,  sizeof(cfg.local_key) - 1);
    iot = iot_client_init(&cfg);
    if (!iot) { printf("[TUYA] iot_client_init fail\r\n"); return; }
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
    static const char SESSION_ATTRS[] = "{\"deviceMcp\":{\"supportCustomMCP\":true}}";
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
        tai_chat_break(ctx);          /* 通知云端中止本轮 TTS */
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
        tai_chat_break(ctx);
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
    /* 本 demo 没有设备侧自定义 MCP 工具，必须显式 false。NULL 也不行：协议层默认
     * 会补 true，云端随后下发 initialize；若它恰在 AUDIO 上行中到达，TEXT 承载的
     * response 会与语音事件重叠并阻塞 STM 任务，表现为只发出前几帧后彻底无响应。 */
    static const char SA[] =
        "{\"deviceMcp\":{\"supportCustomMCP\":false},"
        "\"tts.order.supports\":[{\"format\":\"opus\",\"sampleRate\":16000,"
        "\"bitDepth\":\"16\",\"channels\":1}]}";
#else
    static const char SA[] = "{\"deviceMcp\":{\"supportCustomMCP\":false}}";
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
    g_audio_frame_logged = 0;
#ifdef TUYA_MUSIC_ENABLE
    g_music_playing      = 0;
    tuya_music_reset();                /* 清上一会话残留的半截文本流/未消费的音乐结果 */
#endif
#ifdef TUYA_UPLINK_OPUS_ENABLE
    /* 上行 opus 编码器:幂等 init(编码器常驻堆,跨会话/重连复用);失败自动回退
     * PCM 上行(仍能对话,只是带宽大),不废会话。 */
    int use_opus_uplink = (tuya_opus_enc_init() == 0);
    if (!use_opus_uplink) {
        printf("[TUYA] opus enc init fail -> uplink fallback PCM\r\n");
    }
#else
    const int use_opus_uplink = 0;
#endif
    printf("[TUYA] voice loop ready (local-VAD, uplink=%s 16k/mono)"
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
    unsigned int start_fails = 0;      /* audio_start 连续失败计数:≥5(≈1s)判链路死 */
    unsigned int idle_cnt = 0, idle_sum = 0, idle_act_sum = 0;
#ifdef TUYA_SERVER_VAD_ENABLE
    /* STM 当前不能稳定区分 server-VAD 指令。历史成功轮的 ASR 是流式返回，
     * 无需等待云端 stop；本地 VAD 已做 600ms debounce，直接 fin 可避免事件
     * 长时间不闭合及用户再次说话把2秒静音计数清零。 */
    #define LOCAL_SILENCE_TIMEOUT_FRAMES  1
    unsigned int silence_frames = 0;
#endif

    while (!g_exit) {
        /* ① 等待:未在播放 TTS 且本地 VAD 检测到开口。
         *   空闲时【持续排空】录音 cbuf(_device_get_voice_data 内部 mdelay(60) 按帧节拍),
         *   保证 VAD 触发时缓冲里没有积压旧音频——上行直接发"此刻"的实时语音。
         *   ⚠️ 绝不能在 VAD 触发时再 clear():那会把刚触发到的那句语音一并清掉,
         *      之前正是这样导致云端 ASR 收到静音、回空文本。*/
        int mcp_text_poll = 0;
        g_wake_break = 0;   /* 唤醒打断标记只在本轮迭代内生效(跳过 ④ 收尾/pending 音乐) */
#if TUYA_STM_MCP_VIA_TEXT
        /* MCP response 经 TEXT 发出后，必须先等伪回复并 chat_break，再允许 AUDIO。
         * 否则用户恰好开口会让两个事件重叠，复现“只回空内容、语音无 ASR”。 */
        if (s_mcp_text_reply_pending) {
            if ((s_mcp_text_break_request ||
                 (int)(timer_get_ms() - s_mcp_text_reply_deadline) >= 0) &&
                !s_mcp_text_break_sent) {
                int break_rc = tai_chat_break(ctx);
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
        /* ★ 播放缓冲非空(孤儿 TTS:旧轮 END 把等待骗退/兜底恢复后音频晚到)时不起轮:
         * 喇叭还在播,AEC 劣化态回声就能把 VAD 顶开→幽灵上行轮把故事回声发给云端
         * (2026-09-06 实测 [TTS!] 标记)。唤醒词不受影响(idle 排空照喂引擎),缓冲
         * 排空即恢复起轮;带 g_barge_in 的抢答轮必已清 rbuf,不会被此门误拦。*/
        int start_turn = mcp_text_poll ? 0 :
                         (get_recoder_state() && _device_get_play_level() < 640);
        /* 普通轮 turn-start 能量门:无唤醒词+单麦开麦,任何持续声响都触发 VAD→设备自言自语。
         * VAD 触发后再核 1 帧能量(近场话音够响),达标才起轮并把这帧作 onset 补发;否则当噪音丢弃。
         * barge-in 轮(g_barge_in)已在 ④/drain 做过能量确认,这里跳过直接起轮。*/
        if (start_turn && !g_barge_in) {
            unsigned char _p[TUYA_OPUS_FRAME_LEN];
            unsigned int _s, _a;
            if (_device_get_voice_data(_p, sizeof(_p)) == TUYA_OPUS_FRAME_LEN) {
                opus_frame_stat(_p, TUYA_OPUS_FRAME_LEN, &_s, &_a);
                if (_s >= BARGE_MIN_ENERGY) {
                    memcpy(g_barge_prebuf, _p, TUYA_OPUS_FRAME_LEN);
                    g_barge_prefill = 1;
#ifdef TUYA_KWS_ENABLE
                    tuya_kws_feed(_p, TUYA_OPUS_FRAME_LEN);   /* onset 帧在进 prebuf 后
                                            不再经过任何喂音点,这里补上保 KWS 流连续 */
#endif
                } else {
                    start_turn = 0;   /* 噪音/远场,拒起轮(治自言自语) */
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
        if (start_turn && !g_barge_in &&
            (g_wake_swallow || !tuya_kws_awake())) start_turn = 0;
        if (start_turn) tuya_kws_window_kick();
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
            unsigned char _trash[TUYA_OPUS_FRAME_LEN];
            int tn = _device_get_voice_data(_trash, sizeof(_trash));   /* 丢弃,保持缓冲新鲜 */
            /* MQTT 心跳由独立线程维持(见 tuya_ai_run 入口的 tuya_mqtt_keepalive_task),
             * 不在这里调 iot_client_process——它内部的 TLS recv 会阻塞语音循环线程。*/
            if (tn == TUYA_OPUS_FRAME_LEN) {   /* 攒够整帧才统计,得到稳定的静音底噪基线 */
                unsigned int s, a;
                opus_frame_stat(_trash, TUYA_OPUS_FRAME_LEN, &s, &a);
                idle_cnt++; idle_sum += s; idle_act_sum += a;
                if (idle_cnt >= 16) {   /* 16帧≈1s,汇总打印一次 */
                    printf("[TUYA] idle drain: %u frames, avg_sum=%u avg_act=%u%%\r\n",
                           idle_cnt, idle_sum / idle_cnt, idle_act_sum / idle_cnt);
                    idle_cnt = 0; idle_sum = 0; idle_act_sum = 0;
                }
#ifdef TUYA_KWS_ENABLE
                tuya_kws_feed(_trash, tn);   /* 排空的 40ms 帧喂唤醒词引擎 */
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
            if (g_barge_prefill == 1) {
                printf("[TUYA] turn-start: prepended 1 onset frame\r\n");
            } else {
                printf("[TUYA] barge-in: prepended %u confirm frames\r\n", g_barge_prefill);
            }
            g_barge_prefill = 0;
        }
#endif

        /* 回复开始后不要立即截断上行：本轮若尚无实时帧，g_tts_playing 很可能仍是
         * 上一轮/启动 MCP 污染回复的尾部状态。至少发出 3 帧实时音频后才允许按新 TTS
         * 收尾，避免出现“audio start + prepended 1 frame，但实时上行 0 帧”的空轮。*/
        while (!g_exit) {
            if (g_tts_playing && uplink_frames >= 3 && barge_cooldown_expired()) {
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
                    tai_chat_break(ctx);   /* 词前半已上行:作废本轮,云端不再回话 */
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
            /* 云端VAD模式:停说由云端 TAI_EVT_SERVER_VAD 决定(更准),本地只做超时兜底。
             * 开口仍由本地VAD负责(循环顶部 get_recoder_state),这里只切换"停说"判定。*/
            if (g_server_vad_stop) {
                printf("[TUYA] speak-stop: server-vad (frames=%u active=%u)\r\n",
                       uplink_frames, uplink_active);
                break;
            }
            /* STM 下行无法可靠区分 SERVER_VAD；本地 VAD 已经过去抖，判停后立即
             * 结束事件。仍优先接受可识别的 chat_break/server-vad 标志。 */
            if (!get_recoder_state()) {
                if (++silence_frames >= LOCAL_SILENCE_TIMEOUT_FRAMES) {
                    printf("[TUYA] speak-stop: local-vad (frames=%u active=%u)\r\n",
                           uplink_frames, uplink_active);
                    break;
                }
            } else {
                silence_frames = 0;
            }
#else
            /* 本地VAD模式:本地VAD直接判停说(原逻辑) */
            if (!get_recoder_state()) {     /* enc VAD 已 debounce 判定停说(stop 阈值) */
                printf("[TUYA] speak-stop: uplink end (frames=%u active=%u)\r\n",
                       uplink_frames, uplink_active);
                break;
            }
#endif
        }
        {
            int end_rc = tai_send_audio_end(ctx);
            printf("[TUYA] audio-end rc=%d frames=%u active=%u\r\n",
                   end_rc, uplink_frames, uplink_active);
            if (end_rc != TAI_OK) {
                g_link_broken = 1;
            }
        }

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
                    tai_chat_break(ctx);          /* 通知云端中止本轮 TTS */
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
                        tai_chat_break(ctx);
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
                        tai_chat_break(ctx);
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
            if (!g_wake_break) {   /* 唤醒打断:词尾由吞咽窗在 idle 排,这里不再白排 300ms */
                unsigned char _trash[TUYA_OPUS_FRAME_LEN];
                for (int i = 0; i < 8 && !g_exit; i++) {  /* 8 × ~40ms ≈ 300ms 排空 mic */
                    _device_get_voice_data(_trash, sizeof(_trash));
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
        if (g_link_broken) break;   /* 链路断:退出语音循环,本会话收尾交 supervisor 重连 */
    }

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
static void tuya_ai_run(const pal_t *pal, iot_client_t *iot, const char *local_key)
{
    g_mqtt_ka_run = 1;
    thread_fork("tuya_mqtt_ka", 5, 6 * 1024, 0, 0, tuya_mqtt_keepalive_task, iot);

    audio_stream_init(16000, 16, 1);
    start_audio_stream();

#ifdef TUYA_KWS_ENABLE
    tuya_kws_init();   /* 唤醒词引擎(幂等,只初始化一次;失败自动回退常听) */
#endif

    unsigned int backoff_ms = 5000;
    while (1) {
        /* 每次会话尝试前复位跨线程标志(tai ctx 尚未创建,on_disconnect 无竞态) */
        g_exit = 0; g_link_broken = 0;
        g_tts_playing = 0; g_turn_done = 1;
#ifdef TUYA_BARGE_IN_ENABLE
        g_barge_in = 0; g_barge_prefill = 0;
#endif
        printf("[TUYA] session attempt\r\n");
        unsigned int lived_ms = tuya_ai_session(pal, iot, local_key);
        if (lived_ms > 60000) backoff_ms = 5000;   /* 会话曾健康存活,按首次失败退避 */
        /* 会话报废收尾:清残留 TTS 让喇叭立刻安静,重连后从干净状态起听 */
        _device_rbuf_clear();
        printf("[TUYA] session lost (lived %us), retry in %ums\r\n",
               lived_ms / 1000, backoff_ms);
        msleep(backoff_ms);
        if (backoff_ms < 60000) backoff_ms *= 2;
    }
}

/* 配网等待期循环播报"请配置网络",每 30s 一次,避免用户以为设备死机(tuya/小智的做法)。
 * tuya_ble_netcfg_start 阻塞,故用独立线程周期播报;配网完成/失败/超时置
 * s_prov_prompt_run=0,线程在 ~0.1s 内退出。NetCfgEnter.mp3 是 app_music 现有提示音。*/
static volatile int s_prov_prompt_run;

/* 首次配网从 BLE 收到 WiFi 信息后，最多用 30s 完成 STA 关联、DHCP 以及
 * CONFIG_ASSIGN_MACADDR_ENABLE 下的新板 MAC 分配/重连。总预算从发起本次
 * wifi_enter_sta_mode 起计算，避免 STA 20s + DHCP 30s 叠加拖过 App/token 时限。*/
#define TUYA_NETWORK_READY_TIMEOUT_MS  30000u
#define TUYA_NETWORK_READY_POLL_MS       100u

static volatile int s_tuya_provisioning_active;

int tuya_agentic_provisioning_active(void)
{
    return s_tuya_provisioning_active;
}

extern unsigned int wifi_get_tuya_network_ready_generation(void);
extern void app_music_play_tuya_netcfg_result(int success);
void tuya_clear_provision_and_reset(void);

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

static int tuya_wait_for_network_ready(unsigned int start_generation)
{
    unsigned int waited_ms = 0;
    int sta_seen = 0;

    while (waited_ms < TUYA_NETWORK_READY_TIMEOUT_MS) {
        int sta_connected = (wifi_get_sta_connect_state() == WIFI_STA_CONNECT_SUCC);
        unsigned int generation = wifi_get_tuya_network_ready_generation();

        if (sta_connected && !sta_seen) {
            sta_seen = 1;
            printf("[TUYA] wifi STA connected, wait network ready...\r\n");
        }
        if (generation != start_generation && sta_connected) {
            printf("[TUYA] network ready: generation %u -> %u, waited=%ums\r\n",
                   start_generation, generation, waited_ms);
            return 0;
        }

        msleep(TUYA_NETWORK_READY_POLL_MS);
        waited_ms += TUYA_NETWORK_READY_POLL_MS;
    }

    printf("[TUYA] network ready timeout after %ums (sta_seen=%d, generation=%u)\r\n",
           waited_ms, sta_seen, wifi_get_tuya_network_ready_generation());
    return -1;
}

static int tuya_write_vm_exact(u16 index, const void *data, u16 len, const char *name)
{
    int ret = syscfg_write(index, (void *)data, len);
    if (ret != len) {
        printf("[TUYA] VM write failed: %s ret=%d expected=%u\r\n",
               name, ret, (unsigned int)len);
        return -1;
    }
    return 0;
}

static int tuya_save_required_provision_data(iot_client_t *iot)
{
    uint8_t region_byte = (uint8_t)iot->region;
    int failed = 0;

    failed |= tuya_write_vm_exact(VM_TUYA_DEVID_IDX, iot->devid, 32, "devid");
    failed |= tuya_write_vm_exact(VM_TUYA_SECRET_IDX, iot->secret_key, 32, "secret_key");
    failed |= tuya_write_vm_exact(VM_TUYA_LOCALKEY_IDX, iot->local_key, 32, "local_key");
    failed |= tuya_write_vm_exact(VM_TUYA_SSID_IDX, s_main_creds.ssid, 65, "ssid");
    failed |= tuya_write_vm_exact(VM_TUYA_PWD_IDX, s_main_creds.password, 65, "password");
    failed |= tuya_write_vm_exact(VM_TUYA_REGION_IDX, &region_byte, 1, "region");

    if (!failed) {
        printf("[TUYA] required provisioning data saved, region=%s\r\n",
               region_name(iot->region));
    }
    return failed ? -1 : 0;
}

static void tuya_provisioning_fail_and_reset(const char *stage, iot_client_t *iot)
{
    printf("[TUYA] provisioning failed at %s, reset to BLE provisioning\r\n", stage);
    if (iot) {
        iot_client_deinit(iot);
    }
    app_music_play_tuya_netcfg_result(0);
    /* 保持 s_tuya_provisioning_active=1 直到复位，避免失败音后的网络事件误播成功音。*/
    tuya_clear_provision_and_reset();
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
/*   参考 xiaozhi-esp32 的 iot_log_cb 实现(tuya_protocol.cc:102)。           */
/* ========================================================================= */
#include "tai_log.h"   /* log_set_handler / LOG_* 枚举 */

/* 日志级别控制:改这个值即可控制 SDK 日志输出量。
 * LOG_ERROR=1(只看错误) / LOG_WARN=2(+警告) / LOG_INFO=3(+信息) / LOG_DEBUG=4(全开)
 * 调试云端VAD等问题时设 LOG_DEBUG;平时设 LOG_WARN 减少刷屏。*/
/* 日志级别:LOG_INFO(3) 能看到激活/连接/ASR等关键流程,又避开 LOG_DEBUG 里的
 * %llu(64位格式符,杰理 newlib 可能不支持)包日志路径。调云端VAD等问题时够用。
 * 如需更详细日志改 LOG_DEBUG(4),但注意 tai_pkt_log.c 有 %llu 可能崩溃。*/
#define TUYA_SDK_LOG_LEVEL  LOG_INFO

static OS_MUTEX tuya_log_mutex;   /* 防多线程并发输出交叉 */
static int tuya_log_mutex_inited;

static void tuya_log_redirect(log_level_t level, const char *fmt, va_list args)
{
    static const char level_char[] = {'-', 'E', 'W', 'I', 'D'};
    char lc = (level >= 0 && level <= 4) ? level_char[level] : '?';

    /* 格式化到本地 buffer(vsnprintf 安全:超长截断不溢出)。
     * SDK 单条日志通常 < 200 字节,256 够用。*/
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt ? fmt : "", args);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;   /* 截断 */

    /* 互斥锁保护:printf 不是线程安全的,多线程并发会交叉输出。
     * log_emit 在 worker 线程(回调)和 agentic 线程都可能触发。*/
    if (tuya_log_mutex_inited) os_mutex_pend(&tuya_log_mutex, 0);
    printf("[TUYA-SDK/%c] %s\r\n", lc, buf);
    if (tuya_log_mutex_inited) os_mutex_post(&tuya_log_mutex);
}

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

    if (iot_init(pal) != 0) { printf("[TUYA] iot_init fail\r\n"); return; }

    /* 注册自定义日志 handler:把 SDK 日志从 fprintf(stderr)(会崩溃)重定向到 printf(UART)。
     * 原来用 log_set_level(0) 关掉所有日志规避崩溃,但导致云端VAD配置响应等无法调试。
     * 现在用 tuya_log_redirect 替代,既能看日志又不会崩溃。*/
    os_mutex_create(&tuya_log_mutex);
    tuya_log_mutex_inited = 1;
    log_set_handler(tuya_log_redirect);
    log_set_level(TUYA_SDK_LOG_LEVEL);   /* 见上方宏,改它即可控制日志量 */

    char devid[32] = {0}, secret[32] = {0}, localkey[32] = {0};
    /* syscfg_read 成功返回字节数(>0),失败返回负数;之前误写成 ==0,导致每次开机都判
     * "没有 devid"而重新配网(三元组其实在 flash 里)。参考 stream_protocol.c:157 的用法。*/
    int have = (syscfg_read(VM_TUYA_DEVID_IDX, devid, sizeof(devid)) > 0 && devid[0] != 0 && devid[0] != 0xFF);
    if (have) {
        syscfg_read(VM_TUYA_SECRET_IDX, secret, sizeof(secret));
        syscfg_read(VM_TUYA_LOCALKEY_IDX, localkey, sizeof(localkey));
        printf("[TUYA] already provisioned, devid=%s\r\n", devid);

        /* 直连路径必须自己重连 WiFi:开机 wifi 子系统落回 SMP_CFG_MODE 监听(没联网),
         * 直接 iot_client_init 连 AI 云必失败→"说话没反应"。读配网时一并存的 ssid/password,
         * 复用首次配网那套 wifi_enter_sta_mode + 轮询 SUCC + 等 DHCP(见下方首次配网段)。*/
        char ssid[65] = {0}, pwd[65] = {0};
        syscfg_read(VM_TUYA_SSID_IDX, ssid, sizeof(ssid) - 1);
        syscfg_read(VM_TUYA_PWD_IDX,  pwd,  sizeof(pwd) - 1);
        if (ssid[0] != 0 && ssid[0] != 0xFF) {
            /* wifi_app_task 开机已自动连 VM 里的 SSID 并播报过提示音。
             * 不再重复重连(wifi_get_sta_connect_state 在 wifi_app_task 被杀后返回值不可靠,
             * 且 wifi_enter_sta_mode 会断开再重连,导致第二次提示音)。直接用现有连接即可。*/
            printf("[TUYA] wifi already connected by wifi_app_task (ssid_len=%u)\r\n",
                   (unsigned int)strlen(ssid));
            /* 同步到杰理 wifi 存储:防 app_music 的 wifi_return_sta_mode 读到旧 ssid 覆盖 */
            tuya_sync_wifi_to_jl(ssid, pwd);
        } else {
            /* devid 在但 ssid 空:本修复前烧的设备没存 ssid → 没法联网,直连 AI 必失败。
             * 提示一下,重配一次网即补上 ssid。*/
            printf("[TUYA] WARN: devid present but no stored wifi ssid -> AI will fail, re-provision once\r\n");
        }

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
        cfg.mqtt_disable_tls = false; cfg.mqtt_auto_connect = 1;
        cfg.cert_bundle_attach = NULL; cfg.cacert = NULL;
        extern const char *tuya_get_effective_sw_ver(void);
        cfg.sw_ver = tuya_get_effective_sw_ver();   /* 上报生效版本:VM>源码基线 */

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
    s_tuya_provisioning_active = 1;
    printf("[TUYA] no devid, start BLE provisioning...\r\n");
    s_prov_prompt_run = 1;   /* 启动"请配置网络"循环播报(每 30s),配网完成会停 */
    thread_fork("tuya_prov_prompt", 6, 4 * 1024, 0, 0, tuya_prov_prompt_task, NULL);
    int prov_ret = tuya_ble_netcfg_start("TUYA", TUYA_PRODUCT_KEY, TUYA_UUID, TUYA_AUTH_KEY, main_prov_cb);
    s_prov_prompt_run = 0;   /* 配网完成/失败/超时,停循环播报 */
    os_time_dly(15);         /* ~150ms:让 prompt 线程看到标志退出,别让它播报到连 WiFi/激活阶段 */
    if (prov_ret != 0) {
        printf("[TUYA] BLE provisioning failed/timeout\r\n");
        tuya_provisioning_fail_and_reset("ble", NULL);
        return;
    }
    tuya_ble_netcfg_stop();   /* 配网完停 BLE,释放内存给 WiFi/TLS */
    printf("[TUYA] BLE done: ssid_len=%u token_len=%u\r\n",
           (unsigned int)strlen(s_main_creds.ssid),
           (unsigned int)strlen(s_main_creds.token));

    /* ---- 连 WiFi(配网给的 ssid/密码)---- */
    unsigned int network_generation = wifi_get_tuya_network_ready_generation();
    wifi_enter_sta_mode(s_main_creds.ssid, s_main_creds.password);
    if (tuya_wait_for_network_ready(network_generation) != 0) {
        tuya_provisioning_fail_and_reset("network-ready", NULL);
        return;
    }

    /* 同步到杰理 wifi 存储:防 app_music 的 wifi_return_sta_mode 读到旧 ssid 覆盖。
     * 之前换网络后,杰理 VM 里残留旧 ssid(GJ1)覆盖了涂鸦配的 ssid,导致断网连不上 AI。*/
    tuya_sync_wifi_to_jl(s_main_creds.ssid, s_main_creds.password);

    /* ---- on_boarding 激活 ---- */
    iot_on_boarding_config_t ob;
    memset(&ob, 0, sizeof(ob));
    strncpy((char *)ob.uuid,        TUYA_UUID,        sizeof(ob.uuid) - 1);
    strncpy((char *)ob.authkey,     TUYA_AUTH_KEY,    sizeof(ob.authkey) - 1);
    strncpy((char *)ob.product_key, TUYA_PRODUCT_KEY, sizeof(ob.product_key) - 1);
    ob.env = PROD; ob.mqtt_disable_tls = false; ob.mqtt_auto_connect = 1; ob.timeout_ms = 30000;
    ob.cert_bundle_attach = NULL; ob.cacert = NULL;
    extern const char *tuya_get_effective_sw_ver(void);
    ob.sw_ver = tuya_get_effective_sw_ver();   /* 上报生效版本:VM>源码基线 */
    iot_client_t *iot = iot_client_init_on_boarding_with_token(&ob, s_main_creds.token);
    if (!iot) {
        printf("[TUYA] on_boarding_with_token fail\r\n");
        tuya_provisioning_fail_and_reset("activation", NULL);
        return;
    }
    if (!iot->devid[0] || !iot->secret_key[0] || !iot->local_key[0] || !iot->mqtt) {
        printf("[TUYA] activation missing required credentials or MQTT connection\r\n");
        tuya_provisioning_fail_and_reset("activation-result", iot);
        return;
    }
    printf("[TUYA] activated, devid=%s\r\n", iot->devid);

    /* ---- 持久化三元组 + WiFi 凭据(下次开机直连)----
     * 三元组连涂鸦 AI 云;ssid/password 供直连路径开机重连 WiFi(否则开机离线,连 AI 必失败)。*/
    char lk[32] = {0};
    strncpy(lk, (const char *)iot->local_key, sizeof(lk) - 1);
    if (tuya_save_required_provision_data(iot) != 0) {
        tuya_provisioning_fail_and_reset("credential-persist", iot);
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
    app_music_play_tuya_netcfg_result(1);
    s_tuya_provisioning_active = 0;

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

/* 长按 KEY_PHOTO(K6) 时由 app_music 调用:清除已配网的三元组 + WiFi 凭据并软复位,
 * 复位后 tuya_agentic_main 读不到 devid → 自动重新进入 BLE 配网。
 * 给用户一个"主动重新配网"的入口(否则开机直连,没法重配)。
 * 板上 Reset/Update 是硬件键(固件读不到),电源键是自锁拨动开关(按不出长按),
 * 故重置入口放在 K6 长按(原本是空槽位)。*/
void tuya_clear_provision_and_reset(void)
{
    char zero[65] = {0};   /* 65 覆盖 ssid/pwd(65),也够 devid/secret/localkey(32)*/
    printf("[TUYA] >>> clear provision & reboot (re-enter BLE provisioning) <<<\r\n");
    syscfg_write(VM_TUYA_DEVID_IDX,    zero, 32);
    syscfg_write(VM_TUYA_SECRET_IDX,   zero, 32);
    syscfg_write(VM_TUYA_LOCALKEY_IDX, zero, 32);
    syscfg_write(VM_TUYA_SSID_IDX,     zero, 65);
    syscfg_write(VM_TUYA_PWD_IDX,      zero, 65);
    syscfg_write(VM_TUYA_REGION_IDX,   zero, 1);   /* region 一并清:重配网时重新下发(可能换了区)*/
    /* 同时清杰理 WiFi VM:设回 SMP_CFG_MODE(配网模式)+ 空 ssid。
     * 否则 wifi_app_task 开机读到旧 STA_MODE ssid 自动连网→播"网络连接成功"→
     * 然后才进配网,用户听到两条提示音("网络连接成功"+"请配置网络"),迷惑。*/
    wifi_store_mode_info(SMP_CFG_MODE, zero, zero);
    /* ★复位前先停 BT 广播,让蓝牙控制器进入 idle 再软复位。
     *   软复位(P33_SYSTEM_RESET)不像掉电/reset 键那样完全重置 BT 控制器,带活跃
     *   射频状态复位会导致重启后 BLE 链路异常(conn nack → supervision timeout),
     *   配网必失败(实测:长按 K6 走软复位后配网失败,按 reset 键冷启动则成功)。*/
    tuya_ble_netcfg_stop();
    os_time_dly(300);   /* 3s:BT 控制器 idle + VM 落盘 */
    extern void cpu_reset(void);
    cpu_reset();
    while (1) { ; }   /* 复位路径,不返回 */
}

/* boot 自启动:系统初始化后 fork tuya_agentic_main
 * ⚠️ 前提:BLE 控制器、WiFi 子系统此时已就绪;若 BLE 起不来,把该 initcall
 *    挪到 WiFi/BT 初始化完成之后,或由按键/事件触发。*/
static int tuya_agentic_main_init(void)
{
    return thread_fork("tuya_agentic", 4, 16 * 1024, 0, 0, tuya_agentic_main, NULL);
}
late_initcall(tuya_agentic_main_init);
