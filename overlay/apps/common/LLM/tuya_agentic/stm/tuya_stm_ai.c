/*
 * tuya_stm_ai.c -- tuya_ai.h(tai_* API)的 STM OPEN SDK 后端实现。
 *
 * 传输层:libstm 在 stm_open_session_create 内部对 UDP(DTLS) 与 TCP 竞速,
 * UDP 连通即销毁 TCP 通道("udp connected, destroy tcp conn"),UDP 优先;
 * UDP 不通自动回落 TCP。上层(本文件)无需也无法指定传输方式。
 *
 * 会话语义(逆向 libstm bitcode + 官方接入文档得出,详见 stm/README.md):
 *  - 连接参数完全来自原始 session token(库自行解析 connect_conf/hosts/
 *    udpport 等),设备侧只需把 iot_client_get_session_token 的 base64 串
 *    原样通过 tstm_set_session_token() 绑定;
 *  - 上行按"事件"组织:每个事件第一包带 event_id + 数据参数(音频头等)
 *    + app_data(仅首包),后续包 event_id/app_data 置 NULL,末包 fin=1;
 *    库内 sent_mask 自动推导 ONE_SHOT/START/MIDDLE/END 流标记;
 *  - 下行 on_data_recv:数据包(fin=0)带数据类型/参数/载荷;事件结束标记
 *    以原数据类型 + payload=NULL + fin=1 送达;打断等指令以 CMD(1) 类型
 *    送达(cmd_type=BREAK 可识别,其余指令类型号本身丢失);
 *  - on_state:1=会话就绪(连接 CONNECTED/RECOVERING),2=会话关闭
 *    (连接 DISCONNECTED/FAILED),只升不降。
 *
 * 已知限制(详见 stm/README.md):
 *  - 云端 VAD(TAI_EVT_SERVER_VAD)在 stm 下行无法区分表达,云端 VAD 模式
 *    下依赖 demo 的本地静音兜底;
 *  - 上行打断/打断回应(MCP response)走 libstm 未公开导出的底层
 *    stm_session_send(),按 stm_open_session 布局偏移取 sid,已在发送前做
 *    sid 一致性校验,若厂商库结构变化则放弃发送并报错(不崩溃)。
 */

#include "app_config.h"

#if defined(CONFIG_TUYA_AGENTIC_ENABLE) && defined(TUYA_TRANSPORT_STM_ENABLE) && TUYA_TRANSPORT_STM_ENABLE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "tuya_stm_ai.h"        /* -> tuya_ai.h(tai 类型/常量/回调) */
#include "stm_open.h"           /* STM OPEN SDK 公开 API            */

#include "rng.h"                /* rng_bytes:事件号生成             */
#include "system/timer.h"       /* sys_timer_get_ms                */
#include "system/os/os_api.h"   /* os_time_dly                     */

/* ------------------------------------------------------------------------- */
/* 可调配置(app_config.h 可覆盖)                                            */
/* ------------------------------------------------------------------------- */

/* 库日志最低打印级别(0=VERBOSE..4=ERROR,5=FATAL);默认只打 WARN 及以上。
 * 定为 0 可看全部库日志(排查用,串口量大)。 */
#ifndef TUYA_STM_LOG_PRINT_LEVEL
#define TUYA_STM_LOG_PRINT_LEVEL  3
#endif

/* ★库内部的日志阈值。on_log 只收到 >= 该级别的日志。
 *   排查期曾开 DEBUG 看引擎日志(2026-08-31 轮B拿到全部连接层证据),但实测
 *   DEBUG 洪流下音频首包必崩 axi_rd_inv:根因是 JL printf 无锁,引擎线程经
 *   回调高频 printf 与 demo 任务 printf 撞车(日志中存在两条整行字符级交错的
 *   实锤),并非库自身问题。现已改延迟打印(TUYA_STM_LOG_DEFER),如需再开
 *   DEBUG 改这里即可,但串口量大且仍需观察。 */
#ifndef TUYA_STM_LIB_LOG_LEVEL
#define TUYA_STM_LIB_LOG_LEVEL    STM_LOG_LEVEL_WARN
#endif

/* 库日志延迟打印:1=引擎线程只把日志行拷进环形缓冲,由 demo 任务统一
 * printf 刷出(消除跨线程 printf 撞车);0=回调里直接 printf(旧行为,
 * 多线程并发时有崩溃风险)。demo 侧需周期调用 tstm_log_flush()。 */
#ifndef TUYA_STM_LOG_DEFER
#define TUYA_STM_LOG_DEFER        1
#endif

/* 上行 OPUS 的 stm codec_type。历史记录:8/31 轮换实验曾据"3 正确/111 乱码"
 * 定死 3,但 9/4 复测 codec=3+参数全 0 整轮静默,且 D:\code\agentic-kit 最新
 * 官方源码给出决定性证据:TCP 协议 TAI_AUDIO_OPUS=111(tuya_ai.h:89),Opus
 * 上行必带 bitrate/frame_dur/frame_sz(tai_protocol.c 从帧长推导:80B/帧→
 * 40ms/16000bps);udp_chat_demo(Mac 实测可用)填全 audio_params。8/31 的
 * "111 乱码"实为无帧参数导致切帧失败(解码器跑了但解出垃圾),"3 正确"那次
 * 疑受当日 MCP TEXT 污染干扰。现定死 111 + 帧参数齐备。*/
