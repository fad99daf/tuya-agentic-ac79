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
#include "tuya_agentic.h"
#include "tuya_ble_prov.h"        /* tuya_ble_wifi_creds_t(BLE 配网结果类型)*/

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

/* ===== 上行延迟诊断开关(定位"打断不佳"用)=====
 * 打开后在 tai_send_audio_chunk 前后打时间戳,超 UPLINK_LAT_WARN_MS 才打印(避免刷屏)。
 * 同时打印 send 期间的 mic cbuf 水位变化,判断"send 慢→堆积→丢音"是否成立。
 * 以及 send 发生时 TTS 是否在播(锁竞争假说: TTS drain 期间 worker 频繁持 yield_mutex)。
 * 用法: 定位上行延迟/丢音问题时打开(取消下一行注释),默认关闭。*/
/* #define TUYA_UPLINK_LATENCY_DEBUG */
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
#define BARGE_MIN_ENERGY        100000u /* barge-in 能量门:Σ|int16|/帧。真话音远>>此值,回声/噪音<<此值;3帧≥2帧达标才打断 */

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
static volatile int g_barge_in;           /* barge-in 触发:跳过 TTS 后清缓冲/冷却,直接进新一轮(TUYA_BARGE_IN_ENABLE)*/
static volatile int g_exit;               /* on_disconnect 置位:令语音循环退出 */
static int g_audio_frame_logged;          /* 下行首帧帧长只打印一次,供核对 opus_cbr_pktlen */
#ifdef TUYA_SERVER_VAD_ENABLE
static volatile int g_server_vad_stop;    /* 云端VAD(TAI_EVT_SERVER_VAD)通知停说:上行循环据此收尾。on_event 在 worker 线程置位,主循环读 */
#endif
static volatile unsigned int g_barge_cooldown_until; /* barge-in 冷却到期 ms 时间戳;此前的 g_tts_playing 视为老轮在途残响,忽略(0=始终过期) */
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

