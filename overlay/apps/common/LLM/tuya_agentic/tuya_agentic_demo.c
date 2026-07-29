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
void audio_stream_init(int sample_rate, int bit_dept, int channel_num);
void start_audio_stream(void);
void stop_audio_stream(void);
int  get_recoder_state(void);
unsigned int _device_get_voice_level(void);   /* 录音 cbuf 水位,诊断上行是否丢话头 */
unsigned int _device_get_play_level(void);    /* 下行播放 cbuf 水位:排空≈DAC 播完 */

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
        printf("[TUYA-AI] server-vad\r\n");
    } else if (msg->event_type == TAI_EVT_CHAT_BREAK) {
        /* 服务端中止 TTS(本地 tai_chat_break 的回执,或云端检测到打断)。中止≠本轮结束,
         * 不置 g_turn_done——让新一轮的 TAI_EVT_END 来结束。*/
        printf("[TUYA-AI] chat_break (TTS aborted)\r\n");
        g_tts_playing = 0;
    } else if (msg->event_type == TAI_EVT_MCP_CMD) {
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
    iot_client_deinit(iot);

    /* ⑤ 组装 tai_config */
    static const char SESSION_ATTRS[] = "{\"deviceMcp\":{\"supportCustomMCP\":true}}";
    static const char EVENT_USER_DATA[] = "{\"sys.workflow\":\"asr-llm-tts\"}";

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

    tai_set_log_level(TAI_LOG_WARN);

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
           cp.agent_token[0] ? cp.agent_token : "(none)");
    free(token);
    iot_client_deinit(iot);   /* 必须断 MQTT:保活(并发)时云端会 SESSION_CLOSE 关掉 AI 会话
     * (iot MQTT 与 AI TLS 共用 PAL/设备身份,并发冲突)。改为在 tuya_agentic_main 里
     * 先 hold MQTT 几秒让 App 判定配网成功,再进这里 deinit 断 MQTT 连 AI(串行)。 */

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
    static const char EU[] = "{\"sys.workflow\":\"asr-llm-tts\"}";
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
    tai_set_log_level(TAI_LOG_WARN);
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
    printf("[TUYA] voice loop ready (local-VAD, opus 16k/mono/60ms/180B)\r\n");

    /* ===== 对照实验:文本通道探针(音频流已起,若云端回 TTS 可顺带验证下行播放)=====
     *   文本正常回复 → 会话通,问题锁定在音频上行(云端 ASR 不认我们的 opus)
     *   文本回空/超时 → 会话或 biz_code/agent 配置问题(语音也跟着废) */
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

#define TUYA_OPUS_FRAME_LEN 1280   /* PCM 16k/16bit/mono 40ms = 1280B/帧(原 opus 180B 改 PCM) */
    unsigned char abuf[TUYA_OPUS_FRAME_LEN];
    /* 上行诊断:本轮帧计数/有效帧数 + 空闲排空的底噪基线 */
    unsigned int uplink_frames = 0, uplink_active = 0;
    unsigned int idle_cnt = 0, idle_sum = 0, idle_act_sum = 0;

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
                if (tai_send_audio_chunk(ctx, abuf, TUYA_OPUS_FRAME_LEN) != TAI_OK) {
                    printf("[TUYA] tai_send_audio_chunk fail\r\n");
                    break;
                }
                uplink_frames++;
                if (fact > 3) uplink_active++;   /* 新度量下 fact=avg|sample|/100;>3 即 avg>300(远超 idle 底噪 avg<50)≈有效话音帧 */
                printf("[TUYA] uplink f#%u sum=%u act=%u%%\r\n", uplink_frames, fsum, fact);
            }
            if (!get_recoder_state()) {     /* enc VAD 已 debounce 判定停说(stop 阈值) */
                printf("[TUYA] speak-stop: uplink end (frames=%u active=%u)\r\n",
                       uplink_frames, uplink_active);
                break;
            }
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
        for (int i = 0; i < 300 && s_prov_prompt_run; i++) {
            os_time_dly(10);   /* 100ms × 300 = 30s;100ms 粒度查退出标志 */
        }
        if (!s_prov_prompt_run) break;
        app_music_play_netcfg_prompt();
    }
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

    if (iot_init(pal) != 0) { printf("[TUYA] iot_init fail\r\n"); return; }

    /* log_default_handler 用 fprintf(stderr,...),AC79 没有 stderr → axi_rd_inv 崩溃。
     * 直接把 log level 设成 NONE,关掉所有日志,绕过 fprintf。 */
    extern void log_set_level(int);
    log_set_level(0); /* LOG_NONE */

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
            printf("[TUYA] reconnect wifi ssid=%s\r\n", ssid);
            wifi_enter_sta_mode(ssid, pwd);
            for (int i = 0; i < 100; i++) {   /* 最多 ~20s,一连上就继续 */
                if (wifi_get_sta_connect_state() == WIFI_STA_CONNECT_SUCC) {
                    printf("[TUYA] wifi STA connected, wait DHCP...\r\n");
                    break;
                }
                msleep(200);
            }
            msleep(1500);   /* 给 DHCP ~1.5s 拿 IP */
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
        strncpy((char *)cfg.devid, devid, sizeof(cfg.devid) - 1);
        strncpy((char *)cfg.secret_key, secret, sizeof(cfg.secret_key) - 1);
        strncpy((char *)cfg.local_key, localkey, sizeof(cfg.local_key) - 1);
        iot_client_t *iot = iot_client_init(&cfg);
        if (iot) { tuya_ai_run(pal, iot, localkey); }
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

    /* ---- on_boarding 激活 ---- */
    iot_on_boarding_config_t ob;
    memset(&ob, 0, sizeof(ob));
    strncpy((char *)ob.uuid,        TUYA_UUID,        sizeof(ob.uuid) - 1);
    strncpy((char *)ob.authkey,     TUYA_AUTH_KEY,    sizeof(ob.authkey) - 1);
    strncpy((char *)ob.product_key, TUYA_PRODUCT_KEY, sizeof(ob.product_key) - 1);
    ob.env = PROD; ob.mqtt_disable_tls = false; ob.mqtt_auto_connect = 1; ob.timeout_ms = 30000;
    ob.cert_bundle_attach = NULL; ob.cacert = NULL;
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

    /* ---- hold MQTT 让 App 判定配网成功 ----
     * on_boarding 已建好 MQTT(涂鸦IoT云,设备上线绑定)。App 判"配网成功"靠它,需保持
     * 几秒让云端同步给 App。之后 tuya_ai_run 会 deinit 断 MQTT 再连 AI(MQTT 与 AI TLS
     * 并发时云端会 SESSION_CLOSE 关掉 AI 会话,必须串行)。
     * 代价:之后 App 里该设备显示离线(控制暂不可用),但配网已成功、语音能用。 */
    printf("[TUYA] hold MQTT ~8s for app to confirm provisioning...\r\n");
    msleep(8000);

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