#ifndef TUYA_STM_OPUS_CODEC_TYPE
#define TUYA_STM_OPUS_CODEC_TYPE  111
#endif

/* 对照组值(留作复验):TUYA_STM_CODEC_ALTERNATE=1 时奇数轮用 111、
 * 偶数轮用 3,一轮测试同时验证两个取值(codec 值即轮次标识,看
 * [TSTM] audio start 日志)。默认 0(定死 111)。 */
#ifndef TUYA_STM_OPUS_CODEC_TYPE_ALT
#define TUYA_STM_OPUS_CODEC_TYPE_ALT  3
#endif
#ifndef TUYA_STM_CODEC_ALTERNATE
#define TUYA_STM_CODEC_ALTERNATE  0
#endif

/* MCP 回应走哪条上行通道:
 *   1 = 公开 TEXT 数据事件(现默认)。⚠️ 2026-09-02 反证实验:改走私有指令后,
 *       云端对语音【全程零响应】(连 ASR 都不出,与第2/3轮私有指令时代现象
 *       一致,三次独立开机)——initialize 的 TEXT 回应是云端语音管线激活的
 *       必要条件,尽管云端同时把它当用户聊天输入(开机后答非所问一句
 *       "你还是没有告诉我具体内容…",该回复期间说话不做 ASR,每会话一次)。
 *       官方回应通道/类型号待云端 FAE 确认后替换(改 TUYA_STM_MCP_INSTR_TYPE)。
 *   0 = 私有指令 type=TUYA_STM_MCP_INSTR_TYPE:云端静默忽略,且语音管线
 *       一并不激活——不能用作默认,仅留作对照。 */
#ifndef TUYA_STM_MCP_VIA_TEXT
#define TUYA_STM_MCP_VIA_TEXT     1
#endif

/* 是否启用私有底层通道(上行打断/MCP 回应)。默认开。 */
#ifndef TUYA_STM_PRIVATE_CMD_ENABLE
#define TUYA_STM_PRIVATE_CMD_ENABLE  1
#endif

/* MCP 回应走的私有指令类型号。1000 是对照 tai TCP 协议的推测,无文档背书;
 * 看 [TSTM] mcp downlink cmd_type=... 日志,云端下发用几就用几(待确认#4)。 */
#ifndef TUYA_STM_MCP_INSTR_TYPE
#define TUYA_STM_MCP_INSTR_TYPE  1000
#endif

/* app_data 包裹缓冲大小:{"sessionAttributes":"<escaped json>"} */
#define TSTM_APP_DATA_MAX     384

/* stm_open_session(库内部结构,不透明)中 stm_sid_t 的偏移。
 * 逆向 libstm bitcode 验证:next@0 cid@4 sid@40;发送前有 sid.id 与本侧
 * session_id 的一致性校验兜底。 */
#define TSTM_SESSION_SID_OFFSET  40

/* libstm 未在公开头文件中导出的底层接口(符号在 libstm_tuya.a 中为全局) */
extern int stm_session_send(stm_sid_t *sid, stm_instruction_t *instruction);

/* ------------------------------------------------------------------------- */
/* 上下文                                                                     */
/* ------------------------------------------------------------------------- */

struct tai_ctx {
    tai_config_t         cfg;                          /* 浅拷贝,指针按 tai 约定保持有效 */
    stm_open_session_t  *session;

    char                 session_id[STM_SID_MAX_LEN + 1];
    char                 cur_event_id[STM_EVENT_ID_MAX_LEN + 1];
    char                 sa_json[TSTM_APP_DATA_MAX];   /* {"sessionAttributes":".."} */
    char                 ca_json[TSTM_APP_DATA_MAX];   /* {"chatAttributes":".."}     */

    /* 上行音频事件状态。STM 要求最后一个真实音频包自身携带 fin=1，不能另发
     * payload=NULL 的结束包；因此始终暂存最新一帧，下一帧到来时才发出上一帧。 */
    int                  audio_open;                   /* audio_start 后有效 */
    int                  audio_started;                /* 已向 STM 发出过数据包 */
    uint16_t             a_codec;
    uint16_t             a_channels;
    uint16_t             a_bit_depth;
    uint32_t             a_sample_rate;
    /* 帧式编码(Opus)的描述参数:云端解码器需要帧长/帧大小才能切帧,
     * 全 0 时实测 codec=3 整轮被静默丢弃(PCM 自描述不受影响)。
     * 取值对齐本地编码器:CBR 16kbps / 40ms → 16000*0.04/8 = 80B/帧。 */
    uint32_t             a_bitrate;
    uint16_t             a_frame_duration;
    uint16_t             a_frame_size;
    uint32_t             audio_pending_len;
    uint8_t              audio_pending[1280];          /* PCM/Opus 上行最大均为 40ms PCM 帧 */

    /* 下行流标记合成 */
    char                 down_event_id[STM_EVENT_ID_MAX_LEN + 1];
    int                  down_open;                    /* 0=上轮已结束,下一包是 START */

    /* 状态(引擎线程写,业务线程读) */
    volatile int         ready;
    volatile int         dead;
    volatile int         closing;
};