/* MQTT 心跳维持线程(MQTT 常驻模式)。 */
static void tuya_mqtt_keepalive_task(void *arg)
{
    iot_client_t *iot = (iot_client_t *)arg;
    if (!iot) return;
    while (!g_exit) {
        iot_client_process(iot, 0);
        os_time_dly(500);
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
    printf("[TUYA-AI] %.*s\r\n", (int)msg->len, msg->text);
}
static void on_audio(tai_ctx_t *ctx, const tai_audio_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    /* 独立文本 demo 未起音频流 / 空帧:跳过,避免往未初始化的 cbuf 写 */
    if (!g_audio_ready || !msg || !msg->len) {
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
#ifdef TUYA_DOWNLINK_OPUS_ENABLE
    if (g_audio_frame_logged < 5) {
        /* 打印前 5 帧 len:核对 opus_cbr_pktlen。全一致=CBR(该值即每帧字节);忽大忽小=云端非 CBR */
        printf("[TUYA-AI] on_audio #%d: len=%d codec=%u sr=%u frame_ms=%u stream=%u\r\n",
               g_audio_frame_logged + 1, (int)msg->len, (unsigned)msg->codec,
               (unsigned)msg->sample_rate, (unsigned)msg->frame_duration,
               (unsigned)msg->stream_flag);
        g_audio_frame_logged++;
    }
#endif
    _device_write_voice_data((void *)msg->data, msg->len);
}
static void on_event(tai_ctx_t *ctx, const tai_event_msg_t *msg, void *ud)
{
    demo_ctx_t *dc = (demo_ctx_t *)ud;
    if (msg->event_type == TAI_EVT_END) {
        if (dc) {
            dc->got_done = 1;            /* 兼容独立文本 demo */
        }
        g_turn_done   = 1;
        g_tts_playing = 0;
        printf("[TUYA-AI] === 回答结束 ===\r\n");
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
        /* 最小 MCP 响应:空工具 */
        const char *empty_result =
            "{\"jsonrpc\":\"2.0\",\"id\":1,"
            "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"\"}]}}";
        tai_send_mcp_response(ctx, empty_result);
    }
}
static void on_disconnect(tai_ctx_t *ctx, const tai_disconnect_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    printf("[TUYA-AI] disconnected: reason=%u close_code=%u\r\n",
           (unsigned)msg->reason, (unsigned)msg->close_code);
    g_exit = 1;   /* 令语音循环退出(v1:下次开机重跑;常驻重连留作 fast-follow) */
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
 * 被 barge-in 切了却没播报,即此)。g_barge_prefill=有效帧数,仅确认通过时置。*/
static unsigned char g_barge_prebuf[1280 * 3];
static unsigned int g_barge_prefill;
/* barge-in 能量确认:VAD 触发时连读 3 帧(≈120ms),3 帧 sum 均≥BARGE_MIN_ENERGY 才算真话音。
 * 滤掉 AEC 残留回声/噪音的瞬时 spike——TTS 念密集数字(金价等)时某个响音爆破会在 1~2 帧内冲过
 * 阈值(实测 2-of-2 被这种 spike 骗过,误切断金价播报),要求 3 帧持续能量才能把 ≤2 帧的 spike 滤掉。
 * 真话音 onset 通常持续 >120ms,正常通过。代价:确认比 2 帧多 40ms。漏判会下轮询(20ms)重试。
 * 读到的帧存 g_barge_prebuf 供新上行补发,不再丢弃。帧长 1280=PCM 16k/16bit/mono 40ms。*/
static int barge_in_energy_confirmed(void)
{
    unsigned int s, a, hi = 0, k, n = 0;
    unsigned int sums[3] = {0, 0, 0};
    for (k = 0; k < 3 && !g_exit; k++) {
        if (_device_get_voice_data(&g_barge_prebuf[k * 1280], 1280) != 1280) break;
        opus_frame_stat(&g_barge_prebuf[k * 1280], 1280, &s, &a);
        sums[k] = s;
        if (s >= BARGE_MIN_ENERGY) hi++;
        n++;
    }
    if (hi >= 3 && n >= 3) {   /* 3-of-3:滤 ≤2 帧 spike;读不够 3 帧也算失败 */
        g_barge_prefill = n;
        printf("[TUYA] barge-in confirm 3/3 (sums=%u,%u,%u)\r\n", sums[0], sums[1], sums[2]);
        return 1;
    }
    return 0;   /* 失败不动 prefill;sums 不打(误触发每秒数次,太吵) */
}
#endif

/* 给定 iot_client(已激活/已初始化)+ local_key,换 token 并跑一轮文本对话。
 * 复用 parse_token / on_text / on_event 等。*/
static void tuya_ai_run(const pal_t *pal, iot_client_t *iot, const char *local_key)
{
    char *token = (char *)malloc(4096);
    if (!token) { iot_client_deinit(iot); return; }
    if (iot_client_get_session_token(iot, NULL, token, 4096) != 0 || !token[0]) {
        printf("[TUYA] get_session_token fail\r\n"); free(token); iot_client_deinit(iot); return;
    }
    tai_conn_params_t cp;
    if (parse_token(token, &cp) != 0) {
        printf("[TUYA] parse_token fail\r\n"); free(token); iot_client_deinit(iot); return;
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
    static const char SA[] =
        "{\"deviceMcp\":{\"supportCustomMCP\":true},"
        "\"tts.order.supports\":[{\"format\":\"opus\",\"sampleRate\":16000,\"channels\":1}]}";
#else
    static const char SA[] = "{\"deviceMcp\":{\"supportCustomMCP\":true}}";
#endif
#ifdef TUYA_SERVER_VAD_ENABLE
    /* chatAttributes(event_user_data):asr.enableVad / tts.alternate / processing.interrupt。
     * tts.alternate:"true" 让云端"一句文字→这句音频→下一句文字→下一句音频"交替下发,
     *   而不是"文字全发完→音频慢慢发"。文字和音频天然按句对齐,接屏幕做字幕不需额外计时。*/
    static const char EU[] =
        "{\"asr.enableVad\":\"true\","
        "\"tts.alternate\":\"true\","
        "\"processing.interrupt\":\"true\"}";
#else
    static const char EU[] =
        "{\"sys.workflow\":\"asr-llm-tts\","
        "\"tts.alternate\":\"true\"}";
#endif
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
    if (!mem) return;
    tai_ctx_t *ctx = tai_ctx_init(mem, &tc);
    if (!ctx) { pal->free(mem); return; }
    printf("[TUYA] tai_connect...\r\n");
    if (tai_connect(ctx) != TAI_OK) {
        printf("[TUYA] tai_connect fail\r\n");
        tai_ctx_deinit(ctx); pal->free(mem); return;
    }

    /* ===== 阶段3:免唤醒语音对话(本地 VAD 驱动)=====
     * 起音频流(mic OPUS 采集 + DAC 播放),然后循环:
     *   本地VAD检测开口 → 上行OPUS → 沉默收尾 → 云端回复(on_audio 播TTS)→ 重新待命。
     * 回声抑制:TTS 期间 g_tts_playing=1 暂停上行,播完冷却 ~300ms 再听。 */
    audio_stream_init(16000, 16, 1);
    start_audio_stream();
    g_audio_ready        = 1;
    g_tts_playing        = 0;
    g_turn_done          = 1;          /* 视为"上一轮已结束",直接进入听音 */
    g_exit               = 0;
    g_audio_frame_logged = 0;
    printf("[TUYA] voice loop ready (local-VAD, uplink=PCM 16k/mono)"
#ifdef TUYA_DOWNLINK_OPUS_ENABLE
            " downlink=opus"
#else
            " downlink=pcm"
#endif
            "\r\n");

    /* MQTT 常驻:fork 独立线程维持心跳 + 收 DP 下行(见 tuya_mqtt_keepalive_task)。
     * 不能在语音循环里调 iot_client_process——它内部 TLS recv 会阻塞语音线程。*/
    if (iot) {
        thread_fork("tuya_mqtt_ka", 5, 6 * 1024, 0, 0, tuya_mqtt_keepalive_task, iot);
    }

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

#define TUYA_OPUS_FRAME_LEN 1280   /* PCM 16k/16bit/mono 40ms = 1280B/帧(原 opus 180B 改 PCM) */
    unsigned char abuf[TUYA_OPUS_FRAME_LEN];
    /* 上行诊断:本轮帧计数/有效帧数 + 空闲排空的底噪基线 */
    unsigned int uplink_frames = 0, uplink_active = 0;
    unsigned int idle_cnt = 0, idle_sum = 0, idle_act_sum = 0;
#ifdef TUYA_SERVER_VAD_ENABLE
    /* 云端VAD模式的本地超时兜底:本地VAD连续判静音的帧数计数。
     * 超过 LOCAL_SILENCE_TIMEOUT_FRAMES 帧强制收尾,防云端SERVER_VAD丢失导致一直上行。
     * 每帧~40ms,50帧≈2秒持续静音。保留2s给云端VAD足够时间响应(上班后确认云端VAD开通)。*/
    #define LOCAL_SILENCE_TIMEOUT_FRAMES  50
    unsigned int silence_frames = 0;
#endif

    while (!g_exit) {
        /* ① 等待:未在播放 TTS 且本地 VAD 检测到开口。
         *   空闲时【持续排空】录音 cbuf(_device_get_voice_data 内部 mdelay(60) 按帧节拍),
         *   保证 VAD 触发时缓冲里没有积压旧音频——上行直接发"此刻"的实时语音。
         *   ⚠️ 绝不能在 VAD 触发时再 clear():那会把刚触发到的那句语音一并清掉,
         *      之前正是这样导致云端 ASR 收到静音、回空文本。*/
#ifdef TUYA_BARGE_IN_ENABLE
        /* barge-in:TTS 期间也听(不再被 g_tts_playing 门控),靠 AEC 去回声保证 VAD 不被喇叭误触发。
         * AEC 不行时这里会让回声触发 VAD→TTS 动辄自断,误触发多就关 TUYA_BARGE_IN_ENABLE 先调 AEC。*/
        int start_turn = get_recoder_state();
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
                } else {
                    start_turn = 0;   /* 噪音/远场,拒起轮(治自言自语) */
                }
            } else {
                start_turn = 0;
            }
        }
        if (!start_turn) {
#else
        if (g_tts_playing || !get_recoder_state()) {
#endif
#ifdef TUYA_BARGE_IN_ENABLE
            g_barge_prefill = 0;   /* idle 期间任何 pending barge-in prefill 都已过期,清掉 */
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
            }
            continue;
        }
#ifdef TUYA_BARGE_IN_ENABLE
        /* barge-in 起新上行前:排掉 TTS 期间积压的 stale mic 数据,只留最近 3 帧(刚触发的话音)。
         * 不排则 cbuf 压着 9~12 帧旧残音,新上行先发静音/回声残响→ASR 收垃圾/空文本。
         * _device_get_voice_data 内部 mdelay(40)/帧,故封顶 6 帧(≤240ms)防被持续说话拖住。
         * g_barge_in 在此处排完才清(让 barge-in 标记从 ④/drain 贯穿到新上行起点);普通轮
         * gate 的 idle-drain 已持续保持缓冲新鲜,不会进这里。*/
        if (g_barge_in) {
            unsigned int olvl = _device_get_voice_level();
            unsigned int keep = TUYA_OPUS_FRAME_LEN * 3;   /* 保留最近 3 帧(≈120ms 话音) */
            unsigned char _stale[TUYA_OPUS_FRAME_LEN];
            unsigned int drained = 0;
            while (_device_get_voice_level() > keep && drained < 6) {
                if (_device_get_voice_data(_stale, sizeof(_stale)) != TUYA_OPUS_FRAME_LEN) break;
                drained++;
            }
            if (drained) printf("[TUYA] barge-in: drained %u stale frames (lvl %u→%u)\r\n",
                                drained, olvl, _device_get_voice_level());
            g_barge_in = 0;   /* 排完才清 */
        }
#endif
        g_turn_done = 0;
        uplink_frames = 0; uplink_active = 0;
#ifdef TUYA_SERVER_VAD_ENABLE
        g_server_vad_stop = 0;   /* 清掉上轮残留的云端VAD标志 */
        silence_frames = 0;       /* 清掉上轮残留的静音兜底计数 */
#endif
        printf("[TUYA] speak-start: uplink begin (cbuf_level=%u B)\r\n", _device_get_voice_level());
        if (tai_send_audio_start(ctx, TAI_AUDIO_PCM, 1, 16, 16000) != TAI_OK) {
#ifdef TUYA_BARGE_IN_ENABLE
            g_barge_prefill = 0;   /* 本轮没起上行,别让 prefill 漏到下一轮 */
#endif
            printf("[TUYA] tai_send_audio_start fail\r\n");
            msleep(200);
            continue;
        }
#ifdef TUYA_BARGE_IN_ENABLE
        /* barge-in 轮:先补发能量确认时读走的 onset 帧(最响那段),再进实时上行。
         * 否则 onset 丢失、上行只收尾音/静音→ASR 空→打断后无播报。*/
        if (g_barge_prefill) {
            unsigned int k;
            for (k = 0; k < g_barge_prefill; k++) {
                tai_send_audio_chunk(ctx, &g_barge_prebuf[k * 1280], 1280);
            }
            printf("[TUYA] barge-in: prepended %u confirm frames\r\n", g_barge_prefill);
            g_barge_prefill = 0;
        }
#endif

        /* ②③ 内循环:每帧(≈60ms)上行 180B;VAD 判停说或云端已开始回话则收尾 */
        while (!g_exit) {
            if (g_tts_playing && barge_cooldown_expired()) {  /* 云端开始回话;冷却内忽略老轮在途 TTS,别截断 barge-in 新轮上行 */
                break;
            }
            int n = _device_get_voice_data(abuf, TUYA_OPUS_FRAME_LEN);
            if (n == TUYA_OPUS_FRAME_LEN) {
                unsigned int fsum, fact;
                opus_frame_stat(abuf, TUYA_OPUS_FRAME_LEN, &fsum, &fact);

#ifdef TUYA_UPLINK_LATENCY_DEBUG
                /* 测 send 耗时 + 前后 cbuf 水位 + TTS 上下文。
                 * 正常: send 5-15ms,cbuf 水位接近 0(每帧 mdelay40 消费节拍)。
                 * 异常: send 几十~几百ms,cbuf 水位涨(锁竞争/网络抖动/被抢占)。
                 * 只在超阈值时打印,避免刷屏;阈值见文件顶 UPLINK_LAT_WARN_MS。*/
                unsigned int _lvl_pre = _device_get_voice_level();
                unsigned int _t0 = timer_get_ms();
                int _tts_flag = g_tts_playing;   /* 快照:send 时 TTS 是否在播(锁竞争假说关键信号)*/
                int _snd_rt = tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN);
                unsigned int _dt = timer_get_ms() - _t0;
                unsigned int _lvl_post = _device_get_voice_level();
                if (_snd_rt != TAI_OK) {
                    printf("[TUYA] tai_send_audio_chunk fail\r\n");
                    break;
                }
                /* 超时 或 水位异常高 才打印 */
                if (_dt >= UPLINK_LAT_WARN_MS || _lvl_pre >= UPLINK_LAT_CBUF_WARN) {
                    printf("[LAT] send=%ums lvl=%u→%u (+%u) %s\r\n",
                           _dt, _lvl_pre, _lvl_post, _lvl_post - _lvl_pre,
                           _tts_flag ? "[TTS!]" : "");
                }
#else
                if (tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN) != TAI_OK) {
                    printf("[TUYA] tai_send_audio_chunk fail\r\n");
                    break;
                }
#endif

                uplink_frames++;
                if (fact > 3) uplink_active++;   /* 新度量下 fact=avg|sample|/100;>3 即 avg>300(远超 idle 底噪 avg<50)≈有效话音帧 */
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
            /* 本地超时兜底:云端VAD事件丢失/延迟时,本地VAD连续判静音超过阈值则强制收尾。
             * 本地VAD还在说话(=1)就重置计数;持续静音(=0)累积,到50帧(≈2s)兜底。*/
            if (!get_recoder_state()) {
                if (++silence_frames > LOCAL_SILENCE_TIMEOUT_FRAMES) {
                    printf("[TUYA] speak-stop: local timeout fallback (frames=%u)\r\n", uplink_frames);
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
        tai_send_audio_end(ctx);

        /* ④ 等本轮回复结束(云端 TTS 播完,TAI_EVT_END 置 g_turn_done)再回 ① */
        int w = 0;
        while (!g_exit && !g_turn_done && w < TUYA_WAIT_MS) {
#ifdef TUYA_BARGE_IN_ENABLE
            /* barge-in:TTS 播放期间本地 VAD 检测到说话 → 打断。靠 AEC 保证 VAD 不是被回声触发。*/
            if (g_tts_playing && get_recoder_state() && barge_cooldown_expired()) {
                if (!barge_in_energy_confirmed()) {
                    printf("[TUYA] barge-in: VAD fired but energy low (AEC残留/噪音?), ignored\r\n");
                } else {
                    printf("[TUYA] barge-in: VAD during TTS → chat_break + stop TTS\r\n");
                    tai_chat_break(ctx);          /* 通知云端中止本轮 TTS */
                    _device_rbuf_clear();         /* 清播放 cbuf,立刻停 TTS 喇叭 */
                    g_tts_playing = 0;
                    g_barge_in = 1;
                    g_barge_cooldown_until = timer_get_ms() + TUYA_BARGE_COOLDOWN_MS; /* 冷却:压住老轮在途 TTS 二次触发 */
                    break;
                }
            }
#endif
            msleep(20); w += 20;   /* ④ 轮询 50→20ms,压低 barge-in 检测延迟 */
        }
        /* 回复期间(TTS 播放/等云端)录音 cbuf 积压了环境音/TTS 回采,这里清掉——
           此刻没有要保留的用户语音,清它是安全的(与"VAD触发时清"不同,那才会吞掉话音)。
           barge-in 时用户正在说话,【不能】清缓冲也不能冷却,跳过直接进新一轮上行。*/
        if (!g_barge_in) {
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
                msleep(20); wait_start += 20;
#ifdef TUYA_BARGE_IN_ENABLE
                /* TTS 未到达阶段也检测 barge-in:用户改口不等 TTS,确认话音即打断当前轮。*/
                if (get_recoder_state() && barge_cooldown_expired()) {
                    if (barge_in_energy_confirmed()) {
                        printf("[TUYA] barge-in (pre-TTS): cancel before audio arrives\r\n");
                        tai_chat_break(ctx);
                        _device_rbuf_clear();
                        g_tts_playing = 0;
                        g_barge_in = 1;
                        g_barge_cooldown_until = timer_get_ms() + TUYA_BARGE_COOLDOWN_MS;
                        break;
                    }
                }
#endif
            }
            unsigned int wait_ms = 0;
            while (!g_exit && _device_get_play_level() > 640 && wait_ms < 35000) {  /* 等 32s 缓冲排空(≈喇叭真播完);35s 是兜底,正常排完就提前退 */
                msleep(20); wait_ms += 20;
#ifdef TUYA_BARGE_IN_ENABLE
                /* barge-in 也要在 play-drain-wait 里检测!云端 EVT_END 后 ④ 已退出,但喇叭还在放
                 * 缓冲里的 TTS。这段期间用户说话必须能打断(否则大缓冲=大窗口无法打断)。*/
                if (get_recoder_state() && barge_cooldown_expired()) {
                    if (!barge_in_energy_confirmed()) {
                        printf("[TUYA] barge-in (drain): VAD fired but energy low, ignored\r\n");
                    } else {
                        printf("[TUYA] barge-in (drain): VAD during TTS drain → chat_break + stop\r\n");
                        tai_chat_break(ctx);
                        _device_rbuf_clear();
                        g_tts_playing = 0;
                        g_barge_in = 1;
                        g_barge_cooldown_until = timer_get_ms() + TUYA_BARGE_COOLDOWN_MS;
                        break;
                    }
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
            unsigned char _trash[TUYA_OPUS_FRAME_LEN];
            for (int i = 0; i < 8 && !g_exit; i++) {  /* 8 × ~40ms ≈ 300ms 排空 mic */
                _device_get_voice_data(_trash, sizeof(_trash));
            }
        }
        /* g_barge_in 不在此清:改由 barge-in 新上行起点(循环顶 drain-stale 处)清,让标记贯穿。*/
    }
#undef TUYA_OPUS_FRAME_LEN

    g_audio_ready = 0;
    stop_audio_stream();
    tai_disconnect(ctx);
    tai_ctx_deinit(ctx);
    pal->free(mem);
}

/* 配网等待期循环播报"请配置网络",每 30s 一次,避免用户以为设备死机(tuya/小智的做法)。
 * tuya_ble_netcfg_start 阻塞,故用独立线程周期播报;配网完成/失败/超时置
 * s_prov_prompt_run=0,线程在 ~0.1s 内退出。NetCfgEnter.mp3 是 app_music 现有提示音。*/
static volatile int s_prov_prompt_run;
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
        printf("[TUYA] synced wifi to JL store: ssid=%s\r\n", ssid);
    }
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
            printf("[TUYA] wifi already connected by wifi_app_task (ssid=%s)\r\n", ssid);
            /* 同步到杰理 wifi 存储:防 app_music 的 wifi_return_sta_mode 读到旧 ssid 覆盖 */
            tuya_sync_wifi_to_jl(ssid, pwd);
        } else {
            /* devid 在但 ssid 空:本修复前烧的设备没存 ssid → 没法联网,直连 AI 必失败。
             * 提示一下,重配一次网即补上 ssid。*/
            printf("[TUYA] WARN: devid present but no stored wifi ssid -> AI will fail, re-provision once\r\n");
        }

        iot_client_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.region = AY; cfg.env = PROD;
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
    printf("[TUYA] no devid, start BLE provisioning...\r\n");
    s_prov_prompt_run = 1;   /* 启动"请配置网络"循环播报(每 30s),配网完成会停 */
    thread_fork("tuya_prov_prompt", 6, 4 * 1024, 0, 0, tuya_prov_prompt_task, NULL);
    int prov_ret = tuya_ble_netcfg_start("TUYA", TUYA_PRODUCT_KEY, TUYA_UUID, TUYA_AUTH_KEY, main_prov_cb);
    s_prov_prompt_run = 0;   /* 配网完成/失败/超时,停循环播报 */
    os_time_dly(15);         /* ~150ms:让 prompt 线程看到标志退出,别让它播报到连 WiFi/激活阶段 */
    if (prov_ret != 0) {
        printf("[TUYA] BLE provisioning failed/timeout\r\n"); return;
    }
    tuya_ble_netcfg_stop();   /* 配网完停 BLE,释放内存给 WiFi/TLS */
    printf("[TUYA] BLE done: ssid=%s token=%s\r\n", s_main_creds.ssid, s_main_creds.token);

    /* ---- 连 WiFi(配网给的 ssid/密码)---- */
    wifi_enter_sta_mode(s_main_creds.ssid, s_main_creds.password);
    /* 等 WiFi 关联成功:原死等满 20s 太慢,改成轮询真实状态,一连上就继续。 */
    for (int i = 0; i < 100; i++) {   /* 最多等 ~20s */
        if (wifi_get_sta_connect_state() == WIFI_STA_CONNECT_SUCC) {
            printf("[TUYA] wifi STA connected, wait DHCP...\r\n");
            break;
        }
        msleep(200);
    }
    msleep(1500);   /* 关联成功后给 DHCP ~1.5s 拿 IP(实测 DHCP 在 SUCC 后 ~0.4s 完成) */

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
    if (!iot) { printf("[TUYA] on_boarding_with_token fail\r\n"); return; }
    printf("[TUYA] activated, devid=%s\r\n", iot->devid);

    /* ---- 持久化三元组 + WiFi 凭据(下次开机直连)----
     * 三元组连涂鸦 AI 云;ssid/password 供直连路径开机重连 WiFi(否则开机离线,连 AI 必失败)。*/
    char lk[32] = {0};
    strncpy(lk, (const char *)iot->local_key, sizeof(lk) - 1);
    syscfg_write(VM_TUYA_DEVID_IDX,    iot->devid,            32);
    syscfg_write(VM_TUYA_SECRET_IDX,   iot->secret_key,       32);
    syscfg_write(VM_TUYA_LOCALKEY_IDX, iot->local_key,        32);
    syscfg_write(VM_TUYA_SSID_IDX,     s_main_creds.ssid,     65);
    syscfg_write(VM_TUYA_PWD_IDX,      s_main_creds.password, 65);
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
