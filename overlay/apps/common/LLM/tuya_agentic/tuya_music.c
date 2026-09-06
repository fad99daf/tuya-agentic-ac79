/*
 * tuya_music.c -- 涂鸦音乐技能(Music Skill)下行解析(设备端)。
 *
 * 移植自 agentic-kit/examples/posix/ai/rtc-tcp-client 的 music_play_demo.c
 * + demo_text.h + demo_json.h,针对 AC79 嵌入式环境裁剪:
 *   - 重组缓冲静态固定 4KB(POSIX 版 malloc 可到 256KB);实测音乐 SKILL
 *     JSON 一首 ~1KB,4KB 足够,超了整流丢弃并计数。
 *   - JSON 解析全部就地(drill 时把跨度结尾写 '\0'),零动态分配。
 *   - NLG 逐片打印不做(demo 主循环已有原始打印,保持不变)。
 *   - seq 缺口检查不做:SDK 丢零长帧造成的缺口拼出来仍是对的文档,
 *     交错拼接的混合物会在 JSON 解析处被拒(与 POSIX 版默认策略等效)。
 *
 * 三个关键约束(照搬参考实现的教训,勿"简化"掉):
 *   1. msg->text 无 '\0' 结尾:先按 len 拷入自己的缓冲再 strstr/解析。
 *   2. code 字段必须在信封的 data 对象内取:外层常见
 *      {"code":0,"msg":"ok","data":{"code":"music",...}},
 *      "取全文档第一个 code" 会拿到状态码 0 而静默丢掉音乐响应。
 *   3. 一轮文本流 = ASR/SKILL/NLG 多个完整信封首尾拼接,全文档第一个
 *      "data" 属于 ASR 信封——按 "data" 钻取永远轮不到音乐信封(实测踩坑,
 *      2026-08-28)。判音乐只能靠字面量 "code":"music" 快速通道,钻取从
 *      只在音乐信封出现的 "general" 键进。
 */
#include <stdio.h>
#include <string.h>

#include "tuya_ai.h"        /* TAI_STREAM_START/MIDDLE/END/ONE_SHOT */
#include "tuya_music.h"

/* ===== 极简 JSON 读取(全部操作 NUL 结尾缓冲;跨自 agentic-kit demo_json.h)===== */

static const char *json_str_end(const char *p)
{
    while (*p) {
        if (*p == '\\') {
            if (!p[1]) return NULL;
            p += 2;
            continue;
        }
        if (*p == '"') return p;
        p++;
    }
    return NULL;
}

/* 起始在 p 的平衡 {..}/[..] 跨度结尾(闭合符后一字节),字符串整体跳过 */
static const char *json_span_end(const char *p, char open, char close)
{
    if (!p || *p != open) return NULL;
    int depth = 0;
    while (*p) {
        if (*p == '"') {
            const char *e = json_str_end(p + 1);
            if (!e) return NULL;
            p = e + 1;
            continue;
        }
        if (*p == open) {
            depth++;
        } else if (*p == close && --depth == 0) {
            return p + 1;
        }
        p++;
    }
    return NULL;
}

static const char *json_skip_value(const char *p)
{
    if (!p) return NULL;
    if (*p == '"') {
        const char *e = json_str_end(p + 1);
        return e ? e + 1 : NULL;
    }
    if (*p == '{') return json_span_end(p, '{', '}');
    if (*p == '[') return json_span_end(p, '[', ']');
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return (p > s) ? p : NULL;
}

/* 第一个 "key": 的值指针(字符串字面量整体跳过,key 在值里不会误命中) */
static const char *json_find_value(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = json;
    while (*p) {
        if (*p != '"') { p++; continue; }
        const char *s = p + 1;
        const char *e = json_str_end(s);
        if (!e) return NULL;
        if ((size_t)(e - s) == klen && memcmp(s, key, klen) == 0) {
            const char *v = e + 1;
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
            if (*v == ':') {
                v++;
                while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
                return v;
            }
        }
        p = e + 1;
    }
    return NULL;
}