/* 原始 session token(iot_client_get_session_token 输出,base64 串) */
static char s_session_token[STM_TOKEN_MAX_LEN];

static int s_stm_inited;

size_t tstm_ctx_size(void)
{
    return sizeof(struct tai_ctx);
}

/* ------------------------------------------------------------------------- */
/* 工具                                                                       */
/* ------------------------------------------------------------------------- */

static int tstm_map_err(stm_ret r)
{
    switch (r) {
    case STM_OK:             return TAI_OK;
    case STM_EINVALID_PARM:  return TAI_ERR_ARGS;
    case STM_EMALLOC_FAILED: return TAI_ERR_MEM;
    case STM_EAGAIN:         return TAI_ERR_AGAIN;
    default:                 return TAI_ERR_NET;
    }
}

/* 生成短 id:"prefix" + 8 hex。优先用 kit 的 RNG,未初始化则退化为
 * 计数器 + 时间戳(同一次开机内唯一即可)。 */
static void tstm_gen_id(char *dst, size_t dstsz, const char *prefix, const tai_ctx_t *ctx)
{
    static uint16_t s_seq;
    uint8_t r[4];

    if (ctx && ctx->cfg.pal && rng_bytes(ctx->cfg.pal, r, sizeof(r)) == 0) {
        /* ok */
    } else {
        uint32_t t = (uint32_t)sys_timer_get_ms();
        s_seq++;
        r[0] = (uint8_t)(s_seq >> 8);
        r[1] = (uint8_t)s_seq;
        r[2] = (uint8_t)(t >> 8);
        r[3] = (uint8_t)(t ^ (t >> 16) ^ s_seq);
    }
    snprintf(dst, dstsz, "%s%02x%02x%02x%02x", prefix, r[0], r[1], r[2], r[3]);
}

/* 把一段业务 JSON 包裹成 app_data 通道格式:{"<key>":"<escaped json>"}
 * 与 tai TCP 版 build_userdata_json 的转义规则一致。返回 0 成功。 */
static int tstm_wrap_attr(char *out, size_t outsz, const char *key, const char *json)
{
    const char *p;
    size_t o = 0;
    int n;

    out[0] = '\0';
    if (!json || !json[0]) {
        return 0;
    }

    n = snprintf(out + o, outsz - o, "{\"%s\":\"", key);
    if (n < 0) {
        out[0] = '\0';
        return -1;
    }
    o += (size_t)n;
    for (p = json; *p && o + 8 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c < 0x20) {
            n = snprintf(out + o, outsz - o, "\\u%04x", c);
            if (n < 0) {
                break;
            }
            o += (size_t)n;
        } else {
            out[o++] = (char)c;
        }
    }
    if (o + 2 >= outsz || *p != '\0') {
        /* 放不下/被截断说明业务 JSON 异常长,放弃(会话仍可建,只是不带该属性) */
        printf("[TSTM] wrap %s overflow, drop\r\n", key);
        out[0] = '\0';
        return -1;
    }
    out[o++] = '"';
    out[o++] = '}';
    out[o] = '\0';
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 库初始化 + 日志                                                            */
/* ------------------------------------------------------------------------- */

#if TUYA_STM_LOG_DEFER
/* 库日志环形缓冲:生产者(引擎线程/调用线程,在库的 log_mutex 内)只做拷贝;
 * 消费者(demo 任务)tstm_log_flush() 统一 printf。JL printf 无锁,跨线程
 * 并发打印轻则整行交错、重则内部状态写坏→axi_rd_inv(2026-08-31 轮B实测:
 * DEBUG 洪流下音频首包必崩,寄存器两次逐位一致;日志中有两条整行字符级
 * 交错的实锤)。溢出策略:丢最旧,打一条提示。 */
#define TSTM_LOG_RING_LINES   16
#define TSTM_LOG_LINE_MAX     160
static char s_log_ring[TSTM_LOG_RING_LINES][TSTM_LOG_LINE_MAX];
static volatile unsigned s_log_w;    /* 只在临界区内改 */
static volatile unsigned s_log_r;    /* 只有 demo 任务改 */
#endif

static void tstm_on_log(stm_log_level_e level, const char *log, uint32_t log_len)
{
    if ((int)level < TUYA_STM_LOG_PRINT_LEVEL || !log) {
        return;
    }
#if TUYA_STM_LOG_DEFER
    {
        unsigned w;
        unsigned n = log_len;
        if (n > TSTM_LOG_LINE_MAX - 1) {
            n = TSTM_LOG_LINE_MAX - 1;
        }
        OS_ENTER_CRITICAL();
        if (s_log_w - s_log_r >= TSTM_LOG_RING_LINES) {
            s_log_r = s_log_w - TSTM_LOG_RING_LINES + 1;   /* 丢最旧 */
        }
        w = s_log_w % TSTM_LOG_RING_LINES;
        memcpy(s_log_ring[w], log, n);
        s_log_ring[w][n] = '\0';
        s_log_w++;
        OS_EXIT_CRITICAL();
    }
#else
    printf("[STM][%d] %.*s\r\n", (int)level, (int)log_len, log);
#endif
}

#if TUYA_STM_LOG_DEFER
/* demo 任务周期调用:把缓冲中的库日志统一打印(唯一的 printf 出口) */
void tstm_log_flush(void)
{
    while (s_log_r != s_log_w) {
        unsigned r = s_log_r % TSTM_LOG_RING_LINES;
        printf("[STM] %s\r\n", s_log_ring[r]);
        s_log_r++;
    }
}
#else
void tstm_log_flush(void)
{
}
#endif

static void tstm_global_init(void)
{
    stm_open_config_t c;
    stm_ret r;

    if (s_stm_inited) {
        return;
    }
    memset(&c, 0, sizeof(c));
    c.on_log = tstm_on_log;
    r = stm_open_init(&c);
    if (r == STM_OK || r == STM_EINIT_MORE_THAN_ONCE) {
        s_stm_inited = 1;
        /* 库内部阈值:on_log 只收到 >= 该级别的日志(曾钉死 WARN 导致应用层
         * PRINT_LEVEL 设了也看不到 DEBUG,2026-08-31 排查发现) */
        stm_open_set_log_level(TUYA_STM_LIB_LOG_LEVEL);
        printf("[TSTM] libstm ready, version 0x%08x\r\n",
               (unsigned)stm_open_get_version());
    } else {
        printf("[TSTM] stm_open_init err %d\r\n", (int)r);
    }
}

void tstm_set_session_token(const char *raw_b64_token)
{
    if (!raw_b64_token || !raw_b64_token[0]) {
        s_session_token[0] = '\0';
        return;
    }
    if (strlen(raw_b64_token) >= sizeof(s_session_token)) {
        printf("[TSTM] token too long(%u), drop\r\n",
               (unsigned)strlen(raw_b64_token));
        s_session_token[0] = '\0';
        return;
    }
    strcpy(s_session_token, raw_b64_token);
}

/* ------------------------------------------------------------------------- */
/* 下行分发(引擎线程)                                                       */
/* ------------------------------------------------------------------------- */

static uint8_t tstm_codec_from_stm(uint16_t c)
{
    if (c == 101)          return TAI_AUDIO_PCM;
    if (c == 3 || c == 111) return TAI_AUDIO_OPUS;
    return 0;
}

static uint8_t tstm_codec_to_stm(uint8_t c)
{
    if (c == TAI_AUDIO_PCM)  return 101;
    if (c == TAI_AUDIO_OPUS) return TUYA_STM_OPUS_CODEC_TYPE;
    return c;
}

/* 下行流标记合成:START/MIDDLE/END。
 * 新事件判定按两条线索取或:event_id 变化(库若回填),或上一轮已经 fin
 * 结束(down_open=0)——后者兜底,不依赖库是否在下行包里带 event_id。 */
static uint8_t tstm_down_flag(struct tai_ctx *ctx, const char *eid, int fin)
{
    int is_new;

    if (eid && eid[0] && strcmp(eid, ctx->down_event_id) != 0) {
        strncpy(ctx->down_event_id, eid, sizeof(ctx->down_event_id) - 1);
        ctx->down_event_id[sizeof(ctx->down_event_id) - 1] = '\0';
    }
    is_new = !ctx->down_open;
    if (fin) {
        ctx->down_open = 0;
        return TAI_STREAM_END;
    }
    ctx->down_open = 1;
    return is_new ? TAI_STREAM_START : TAI_STREAM_MIDDLE;
}