static int json_hex4(const char *p, unsigned int *out)
{
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int json_utf8_put(char *out, unsigned int cap, unsigned int *n, unsigned int cp)
{
    char tmp[4];
    unsigned int k = 0;
    if (cp < 0x80) {
        tmp[k++] = (char)cp;
    } else if (cp < 0x800) {
        tmp[k++] = (char)(0xC0 | (cp >> 6));
        tmp[k++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        tmp[k++] = (char)(0xE0 | (cp >> 12));
        tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[k++] = (char)(0x80 | (cp & 0x3F));
    } else {
        tmp[k++] = (char)(0xF0 | (cp >> 18));
        tmp[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[k++] = (char)(0x80 | (cp & 0x3F));
    }
    if (*n + k >= cap) return -1;
    memcpy(out + *n, tmp, k);
    *n += k;
    return 0;
}

/* 解码字符串字面量原文(两引号之间)。truncate=0:装不下判失败(URL 用,
 * 半截 URL 比没有更坏);1:保留能装下的前缀(歌名打印用)。*/
static int json_unescape_ex(const char *p, unsigned int len, char *out,
                            unsigned int cap, int truncate)
{
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!p) return len ? -1 : 0;

    const char *end = p + len;
    unsigned int n  = 0;
    int full = 0;

    while (p < end) {
        char lit = 0;
        unsigned int cp = 0;
        int is_cp = 0;

        if (*p != '\\') {
            lit = *p++;
        } else {
            if (++p >= end) goto fail;
            switch (*p) {
            case '"': case '\\': case '/': lit = *p++;  break;
            case 'b': lit = '\b'; p++; break;
            case 'f': lit = '\f'; p++; break;
            case 'n': lit = '\n'; p++; break;
            case 'r': lit = '\r'; p++; break;
            case 't': lit = '\t'; p++; break;
            case 'u':
                if (end - p < 5 || json_hex4(p + 1, &cp) != 0) goto fail;
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 &&
                    p[0] == '\\' && p[1] == 'u') {
                    unsigned int lo = 0;
                    if (json_hex4(p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        p += 6;
                    }
                }
                if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) goto fail;
                is_cp = 1;
                break;
            default:
                goto fail;
            }
        }

        if (full) continue;
        if (is_cp) {
            if (json_utf8_put(out, cap, &n, cp) != 0) {
                if (!truncate) goto fail;
                full = 1;
            }
        } else if (n + 1 >= cap) {
            if (!truncate) goto fail;
            full = 1;
        } else {
            out[n++] = lit;
        }
    }
    out[n] = '\0';
    return (int)n;

fail:
    out[0] = '\0';
    return -1;
}

/* 取 "key" 的字符串值(严格装下):0=成功,-1=缺失/非串/装不下(置空) */
static int json_get_string(const char *json, const char *key, char *out, unsigned int cap)
{
    const char *v = json_find_value(json, key);
    if (!v || *v != '"') { out[0] = '\0'; return -1; }
    const char *e = json_str_end(v + 1);
    if (!e) { out[0] = '\0'; return -1; }
    return json_unescape_ex(v + 1, (unsigned)(e - v - 1), out, cap, 0) < 0 ? -1 : 0;
}

/* 同上但超长截断(仅打印用途:截短的歌名仍有信息量) */
static int json_get_display_string(const char *json, const char *key, char *out, unsigned int cap)
{
    const char *v = json_find_value(json, key);
    if (!v || *v != '"') { out[0] = '\0'; return -1; }
    const char *e = json_str_end(v + 1);
    if (!e) { out[0] = '\0'; return -1; }
    return json_unescape_ex(v + 1, (unsigned)(e - v - 1), out, cap, 1) < 0 ? -1 : 0;
}

/* 就地取 "key" 的 {...}/[...] 跨度:结尾写 '\0' 截断,返回起始指针。
 * ★ 只进不回的钻取序(截断会毁掉跨度之后的兄弟字段),音乐文档
 *   data→general→data→audios→[0] 的钻取顺序满足该约束。*/
static char *json_span_z(char *json, const char *key, char open, char close)
{
    char *v = (char *)json_find_value(json, key);
    if (!v) return NULL;
    char *e = (char *)json_span_end(v, open, close);
    if (!e) return NULL;
    *e = '\0';
    return v;
}

/* 数组内第一个 {...} 元素(同样就地截断) */
static char *json_array_first_z(char *arr)
{
    if (!arr) return NULL;
    while (*arr && *arr != '{') {
        if (*arr == '"') {               /* 数组元素是字符串:跳过 */
            const char *e = json_str_end(arr + 1);
            if (!e) return NULL;
            arr = (char *)e + 1;
            continue;
        }
        arr++;
    }
    char *e = (char *)json_span_end(arr, '{', '}');
    if (!e) return NULL;
    *e = '\0';
    return arr;
}

/* ===== 文本流重组 + 音乐解析 ===== */

#define TUYA_TEXTBUF_MAX  4096   /* 重组缓冲上限:实测音乐 SKILL JSON ~1KB,4KB 留裕量 */

static struct {
    char buf[TUYA_TEXTBUF_MAX];   /* 交付时 NUL 结尾 */
    unsigned int len;
    int done;                     /* buf 已交付过一次(重复 END 不再交付) */
    int dropping;                 /* 当前流已判废:吞掉后续分片直到 END */
    unsigned dropped;             /* 本次会话累计丢流数(诊断) */
} s_tb;

static struct {
    char url[512];
    char name[128];
    char artist[128];
    volatile int pending;         /* 置位前 url/name/artist 已写好 */
} s_music;

void tuya_music_reset(void)
{
    s_tb.len      = 0;
    s_tb.done     = 0;
    s_tb.dropping = 0;
    /* s_tb.dropped 不清:会话累计诊断计数 */
}

/* s_tb.buf 里是一条完整流(多信封拼接):是音乐响应就解析暂存 */
static void tuya_music_try_parse(void)
{
    /* 判据两级(照搬参考实现):
     * 1) 快速通道:带引号字面量,在拼接流里任意位置命中即算;
     * 2) 变体兜底:字面量没中(如冒号后带空格)才取 data 验 code,
     *    只借一刀截断验完就还原,不毁后面的 general 钻取。 */
    int music = (strstr(s_tb.buf, "\"code\":\"music\"") != NULL);
    if (!music && !strstr(s_tb.buf, "music")) {
        return;                    /* 便宜排除:绝大多数是 NLG 闲聊流 */
    }
    if (!music) {
        char *v  = (char *)json_find_value(s_tb.buf, "data");
        int   ok = 0;
        if (v) {
            char *e = (char *)json_span_end(v, '{', '}');
            if (e) {
                char save = *e;
                char code[32];
                *e = '\0';
                ok = (json_get_string(v, "code", code, sizeof(code)) == 0 &&
                      strcmp(code, "music") == 0);
                *e = save;         /* 防外层 {"code":0} 状态码误判 */
            }
        }
        if (!ok) return;
    }

    /* general 只出现在音乐信封里:从这里钻,天然避开 ASR/NLG 信封里
     * 同名 "data" 的干扰(全文档第一个 "data" 属于 ASR 信封) */
    char *general = json_span_z(s_tb.buf, "general", '{', '}'); if (!general) goto bad;
    char *gdata   = json_span_z(general, "data", '{', '}');     if (!gdata) goto bad;
    char *audios  = json_span_z(gdata, "audios", '[', ']');     if (!audios) goto bad;
    char *first   = json_array_first_z(audios);                 if (!first) goto bad;

    {
        char url[512], name[128], artist[128];
        int has_url = (json_find_value(first, "url") != NULL);
        if (has_url) {
            if (json_get_string(first, "url", url, sizeof(url)) != 0) {
                printf("[TUYA-MUSIC] url field malformed or too long (>=%uB)\r\n",
                       (unsigned)sizeof(url));
                goto bad;
            }
            if (url[0] && strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
                printf("[TUYA-MUSIC] reject non-http(s) url: %.64s\r\n", url);
                goto bad;
            }
        }
        json_get_display_string(first, "name",   name,   sizeof(name));
        json_get_display_string(first, "artist", artist, sizeof(artist));

        if (!has_url || !url[0]) {
            /* 音乐帧但无 audios[].url(播放态变化帧):不覆盖已有 pending,
             * 否则会抹掉同轮更早帧给的可播地址(参考实现 music_state 语义) */
            printf("[TUYA-MUSIC] music frame without playable url, ignored\r\n");
            return;
        }

        memcpy(s_music.url, url, sizeof(s_music.url));
        memcpy(s_music.name, name, sizeof(s_music.name));
        memcpy(s_music.artist, artist, sizeof(s_music.artist));
        s_music.pending = 1;      /* 最后置位:读到 1 时其余字段已就绪 */
        printf("[TUYA-MUSIC] >>> %s - %s\r\n[ TUYA-MUSIC] url: %s\r\n",
               s_music.artist[0] ? s_music.artist : "?",
               s_music.name[0] ? s_music.name : "?",
               s_music.url);
    }
    return;

bad:
    printf("[TUYA-MUSIC] music response but parse failed (dropped=%u)\r\n", s_tb.dropped);
}

int tuya_music_text_accum(int stream_flag, const char *text, unsigned int len)
{
    int starts = (stream_flag == TAI_STREAM_START || stream_flag == TAI_STREAM_ONE_SHOT);
    int ends   = (stream_flag == TAI_STREAM_END   || stream_flag == TAI_STREAM_ONE_SHOT);

    if (starts && !s_tb.done && !s_tb.dropping && s_tb.len > 0) {
        printf("[TUYA-MUSIC] new stream started with %uB of previous still buffered: drop\r\n",
               s_tb.len);
        s_tb.dropped++;
    }
    if (starts) {
        /* 判废状态不得跨流存活:dropping 本该由本流的 END 清,但 END 是空文本帧,
         * SDK 不派发给 on_text(见 demo tuya_music_text_flush 注释),只能靠新流
         * START 复位。长故事轮的 NLG 拼流轻松超 4KB 上限,一旦判废又等不到 END,
         * dropping 卡到会话结束——之后每一轮的 music JSON 全被吞,云端明明回了
         * URL 也永远不放歌(2026-09-06 实测:放歌→讲故事→再放歌,歌不播)。*/
        if (s_tb.dropping) {
            printf("[TUYA-MUSIC] new stream after dropped one: recover\r\n");
        }
        s_tb.dropping = 0;
    }
    if (starts || s_tb.done) {
        s_tb.len  = 0;
        s_tb.done = 0;
    }

    if (s_tb.dropping) {
        if (ends) s_tb.dropping = 0;
        return 0;
    }

    if (len > 0) {
        if (!text || s_tb.len + len + 1 > TUYA_TEXTBUF_MAX) {
            printf("[TUYA-MUSIC] stream too large (%uB, cap %uB): drop\r\n",
                   s_tb.len + len, (unsigned)TUYA_TEXTBUF_MAX);
            s_tb.dropped++;
            s_tb.len      = 0;
            s_tb.dropping = !ends;
            return -1;
        }
        memcpy(s_tb.buf + s_tb.len, text, len);
        s_tb.len += len;
    }

    if (!ends || s_tb.len == 0) return 0;

    s_tb.buf[s_tb.len] = '\0';
    s_tb.done = 1;
    tuya_music_try_parse();
    return 1;
}

int tuya_music_text_flush(void)
{
    if (s_tb.done || s_tb.dropping || s_tb.len == 0) return 0;
    s_tb.buf[s_tb.len] = '\0';
    s_tb.done = 1;
    tuya_music_try_parse();
    return 1;
}

int tuya_music_pending(void)            { return s_music.pending; }
const char *tuya_music_get_url(void)    { return s_music.pending ? s_music.url : ""; }
const char *tuya_music_get_name(void)   { return s_music.pending ? s_music.name : ""; }
const char *tuya_music_get_artist(void) { return s_music.pending ? s_music.artist : ""; }
void tuya_music_clear_pending(void)     { s_music.pending = 0; }