/* newlib 无 memmem,自己写一个只读子串探测 */
static int tstm_payload_contains(const uint8_t *hay, uint32_t hay_len,
                                 const char *needle, uint32_t needle_len)
{
    uint32_t i;

    if (!hay || hay_len < needle_len || !needle_len) {
        return 0;
    }
    for (i = 0; i + needle_len <= hay_len; i++) {
        if (hay[i] == (uint8_t)needle[0] &&
            memcmp(hay + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 事件(一轮回复)结束:合成 TAI_EVT_END,与 tai TCP 版行为对齐 */
static void tstm_emit_turn_end(struct tai_ctx *ctx, const char *eid)
{
    tai_event_msg_t m;

    if (!ctx->cfg.on_event) {
        return;
    }
    memset(&m, 0, sizeof(m));
    m.event_type = TAI_EVT_END;
    m.event_id = (eid && eid[0]) ? eid : "";
    ctx->cfg.on_event((tai_ctx_t *)ctx, &m, ctx->cfg.user_data);
}

static void tstm_on_data_recv(stm_open_session_t *session,
                              stm_open_data_t *d, int8_t fin, void *user)
{
    struct tai_ctx *ctx = (struct tai_ctx *)user;

    (void)session;
    if (!ctx || !d) {
        return;
    }

    switch (d->data_type) {
    case STM_DATA_TYPE_AUDIO: {
        tai_audio_msg_t m;
        const char *eid = (d->event_id && d->event_id[0]) ? d->event_id : "";

        memset(&m, 0, sizeof(m));
        m.data = d->payload;
        m.len = d->payload_length;
        m.stream_flag = tstm_down_flag(ctx, eid, fin);
        m.data_id = TAI_DATA_ID_AUDIO_DOWN;
        m.codec = tstm_codec_from_stm(d->audio_params.codec_type);
        m.sample_rate = d->audio_params.sample_rate;
        m.frame_duration = d->audio_params.frame_duration;
        m.event_id = eid;
        m.timestamp_ms = d->timestamp;
        if (ctx->cfg.on_audio) {
            ctx->cfg.on_audio((tai_ctx_t *)ctx, &m, ctx->cfg.user_data);
        }
        if (fin) {
            tstm_emit_turn_end(ctx, eid);
        }
        break;
    }

    case STM_DATA_TYPE_TEXT: {
        tai_text_msg_t m;
        const char *eid = (d->event_id && d->event_id[0]) ? d->event_id : "";

        memset(&m, 0, sizeof(m));
        m.text = (const char *)d->payload;
        m.len = d->payload_length;
        m.stream_flag = tstm_down_flag(ctx, eid, fin);
        m.data_id = TAI_DATA_ID_TEXT_DOWN;
        m.event_id = eid;
        if (ctx->cfg.on_text) {
            ctx->cfg.on_text((tai_ctx_t *)ctx, &m, ctx->cfg.user_data);
        }
        if (fin) {
            tstm_emit_turn_end(ctx, eid);
        }
        break;
    }

    case STM_DATA_TYPE_IMAGE: {
        tai_image_msg_t m;
        const char *eid = (d->event_id && d->event_id[0]) ? d->event_id : "";

        memset(&m, 0, sizeof(m));
        m.data = d->payload;
        m.len = d->payload_length;
        m.stream_flag = tstm_down_flag(ctx, eid, fin);
        m.format = d->image_params.format;
        m.width = d->image_params.width;
        m.height = d->image_params.height;
        m.event_id = eid;
        m.timestamp_ms = d->timestamp;
        if (ctx->cfg.on_image) {
            ctx->cfg.on_image((tai_ctx_t *)ctx, &m, ctx->cfg.user_data);
        }
        if (fin) {
            tstm_emit_turn_end(ctx, eid);
        }
        break;
    }

    case STM_DATA_TYPE_CMD: {
        tai_event_msg_t m;
        int is_break = (d->cmd_params.cmd_type == STM_CMD_TYPE_BREAK);

        memset(&m, 0, sizeof(m));
        m.event_id = "";
        if (is_break) {
            m.event_type = TAI_EVT_CHAT_BREAK;
        } else if (d->payload && d->payload_length > 2 &&
                   d->payload[0] == '{' &&
                   tstm_payload_contains(d->payload, d->payload_length, "jsonrpc", 7)) {
            /* MCP 命令。★打出 cmd_type:上行回应该用哪个 instruction type,
             * 云端下发用的类型号就是权威答案(README 待确认#4/#5;2026-08-31
             * 实测:发完 type=1000 的回应后云端全沉默,疑似类型号猜错)。*/
            printf("[TSTM] mcp downlink cmd_type=%u len=%u\r\n",
                   (unsigned)d->cmd_params.cmd_type, (unsigned)d->payload_length);
            m.event_type = TAI_EVT_MCP_CMD;
            m.data = d->payload;
            m.len = d->payload_length;
        } else {
            printf("[TSTM] unknown cmd(type=%u len=%u)\r\n",
                   (unsigned)d->cmd_params.cmd_type,
                   (unsigned)d->payload_length);
            break;
        }
        if (ctx->cfg.on_event) {
            ctx->cfg.on_event((tai_ctx_t *)ctx, &m, ctx->cfg.user_data);
        }
        break;
    }

    default:
        printf("[TSTM] recv type=%u fin=%d len=%u (ignored)\r\n",
               (unsigned)d->data_type, (int)fin,
               (unsigned)d->payload_length);
        break;
    }
}

static void tstm_on_state(stm_open_session_t *session, uint16_t state, void *user)
{
    struct tai_ctx *ctx = (struct tai_ctx *)user;

    (void)session;
    if (!ctx) {
        return;
    }
    if (state == 1) {                /* STM_SESSION_STATE_NEW:会话就绪 */
        ctx->ready = 1;
        printf("[TSTM] session ready\r\n");
        return;
    }
    if (state == 2) {                /* STM_SESSION_STATE_CLOSE */
        ctx->ready = 0;
        ctx->dead = 1;
        if (ctx->closing) {
            return;                  /* 本侧主动关闭,不算异常断连 */
        }
        if (ctx->cfg.on_disconnect) {
            tai_disconnect_msg_t m;
            memset(&m, 0, sizeof(m));
            m.reason = TAI_DISCONNECT_TRANSPORT;
            m.detail = TAI_TRANSPORT_NET_ERROR;
            strncpy(m.session_id, ctx->session_id, sizeof(m.session_id) - 1);
            ctx->cfg.on_disconnect((tai_ctx_t *)ctx, &m, ctx->cfg.user_data);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 上行                                                                       */
/* ------------------------------------------------------------------------- */

static int tstm_send_data(struct tai_ctx *ctx, stm_open_data_t *d, int8_t fin)
{
    stm_ret r;

    if (!ctx->session) {
        return TAI_ERR_NET;
    }
    r = stm_open_session_send(ctx->session, d, fin);
    if (r != STM_OK) {
        printf("[TSTM] send type=%u fin=%d err=%d\r\n",
               (unsigned)d->data_type, (int)fin, (int)r);
        return tstm_map_err(r);
    }
    return TAI_OK;
}

#if TUYA_STM_PRIVATE_CMD_ENABLE
/*
 * 私有底层指令通道:上行打断(instruction type 4,与下行 break 指令对称)
 * 与 MCP 回应(type 1000,JSON-RPC 作载荷)。公开 API stm_open_session_send
 * 只接受数据类型 2..6,发不了指令,故借底层 stm_session_send。
 * sid 从不透明会话结构的固定偏移读取,发送前校验 sid.id 与本侧
 * session_id 一致,库结构变化时只会放弃发送,不会写坏内存。
 */
static int tstm_send_instruction(struct tai_ctx *ctx, uint16_t type,
                                 const uint8_t *payload, uint32_t len)
{
    stm_sid_t *sid;
    stm_instruction_t ins;
    stm_ret r;

    if (!ctx->session || !ctx->ready) {
        return TAI_ERR_NET;
    }
    sid = (stm_sid_t *)((char *)ctx->session + TSTM_SESSION_SID_OFFSET);
    if (strncmp(sid->id, ctx->session_id, sizeof(sid->id)) != 0) {
        printf("[TSTM] sid layout mismatch, skip instr %u\r\n", (unsigned)type);
        return TAI_ERR_PROTO;
    }
    memset(&ins, 0, sizeof(ins));
    ins.type = type;
    ins.payload = (uint8_t *)payload;
    ins.payload_length = len;
    r = stm_session_send(sid, &ins);
    if (r != STM_OK) {
        printf("[TSTM] instr %u err=%d\r\n", (unsigned)type, (int)r);
        return tstm_map_err(r);
    }
    return TAI_OK;
}
#else
static int tstm_send_instruction(struct tai_ctx *ctx, uint16_t type,
                                 const uint8_t *payload, uint32_t len)
{
    (void)ctx; (void)payload; (void)len;
    printf("[TSTM] instr %u disabled\r\n", (unsigned)type);
    return TAI_ERR_PROTO;
}
#endif /* TUYA_STM_PRIVATE_CMD_ENABLE */

/* ------------------------------------------------------------------------- */
/* 生命周期(tai_* 对齐)                                                     */
/* ------------------------------------------------------------------------- */

tai_ctx_t *tstm_ctx_init(void *mem, const tai_config_t *cfg)
{
    struct tai_ctx *ctx = (struct tai_ctx *)mem;

    if (!ctx || !cfg || !cfg->pal || !cfg->local_key) {
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;

    tstm_wrap_attr(ctx->sa_json, sizeof(ctx->sa_json),
                   "sessionAttributes", cfg->session_attrs_json);
    tstm_wrap_attr(ctx->ca_json, sizeof(ctx->ca_json),
                   "chatAttributes", cfg->event_user_data_json);
    tstm_gen_id(ctx->session_id, sizeof(ctx->session_id), "vcd-s-", (tai_ctx_t *)ctx);
    return (tai_ctx_t *)ctx;
}

void tstm_ctx_deinit(tai_ctx_t *ctx)
{
    if (ctx) {
        memset(ctx, 0, tstm_ctx_size());
    }
}

int tstm_connect(tai_ctx_t *ctx)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;
    stm_open_session_config_t sc;
    stm_open_session_t *s;
    uint32_t budget, waited;

    if (!c) {
        return TAI_ERR_ARGS;
    }
    if (c->session) {
        return TAI_OK;
    }
    if (!s_session_token[0]) {
        printf("[TSTM] no session token bound\r\n");
        return TAI_ERR_ARGS;
    }

    tstm_global_init();
    if (!s_stm_inited) {
        return TAI_ERR_NET;
    }

    c->ready = 0;
    c->dead = 0;
    c->closing = 0;
    c->down_event_id[0] = '\0';
    c->down_open = 0;

    memset(&sc, 0, sizeof(sc));
    sc.client_type = c->cfg.client_type ? c->cfg.client_type
                                        : (uint8_t)STM_CLIENT_TYPE_DEVICE;
    sc.session_token = s_session_token;
    sc.session_id = c->session_id;
    sc.encrypt_key = (char *)c->cfg.local_key;
    sc.on_state = tstm_on_state;
    sc.on_data_recv = tstm_on_data_recv;
    sc.app_data = c->sa_json[0] ? c->sa_json : NULL;
    sc.user_data = c;
    sc.max_fragment_size = 0;       /* 0=库默认分片 */

    /* 注意:库内部 UDP/TCP 竞速建连超时 30s,此调用可能阻塞较久 */
    s = stm_open_session_create(&sc);
    if (!s) {
        printf("[TSTM] session create failed\r\n");
        return TAI_ERR_NET;
    }
    c->session = s;

    budget = c->cfg.connect_timeout_ms ? c->cfg.connect_timeout_ms : 5000;
    waited = 0;
    while (!c->ready && !c->dead && waited < budget) {
        os_time_dly(1);             /* 10ms */
        waited += 10;
    }
    if (c->dead || !c->ready) {
        printf("[TSTM] session not ready after %ums (dead=%d)\r\n",
               (unsigned)waited, c->dead);
        tstm_disconnect(ctx);
        return TAI_ERR_NET;
    }
    printf("[TSTM] connected (session %s)\r\n", c->session_id);
    return TAI_OK;
}

void tstm_disconnect(tai_ctx_t *ctx)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;
    stm_open_session_t *s;

    if (!c) {
        return;
    }
    c->closing = 1;
    c->audio_open = 0;
    c->audio_started = 0;
    s = c->session;
    c->session = NULL;
    c->ready = 0;
    if (s) {
        os_time_dly(1);             /* 留出在途回调退出窗口 */
        stm_open_session_close(s);
    }
}

void tstm_request_disconnect(tai_ctx_t *ctx)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;
    if (c) {
        c->dead = 1;                /* 仅置位;由持有线程调 tstm_disconnect */
    }
}

/* ------------------------------------------------------------------------- */
/* 发送(tai_* 对齐)                                                         */
/* ------------------------------------------------------------------------- */

int tstm_send_text(tai_ctx_t *ctx, const char *text, size_t len)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;
    stm_open_data_t d;
    char eid[STM_EVENT_ID_MAX_LEN + 1];

    if (!c || !text) {
        return TAI_ERR_ARGS;
    }
    tstm_gen_id(eid, sizeof(eid), "vcd-e-", ctx);

    memset(&d, 0, sizeof(d));
    d.data_type = STM_DATA_TYPE_TEXT;
    d.event_id = eid;               /* 一次性事件:首包带 id + fin=1 */
    d.payload = (uint8_t *)text;
    d.payload_length = (uint32_t)len;
    d.app_data = c->ca_json[0] ? c->ca_json : NULL;
    return tstm_send_data(c, &d, 1);
}

#if TUYA_STM_CODEC_ALTERNATE
static uint8_t s_codec_turn;   /* codec 轮换轮次:奇=3 偶=111(见配置块注释) */
#endif

int tstm_send_audio_start(tai_ctx_t *ctx, uint8_t codec, uint8_t channels,
                          uint8_t bit_depth, uint32_t sample_rate)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;

    if (!c) {
        return TAI_ERR_ARGS;
    }
    tstm_gen_id(c->cur_event_id, sizeof(c->cur_event_id), "vcd-e-", ctx);
    c->a_codec = tstm_codec_to_stm(codec);
#if TUYA_STM_CODEC_ALTERNATE
    if (codec == TAI_AUDIO_OPUS) {
        c->a_codec = (++s_codec_turn & 1u) ? TUYA_STM_OPUS_CODEC_TYPE
                                           : TUYA_STM_OPUS_CODEC_TYPE_ALT;
    }
#endif
    c->a_channels = channels;
    c->a_bit_depth = bit_depth;
    c->a_sample_rate = sample_rate;
    /* PCM 全 0 已实测可用(自描述格式,别动);仅帧式编码补齐描述参数。
     * 用入参 codec(应用层枚举)判断,而非映射后的 a_codec。 */
    c->a_bitrate = 0;
    c->a_frame_duration = 0;
    c->a_frame_size = 0;
    if (codec == TAI_AUDIO_OPUS) {
        /* 对齐本地 libopus 配置:16k/mono/CBR 16kbps/40ms → 80B/帧 */
        c->a_bitrate = 16000;
        c->a_frame_duration = 40;
        c->a_frame_size = 80;
    }
    c->audio_open = 1;
    c->audio_started = 0;           /* 音频参数随首个实际发送包下发 */
    c->audio_pending_len = 0;       /* 最新一帧留待下一帧/fin 决定是否为末包 */
    /* 每轮必打:上行 codec 是云端 ASR 能否解码的关键参数(实验期靠它对照轮次) */
    printf("[TSTM] audio start codec=%u ch=%u rate=%u br=%u fd=%u fs=%u\r\n",
           c->a_codec, channels, sample_rate,
           (unsigned)c->a_bitrate, (unsigned)c->a_frame_duration,
           (unsigned)c->a_frame_size);
    return TAI_OK;
}

int tstm_send_audio_chunk(tai_ctx_t *ctx, const uint8_t *data, size_t len)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;
    stm_open_data_t d;
    int rc;

    if (!c || !data || !len || len > sizeof(c->audio_pending)) {
        return TAI_ERR_ARGS;
    }
    if (!c->audio_open) {
        return TAI_ERR_PROTO;
    }

    /* 暂存首帧；后续每来一帧先发送上一帧(fin=0)，从而让 audio_end 能把
     * 最后真实 payload 作为 fin=1 末包。官方 OPEN SDK 文档明确要求该形态。 */
    if (!c->audio_pending_len) {
        memcpy(c->audio_pending, data, len);
        c->audio_pending_len = (uint32_t)len;
        return TAI_OK;
    }

    memset(&d, 0, sizeof(d));
    d.data_type = STM_DATA_TYPE_AUDIO;
    if (!c->audio_started) {
        d.event_id = c->cur_event_id;
        d.audio_params.codec_type = c->a_codec;
        d.audio_params.sample_rate = c->a_sample_rate;
        d.audio_params.channels = c->a_channels;
        d.audio_params.bit_depth = c->a_bit_depth;
        d.audio_params.bitrate = c->a_bitrate;
        d.audio_params.frame_duration = c->a_frame_duration;
        d.audio_params.frame_size = c->a_frame_size;
        d.app_data = c->ca_json[0] ? c->ca_json : NULL;
    }
    d.payload = c->audio_pending;
    d.payload_length = c->audio_pending_len;
    rc = tstm_send_data(c, &d, 0);
    if (rc == TAI_OK) {
        c->audio_started = 1;
        memcpy(c->audio_pending, data, len);
        c->audio_pending_len = (uint32_t)len;
    }
    return rc;
}

int tstm_send_audio_end(tai_ctx_t *ctx)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;
    stm_open_data_t d;
    int rc;

    if (!c || !c->audio_open) {
        return TAI_ERR_ARGS;
    }

    memset(&d, 0, sizeof(d));
    d.data_type = STM_DATA_TYPE_AUDIO;
    if (!c->audio_started) {
        d.event_id = c->cur_event_id;
        d.audio_params.codec_type = c->a_codec;
        d.audio_params.sample_rate = c->a_sample_rate;
        d.audio_params.channels = c->a_channels;
        d.audio_params.bit_depth = c->a_bit_depth;
        d.audio_params.bitrate = c->a_bitrate;
        d.audio_params.frame_duration = c->a_frame_duration;
        d.audio_params.frame_size = c->a_frame_size;
        d.app_data = c->ca_json[0] ? c->ca_json : NULL;
    }
    /* 官方示例:末包必须携带最后一段真实音频 payload + fin=1(文档要求的
     * 事件形态;与 codec 问题无关,但保持符合规范)。 */
    d.payload = c->audio_pending_len ? c->audio_pending : NULL;
    d.payload_length = c->audio_pending_len;
    rc = tstm_send_data(c, &d, 1);
    c->audio_open = 0;
    c->audio_started = 0;
    c->audio_pending_len = 0;
    return rc;
}

int tstm_send_image(tai_ctx_t *ctx, const uint8_t *data, size_t len,
                    uint8_t format, uint16_t width, uint16_t height)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;
    stm_open_data_t d;
    char eid[STM_EVENT_ID_MAX_LEN + 1];

    if (!c || !data || !len) {
        return TAI_ERR_ARGS;
    }
    tstm_gen_id(eid, sizeof(eid), "vcd-e-", ctx);

    memset(&d, 0, sizeof(d));
    d.data_type = STM_DATA_TYPE_IMAGE;
    d.event_id = eid;
    d.image_params.payload_type = 0;   /* raw */
    d.image_params.format = format;    /* 1=jpeg 2=png,与 tai 同编号 */
    d.image_params.width = width;
    d.image_params.height = height;
    d.payload = (uint8_t *)data;
    d.payload_length = (uint32_t)len;
    return tstm_send_data(c, &d, 1);
}

int tstm_send_image_with_text(tai_ctx_t *ctx, const char *text, size_t text_len,
                              const uint8_t *img_data, size_t img_len,
                              uint8_t format, uint16_t width, uint16_t height)
{
    int rc = tstm_send_image(ctx, img_data, img_len, format, width, height);
    if (rc != TAI_OK) {
        return rc;
    }
    return tstm_send_text(ctx, text, text_len);
}

int tstm_chat_break(tai_ctx_t *ctx)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;

    if (!c) {
        return TAI_ERR_ARGS;
    }
    if (c->audio_open) {
        tstm_send_audio_end(ctx);   /* 先收尾在途上行事件 */
    }
    /* instruction type 4 = break,与下行 break 指令(cmd_type=BREAK)对称 */
    return tstm_send_instruction(c, 4, NULL, 0);
}

/* ★只能在 demo 任务上下文调用(tuya_agentic_demo.c 的 mcp_resp_pump)。
 * 本函数会进库的发送路径;若从 on_data_recv 回调(库引擎线程)里同步调,
 * 会与 demo 任务正在进行的音频上行在库内会话锁上互等卡死(2026-08-31
 * 第五轮实测,详见 demo 侧 mcp 回应延迟发送注释)。 */
int tstm_send_mcp_response(tai_ctx_t *ctx, const char *json_rpc_response)
{
    struct tai_ctx *c = (struct tai_ctx *)ctx;

    if (!c || !json_rpc_response) {
        return TAI_ERR_ARGS;
    }
#if TUYA_STM_MCP_VIA_TEXT
    /* 轮 B(2026-08-31):公开 TEXT 数据事件载 JSON-RPC。回应体与 TCP 模式
     * 完全相同(TCP 下云端接受),变量只剩通道。注意 TEXT 事件在会话语义上
     * 也可能是"用户文本输入"——若云端把 JSON 当用户输入喂给 agent,出现任何
     * 下行(哪怕答非所问的 TTS)都证明上下行管道通,再回头修语义。 */
    {
        stm_open_data_t d;
        char eid[STM_EVENT_ID_MAX_LEN + 1];

        tstm_gen_id(eid, sizeof(eid), "vcd-m-", ctx);
        memset(&d, 0, sizeof(d));
        d.data_type = STM_DATA_TYPE_TEXT;
        d.event_id = eid;
        d.payload = (uint8_t *)json_rpc_response;
        d.payload_length = (uint32_t)strlen(json_rpc_response);
        return tstm_send_data(c, &d, 1);
    }
#else
    /* 轮 A(对照):私有指令 type=TUYA_STM_MCP_INSTR_TYPE,已实测云端零反应 */
    return tstm_send_instruction(c, TUYA_STM_MCP_INSTR_TYPE,
                                 (const uint8_t *)json_rpc_response,
                                 (uint32_t)strlen(json_rpc_response));
#endif
}

#endif /* CONFIG_TUYA_AGENTIC_ENABLE && TUYA_TRANSPORT_STM_ENABLE */
