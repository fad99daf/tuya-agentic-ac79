/*
 * app_image.c -- App 图片下发(面板"打印图片")接收链,AC791N 移植。
 *
 * 参考实现:agentic-kit-solution solutions/printer/main/app_image.c(ESP32-S3,
 * 板测收图成功)。事件机/ATOP 报文/COS-S3-Azure 签名/AES 解密规格照搬,宿主
 * 差异集中在三处:
 *   1. 无 NVS:tckey 不落盘,每次开机现场 thing.tc.init(JL VM 区有慢性坏记录
 *      问题,不存关键数据;tckey 幂等,重复 init 云端返回同一把钥匙)。
 *   2. 无 SNTP:开机时 time 轴是上电秒数。云侧实测容忍(激活/ATOP 一直用的
 *      就是这条轴);对象存储签名需要真时区,首次 ATOP 应答信封里的 t
 *      (server_time)落地后补齐纪元基准——见 now_epoch()/epoch_seed_from()。
 *   3. HTTP:分片下载不走 kit 的 http_client_interface(响应缓冲是编译期
 *      定长,放大它会让所有 ATOP 调用陪绑大内存),改用 pal TCP + common/tls
 *      自写精简 GET,单一预分配缓冲,解密在原缓冲内完成。
 *
 * 内存预算:单分片下载缓冲 = MAX_IMAGE_BYTES+80(约 512KB);skill 里
 * maxFc=1 与参考一致,正常只有一块。多分片兜底路径临时再配一块累计缓冲。
 */
/* JL 宿主头最先:asm/cpu.h 里 typedef unsigned char bool,必须赶在
 * 任何 stdbool.h(kit 头/i_client 头会拉)把 bool 宏成 _Bool 之前完成——
 * 与 demo.c "system 头先于 kit 头" 同序。 */
#include "system/os/os_api.h"     /* thread_fork, os_mutex/os_sem, msleep */
#include "system/timer.h"         /* sys_timer_get_ms */

/* 堆余量探针(P0):查询符号来自预编译 system.a 的 mem_heap_ram.c.o
 * (llvm-nm 验证为 T 导出),全树无头文件,在此显式 extern。malloc.h 里的
 * xPortGetFreeHeapSize 只有声明、链接器找不到——勿用(2026-10-20 链接失败定罪)。 */
extern unsigned int get_malloc_remain_heap_size(void);
extern unsigned int get_ram_free_heap_size(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_image.h"
#include "cJSON.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include "iot_client.h"
#include "iot_atop.h"
#include "tls.h"                   /* kit 公共 TLS(pal TCP 之上的 mbedTLS 封装) */

#define TAG_PRINTF  "[APP-IMG] "
#define PRINT_TAG   "[PRINT-DL] "

#define TC_KEY_LEN            16
#define MAX_EVENT_JSON        4096
#define MAX_IMAGE_BYTES       (512 * 1024)
#define DL_BUF_LEN            (MAX_IMAGE_BYTES + 64 + 16)  /* 64B头 + CBC对齐余量 */
#define MQTT_PROTOCOL_FILE_STORAGE_DL 64
/* 上行回执协议号按产品族分岔,移植勿裸搬:打印件族=19(TuyaOpen 源码 PRO_DEV_DA_RESP,
 * 打印件 PID 板测过);云相框族=65(闭源样机+wiki+涂鸦云端 2026-10-27 坐实)——tc 服务按
 * 协议号分发,19 会走打印/透传通道,相框面板等不到回执 60s 判"设备已离线"。 */
#define MQTT_PROTOCOL_DEVICE_RESPONSE 65

#define IMG_Q_DEPTH           4
#define DL_TIMEOUT_MS         25000   /* 单分片读循环总预算(参考 20s,放宽链路抖动) */
#define TLS_CONN_TIMEOUT_MS   15000

/* ------------------------------------------------------------------------- */
/* 简单队列:事件入队(message_callback→worker)与回执出队(worker→保活线程)   */
/* ------------------------------------------------------------------------- */
typedef struct {
    char     *slot[IMG_Q_DEPTH];
    int       head, tail;
    OS_MUTEX  mtx;
    OS_SEM    cnt;
    int       inited;
} img_queue_t;

static int q_init(img_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    if (os_mutex_create(&q->mtx) != 0) return -1;
    if (os_sem_create(&q->cnt, 0) != 0) return -1;
    q->inited = 1;
    return 0;
}

/* 取得所有权;0=成功,-1=队列未就绪(引导线程未跑完 q_init),-2=满。
 * 三种情况 item 均已在本函数内释放,调用侧不得再 free。 */
static int q_send(img_queue_t *q, char *item)
{
    if (!item) return -1;
    if (!q->inited) {
        free(item);
        return -1;
    }
    os_mutex_pend(&q->mtx, 0);
    int next = (q->head + 1) % IMG_Q_DEPTH;
    int full = (next == q->tail);
    if (!full) {
        q->slot[q->head] = item;
        q->head = next;
    }
    os_mutex_post(&q->mtx);
    if (full) {
        free(item);
        return -2;
    }
    os_sem_post(&q->cnt);
    return 0;
}

/* 窥视队头不取走(回执泵用:publish 成功才 drop,失败留待下轮重试)。
 * 单消费者(保活线程)假设下与 q_send 无竞态;ring 状态为准,信号量计数
 * 仅与 ring 大致同步(drop 里的 accept 只在确有计数时消耗,偏差无害)。 */
static char *q_peek(img_queue_t *q)
{
    if (!q->inited) return NULL;
    return (q->tail != q->head) ? q->slot[q->tail] : NULL;
}

static void q_drop_head(img_queue_t *q)
{
    os_mutex_pend(&q->mtx, 0);
    if (q->tail != q->head) {
        q->slot[q->tail] = NULL;
        q->tail = (q->tail + 1) % IMG_Q_DEPTH;
    }
    os_mutex_post(&q->mtx);
    (void)os_sem_accept(&q->cnt);   /* 与 ring 保持计数同步(尽力) */
}

static char *q_recv_block(img_queue_t *q)   /* 阻塞(worker 用) */
{
    os_sem_pend(&q->cnt, 0);
    os_mutex_pend(&q->mtx, 0);
    char *item = NULL;
    if (q->tail != q->head) {
        item = q->slot[q->tail];
        q->tail = (q->tail + 1) % IMG_Q_DEPTH;
    }
    os_mutex_post(&q->mtx);
    return item;
}

/* ------------------------------------------------------------------------- */
/* 时钟:上电秒数轴 + 首个 ATOP 应答 server_time 补齐的纪元基准              */
/* ------------------------------------------------------------------------- */
static volatile unsigned int s_epoch_base;
static volatile unsigned int s_epoch_tick_ms;
static volatile int          s_epoch_ok;

static unsigned int now_epoch(void)
{
    unsigned int ms = sys_timer_get_ms();
    if (s_epoch_ok) return s_epoch_base + (ms - s_epoch_tick_ms) / 1000u;
    return ms / 1000u;
}

static void epoch_seed_from(long server_t)
{
    if (!s_epoch_ok && server_t > 1700000000L) {
        s_epoch_base    = (unsigned int)server_t;
        s_epoch_tick_ms = sys_timer_get_ms();
        s_epoch_ok      = 1;
        printf(TAG_PRINTF "clock seeded from cloud t=%us\r\n", (unsigned int)server_t);
    }
}

/* epoch → UTC 六元组(自实现 civil_from_days,JL newlib 的 gmtime_r 不可靠)。 */
static void epoch_to_utc(unsigned int epoch, int *Y, int *M, int *D,
                         int *h, int *mi, int *sec)
{
    unsigned int days = epoch / 86400u;
    unsigned int rem  = epoch % 86400u;
    *h   = (int)(rem / 3600u);
    *mi  = (int)((rem % 3600u) / 60u);
    *sec = (int)(rem % 60u);

    int z = (int)days + 719468;
    int era = z / 146097;
    int doe = z - era * 146097;                                  /* [0,146096] */
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y   = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           /* [0,365] */
    int mp  = (5 * doy + 2) / 153;                               /* [0,11] */
    int d   = doy - (153 * mp + 2) / 5 + 1;                      /* [1,31] */
    int m   = (mp < 10) ? (mp + 3) : (mp - 9);                   /* [1,12] */
    if (m <= 2) y++;
    *Y = y; *M = m; *D = d;
}

/* ------------------------------------------------------------------------- */
/* 模块状态                                                                  */
/* ------------------------------------------------------------------------- */
typedef struct {
    char provider[16], ak[128], sk[128], bucket[128], endpoint[192];
    char token[768], region[64], path[192];
} storage_config_t;

static iot_client_t        *s_iot;
static const pal_t         *s_pal;
static uint8_t              s_tc_key[TC_KEY_LEN];
static img_queue_t          s_evtq;
static img_queue_t          s_pubq;
static app_image_ready_cb_t s_ready_cb;
static void                *s_ready_ud;
static int                  s_started;
static unsigned             s_downloaded_count;

static int json_string(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= cap) return -1;
    strcpy(out, v->valuestring);
    return 0;
}

static int atop(const char *api, const char *version, const char *data,
                uint32_t t, iot_atop_response_t *resp)
{
    /* t 单值贯通:body 里的 "t" 与验签 URL 的 t 必须同一值(见 tc_key_init)。 */
    iot_atop_request_t req = { .api = api, .version = version, .data = data,
                               .timestamp = t };
    int rc = iot_atop_call(s_iot, &req, resp);
    epoch_seed_from(resp->server_time);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* thing.tc.init:注册 tc 通道拿 tckey(无 NVS,每次开机现场 init)             */
/* ------------------------------------------------------------------------- */
static int tc_key_init(void)
{
    /* skill 声明能力上限:参考实现同款,仅 maxFs 按 AC79 内存预算降到 512KB。
     * 验签要求 body 的 "t" 与 URL 的 t 是同一个值(2026-10-27 云端实测:全等=过,
     * 差 1 秒或开机秒数=SING_VALIDATE_FALED;旧假设"第 2 发起真纪元就能过"
     * 不成立,两把钟系统差 ~1s)。每次尝试采 time(NULL) 一次,单值进 body 与
     * req.timestamp。业务错误也重试:重绑定后云端文件服务 sekey 传播滞后
     * 同样报签错,退避 5s/10s 留窗。 */
    int ok = -1;
    for (int attempt = 1; attempt <= 3; attempt++) {
        uint32_t t = (uint32_t)time(NULL);
        char skill[320];
        snprintf(skill, sizeof(skill),
                 "{\"sdkVer\":\"agentic-kit\",\"resolution\":\"384*384\","
                 "\"maxFs\":%u,\"maxFc\":1,\"checksumAlg\":\"SHA-1\","
                 "\"allowFormats\":[\"jpg\"]}", (unsigned)MAX_IMAGE_BYTES);
        cJSON *body_obj = cJSON_CreateObject();
        if (!body_obj) return -1;
        cJSON_AddStringToObject(body_obj, "skill", skill);
        cJSON_AddNumberToObject(body_obj, "t", (double)t);
        char *body = cJSON_PrintUnformatted(body_obj);
        cJSON_Delete(body_obj);
        if (!body) return -1;

        iot_atop_response_t resp = {0};
        int rc = atop("thing.tc.init", "2.0", body, t, &resp);
        free(body);
        if (rc != OPRT_OK || !resp.result) {
            printf(PRINT_TAG "thing.tc.init failed: %d (%s) attempt %d/3\r\n", rc,
                   resp.error_code[0] ? resp.error_code : "no-result", attempt);
            iot_atop_response_free(s_iot, &resp);
            if (attempt < 3) msleep(attempt == 1 ? 5000 : 10000);
            continue;
        }
        cJSON *result = cJSON_Parse(resp.result);
        iot_atop_response_free(s_iot, &resp);
        if (!result) continue;
        char encoded[64];
        int have = json_string(result, "tckey", encoded, sizeof(encoded));
        cJSON_Delete(result);
        if (have != 0) continue;
        size_t decoded = 0;
        if (mbedtls_base64_decode(s_tc_key, sizeof(s_tc_key), &decoded,
                                  (const unsigned char *)encoded, strlen(encoded)) != 0
            || decoded != TC_KEY_LEN) {
            printf(PRINT_TAG "thing.tc.init returned invalid key\r\n");
            continue;
        }
        ok = 0;
        break;
    }
    if (ok == 0) printf(PRINT_TAG "image-transfer key initialized\r\n");
    return ok;
}

/* thing.tc.config.get:存储凭证(分片级时效,每片现取,与参考一致)。 */
static int get_storage_config(storage_config_t *cfg)
{
    uint32_t t = (uint32_t)time(NULL);
    char body[48];
    snprintf(body, sizeof(body), "{\"action\":1,\"t\":%u}", (unsigned)t);
    iot_atop_response_t resp = {0};
    int rc = atop("thing.tc.config.get", "2.0", body, t, &resp);
    if (rc != OPRT_OK || !resp.result) {
        printf(PRINT_TAG "thing.tc.config.get failed: %d (%s)\r\n", rc,
               resp.error_code[0] ? resp.error_code : "no-result");
        iot_atop_response_free(s_iot, &resp);
        return -1;
    }
    cJSON *root = cJSON_Parse(resp.result);
    iot_atop_response_free(s_iot, &resp);
    if (!root) return -1;
    int ok = json_string(root, "provider", cfg->provider, sizeof(cfg->provider))
        || json_string(root, "ak",      cfg->ak,      sizeof(cfg->ak))
        || json_string(root, "sk",      cfg->sk,      sizeof(cfg->sk))
        || json_string(root, "bucket",  cfg->bucket,  sizeof(cfg->bucket))
        || json_string(root, "endpoint",cfg->endpoint,sizeof(cfg->endpoint))
        || json_string(root, "token",   cfg->token,   sizeof(cfg->token))
        || json_string(root, "region",  cfg->region,  sizeof(cfg->region))
        || json_string(root, "path",    cfg->path,    sizeof(cfg->path));
    cJSON_Delete(root);
    if (ok) printf(PRINT_TAG "incomplete storage config\r\n");
    return ok ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* 对象存储签名(COS q-sign / S3 AWS4 / Azure SAS-in-URI)——规格照参考实现     */
/* ------------------------------------------------------------------------- */
static void hex_encode(const unsigned char *src, size_t len, char *dst)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2] = hex[src[i] >> 4];
        dst[i * 2 + 1] = hex[src[i] & 15];
    }
    dst[len * 2] = 0;
}

static int s3_signature(storage_config_t *c, const char *object, char *date,
                        size_t date_cap, char *auth, size_t auth_cap)
{
    unsigned int now = now_epoch();
    if (now < 1700000000u) return -1;   /* 纪元基准未落地,签出来也是垃圾 */
    int Y, Mo, D, h, Mi, S;
    epoch_to_utc(now, &Y, &Mo, &D, &h, &Mi, &S);
    snprintf(date, date_cap, "%04d%02d%02dT%02d%02d%02dZ", Y, Mo, D, h, Mi, S);
    char day[9];
    memcpy(day, date, 8);
    day[8] = 0;

    char canonical_headers[1600], canonical[2400], string_to_sign[512];
    snprintf(canonical_headers, sizeof(canonical_headers),
             "host:%s.%s\nx-amz-content-sha256:UNSIGNED-PAYLOAD\nx-amz-date:%s\n"
             "x-amz-security-token:%s\n",
             c->bucket, c->endpoint, date, c->token);
    snprintf(canonical, sizeof(canonical),
             "GET\n%s\n\n%s\nhost;x-amz-content-sha256;x-amz-date;x-amz-security-token\n"
             "UNSIGNED-PAYLOAD",
             object, canonical_headers);

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    unsigned char hash[32], key[32], k1[32], k2[32], k3[32], k4[32];
    char hash_hex[65];
    if (!md || mbedtls_md(md, (const unsigned char *)canonical, strlen(canonical), hash)) return -1;
    hex_encode(hash, 32, hash_hex);
    snprintf(string_to_sign, sizeof(string_to_sign),
             "AWS4-HMAC-SHA256\n%s\n%s/%s/s3/aws4_request\n%s",
             date, day, c->region, hash_hex);
    char secret[132];
    snprintf(secret, sizeof(secret), "AWS4%s", c->sk);
    if (mbedtls_md_hmac(md, (const unsigned char *)secret, strlen(secret), (const unsigned char *)day, 8, k1)
        || mbedtls_md_hmac(md, k1, 32, (const unsigned char *)c->region, strlen(c->region), k2)
        || mbedtls_md_hmac(md, k2, 32, (const unsigned char *)"s3", 2, k3)
        || mbedtls_md_hmac(md, k3, 32, (const unsigned char *)"aws4_request", 12, k4)
        || mbedtls_md_hmac(md, k4, 32, (const unsigned char *)string_to_sign, strlen(string_to_sign), key)) return -1;
    char sig[65];
    hex_encode(key, 32, sig);
    int n = snprintf(auth, auth_cap,
                     "AWS4-HMAC-SHA256 Credential=%s/%s/%s/s3/aws4_request,"
                     "SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-security-token,"
                     "Signature=%s",
                     c->ak, day, c->region, sig);
    return (n > 0 && (size_t)n < auth_cap) ? 0 : -1;
}

static int cos_authorization(storage_config_t *c, const char *object, char *auth, size_t cap)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    unsigned int now = now_epoch();
    if (!md || now < 1700000000u) return -1;
    char times[64], to_sign[512];
    snprintf(times, sizeof(times), "%u;%u", now, now + 300u);
    unsigned char sign_key[20], hash[20], sig[20];
    char sign_key_hex[41], hash_hex[41], sig_hex[41];
    int n = snprintf(to_sign, sizeof(to_sign), "get\n%s\n\n\n", object);
    if (n < 0 || (size_t)n >= sizeof(to_sign)
        || mbedtls_md_hmac(md, (const unsigned char *)c->sk, strlen(c->sk), (const unsigned char *)times, strlen(times), sign_key)) return -1;
    hex_encode(sign_key, 20, sign_key_hex);
    if (mbedtls_md(md, (const unsigned char *)to_sign, strlen(to_sign), hash)) return -1;
    hex_encode(hash, 20, hash_hex);
    n = snprintf(to_sign, sizeof(to_sign), "sha1\n%s\n%s\n", times, hash_hex);
    if (n < 0 || (size_t)n >= sizeof(to_sign)
        || mbedtls_md_hmac(md, (const unsigned char *)sign_key_hex, 40, (const unsigned char *)to_sign, strlen(to_sign), sig)) return -1;
    hex_encode(sig, 20, sig_hex);
    n = snprintf(auth, cap,
                 "q-sign-algorithm=sha1&q-ak=%s&q-sign-time=%s&q-key-time=%s"
                 "&q-header-list=&q-url-param-list=&q-signature=%s",
                 c->ak, times, times, sig_hex);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* provider → host + uri(下载请求两要素)。返回 0 成功。 */
static int build_request(const storage_config_t *c, const char *filename,
                         char *host, size_t hcap, char *uri, size_t ucap)
{
    int n;
    if (!strcmp(c->provider, "COS") || !strcmp(c->provider, "cos")
        || !strcmp(c->provider, "S3") || !strcmp(c->provider, "s3")) {
        n = snprintf(host, hcap, "%s.%s", c->bucket, c->endpoint);
        if (n < 0 || (size_t)n >= hcap) return -1;
        n = snprintf(uri, ucap, "%s/%s", c->path, filename);
    } else if (!strcmp(c->provider, "AZURE") || !strcmp(c->provider, "azure")
               || !strcmp(c->provider, "AzureBlob")) {
        n = snprintf(host, hcap, "%s%s", c->ak, c->endpoint);
        if (n < 0 || (size_t)n >= hcap) return -1;
        /* SAS 令牌("?sv=...")作查询串拼在 uri 尾(参考实现同款 URL 形态)。 */
        n = snprintf(uri, ucap, "%s%s/%s%s", c->bucket, c->path, filename, c->token);
    } else {
        return -1;
    }
    return (n > 0 && (size_t)n < ucap) ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* 分片下载:精简 HTTPS GET(pal TCP + kit TLS),密文解密在原缓冲内完成        */
/* ------------------------------------------------------------------------- */
/* 在 [from,to) 里找 "\r\n\r\n",返回其起始下标,没有返回 -1。 */
static int find_header_end(const uint8_t *buf, int from, size_t to)
{
    if ((size_t)from + 4 > to) return -1;
    for (int i = from; i + 3 < (int)to; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i;
        }
    }
    return -1;
}

static int download_fragment(storage_config_t *c, const char *filename,
                             uint8_t **plain, size_t *plain_len)
{
    char host[320], uri[1408], auth[1400], date[40];
    if (build_request(c, filename, host, sizeof(host), uri, sizeof(uri))) {
        printf(PRINT_TAG "build request failed (provider=%s)\r\n", c->provider);
        return -1;
    }

    /* P0 堆余量探针:512KB 单块分配在本工程无先例(OTA 只敢 8KB 流式),
     * 首测靠这行定夺;若 free 贴着 need 或 OOM,降 MAX_IMAGE_BYTES+
     * skill maxFs(128KB 起步)再试。 */
    printf(PRINT_TAG "fragment buffer need=%uB free(malloc)=%uB free(ram)=%uB\r\n",
           (unsigned)(DL_BUF_LEN + 4608),
           get_malloc_remain_heap_size(),
           get_ram_free_heap_size());
    char *req = (char *)malloc(4608);
    uint8_t *dl = (uint8_t *)malloc(DL_BUF_LEN);
    if (!req || !dl) {
        printf(PRINT_TAG "OOM for download buffers (need ~%uB)\r\n",
               (unsigned)(DL_BUF_LEN + 4608));
        free(req);
        free(dl);
        return -1;
    }

    int rc = -1;
    tls_t *tls = NULL;
    do {
        /* tls_connect 内部经 pal 完成 TCP+TLS 两阶段(共享一个建立预算),
         * 不能再自建一条 TCP,否则是两条白连的连接。 */
        tls_config_t tcfg = {
            .host         = host,
            .port         = 443,
            .sni          = host,
            /* 对象存储无设备侧 CA 包:沿用 kit 既有 VERIFY_NONE 形态(RED/CRA
             * 整改时随全链 CA 方案一并收口),功能面不受影响。 */
            .cacert       = NULL,
            .cert_bundle_attach = NULL,
            .verify       = TLS_VERIFY_NONE,
            .force_tls12  = true,
            .ciphersuites = tls_ciphersuites_tuya_default(),
            .connect_timeout_ms = TLS_CONN_TIMEOUT_MS,
            .pal          = s_pal,
        };
        tls = tls_connect(&tcfg);
        if (!tls) { printf(PRINT_TAG "TLS connect %s failed\r\n", host); break; }

        int hn = snprintf(req, 4608, "GET %s HTTP/1.1\r\nHost: %s\r\n", uri, host);
        if (hn < 0 || hn >= 4608) break;
        if (!strcmp(c->provider, "COS") || !strcmp(c->provider, "cos")) {
            if (cos_authorization(c, uri, auth, sizeof(auth))) break;
            hn += snprintf(req + hn, 4608 - hn,
                           "Authorization: %s\r\nx-cos-security-token: %s\r\n",
                           auth, c->token);
        } else if (!strcmp(c->provider, "S3") || !strcmp(c->provider, "s3")) {
            if (s3_signature(c, uri, date, sizeof(date), auth, sizeof(auth))) break;
            hn += snprintf(req + hn, 4608 - hn,
                           "x-amz-content-sha256: UNSIGNED-PAYLOAD\r\n"
                           "Authorization: %s\r\nx-amz-date: %s\r\n"
                           "x-amz-security-token: %s\r\n",
                           auth, date, c->token);
        } else {   /* Azure */
            const char *sv = strstr(c->token, "sv=");
            if (!sv) break;
            char version[40];
            size_t i = 0;
            sv += 3;
            while (sv[i] && sv[i] != '&' && i < sizeof(version) - 1) {
                version[i] = sv[i];
                i++;
            }
            version[i] = 0;
            hn += snprintf(req + hn, 4608 - hn,
                           "x-ms-blob-type: BlockBlob\r\nx-ms-version: %s\r\n", version);
        }
        if (hn < 0 || hn >= 4608) break;
        hn += snprintf(req + hn, 4608 - hn, "Connection: close\r\n\r\n");
        if (hn < 0 || hn >= 4608) break;

        if (tls_write(tls, (const uint8_t *)req, (size_t)hn, 10000) != TLS_OK) {
            printf(PRINT_TAG "request write failed\r\n");
            break;
        }

        size_t used = 0;
        int hdr_end = -1;
        int peer_closed = 0;
        unsigned int t0 = sys_timer_get_ms();
        for (;;) {
            if (sys_timer_get_ms() - t0 > DL_TIMEOUT_MS) {
                printf(PRINT_TAG "download timeout (%uB)\r\n", (unsigned)used);
                break;
            }
            size_t space = DL_BUF_LEN - used;
            if (!space) { printf(PRINT_TAG "fragment overflow\r\n"); break; }
            int n = tls_read(tls, dl + used, space, 3000);
            if (n > 0) {
                int search_from = ((size_t)used > 3) ? (int)used - 3 : 0;
                used += (size_t)n;
                if (hdr_end < 0) hdr_end = find_header_end(dl, search_from, used);
                continue;
            }
            if (n == 0) { peer_closed = 1; break; }        /* 对端 close_notify */
            if (n == TLS_ERR_AGAIN) continue;              /* 3s 无数据,外层超时兜底 */
            printf(PRINT_TAG "tls read error %d\r\n", n);
            break;
        }
        if (hdr_end < 0) { printf(PRINT_TAG "no HTTP response header\r\n"); break; }

        /* 状态行:HTTP/1.x NNN */
        if (used < 12 || memcmp(dl, "HTTP/1.", 7)) {
            printf(PRINT_TAG "bad status line\r\n");
            break;
        }
        int status = (dl[9] - '0') * 100 + (dl[10] - '0') * 10 + (dl[11] - '0');
        if (status != 200 && status != 201) {
            printf(PRINT_TAG "fragment GET HTTP %d\r\n", status);
            break;
        }

        /* Content-Length(大小写两种写法都认);无 CL 时以对端关闭为界。 */
        size_t body_off = (size_t)hdr_end + 4;
        size_t body_len = used - body_off;
        int have_cl = 0;
        size_t cl = 0;
        for (int i = 0; i < hdr_end; i++) {
            const char *p = (const char *)dl + i;
            int match = 0;
            if (hdr_end - i > 15
                && (!memcmp(p, "content-length:", 15) || !memcmp(p, "Content-Length:", 15))) {
                match = 1;
            }
            if (match) {
                cl = 0;
                int j = i + 15;
                while (j < hdr_end && (dl[j] == ' ' || dl[j] == '\t')) j++;
                while (j < hdr_end && dl[j] >= '0' && dl[j] <= '9') {
                    cl = cl * 10u + (size_t)(dl[j] - '0');
                    j++;
                }
                have_cl = 1;
                break;
            }
        }
        if (have_cl && body_len != cl) {
            printf(PRINT_TAG "truncated body: %uB of %uB\r\n",
                   (unsigned)body_len, (unsigned)cl);
            break;
        }
        if (!have_cl && !peer_closed) {
            printf(PRINT_TAG "no Content-Length and connection still open\r\n");
            break;
        }
        memmove(dl, dl + body_off, body_len);

        /* 密文格式:64B 头(ver4+iv16+resv44)+ CBC 正文,16 对齐(手册 §4.6)。 */
        if (body_len < 80 || ((body_len - 64) % 16) != 0) {
            printf(PRINT_TAG "invalid encrypted fragment length %uB\r\n",
                   (unsigned)body_len);
            break;
        }
        uint8_t iv[16];
        memcpy(iv, dl + 4, 16);
        size_t cipher_len = body_len - 64;
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        int arc = mbedtls_aes_setkey_dec(&aes, s_tc_key, 128);
        if (!arc) {
            arc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipher_len,
                                        iv, dl + 64, dl + 64);
        }
        mbedtls_aes_free(&aes);
        if (arc) { printf(PRINT_TAG "AES-CBC decode failed: %d\r\n", arc); break; }
        /* PKCS#7 末包去填充:校验与去填充用同一个 pad 值。 */
        uint8_t pad = dl[body_len - 1];
        int pad_ok = (pad > 0 && pad <= 16 && pad <= cipher_len);
        for (size_t i = 0; pad_ok && i < pad; i++) {
            if (dl[body_len - 1 - i] != pad) pad_ok = 0;
        }
        if (!pad_ok) {
            printf(PRINT_TAG "invalid AES padding\r\n");
            break;
        }
        printf(PRINT_TAG "fragment decrypted: %u bytes (encrypted %uB)\r\n",
               (unsigned)(cipher_len - pad), (unsigned)body_len);
        *plain = dl + 64;              /* 内部指针,释放基址 = *plain - 64 */
        *plain_len = cipher_len - pad;
        rc = 0;
    } while (0);

    if (rc != 0) free(dl);
    if (tls) tls_close(tls);   /* 含 close_notify + 关底层 socket + 释放 */
    free(req);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* 协议19回执:组装信封入队,由保活线程泵出                                    */
/* ------------------------------------------------------------------------- */
static int publish_reply(const cJSON *data, const char *event, int code)
{
    cJSON *reply = cJSON_CreateObject();
    if (!reply) return -1;
    cJSON_AddStringToObject(reply, "reqType", "fileStorageDl");
    const cJSON *sid = cJSON_GetObjectItem(data, "sid");
    const cJSON *prefix = cJSON_GetObjectItem(data, "prefix");
    if (cJSON_IsString(sid)) {
        cJSON_AddStringToObject(reply, "sid", sid->valuestring);
    } else if (cJSON_IsNumber(sid)) {
        /* wiki 回执模板与闭源实测(10/27)的 sid 都是字符串;探针 sid 是纯数字毫秒,
         * 转字符串镜像保持类型同构。数值转文本走 cJSON(本板已验证零失真),
         * 绕开 printf 浮点与 %llu 双坑([[activation-crash-zu-printf]])。 */
        char *num = cJSON_Print(sid);
        if (num) {
            cJSON_AddStringToObject(reply, "sid", num);
            free(num);
        } else {
            cJSON_AddStringToObject(reply, "sid", "");
        }
    } else {
        cJSON_AddStringToObject(reply, "sid", "");
    }
    cJSON_AddStringToObject(reply, "event", event);
    if (cJSON_IsString(prefix)) cJSON_AddStringToObject(reply, "prefix", prefix->valuestring);
    cJSON_AddNumberToObject(reply, "code", code);
    const cJSON *name = cJSON_GetObjectItem(data, "realFn");
    const cJSON *meta = cJSON_GetObjectItem(data, "meta");
    if (cJSON_IsString(name)) cJSON_AddStringToObject(reply, "realFn", name->valuestring);
    if (cJSON_IsString(meta) && meta->valuestring[0]) {
        cJSON_AddStringToObject(reply, "meta", meta->valuestring);
    }
    char *payload = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (!payload) return -1;

    cJSON *envelope = cJSON_CreateObject();
    if (!envelope) { free(payload); return -1; }
    cJSON_AddNumberToObject(envelope, "protocol", MQTT_PROTOCOL_DEVICE_RESPONSE);
    /* SNTP 系统钟,与 kit/URL 同源;now_epoch 播种前的开机秒数 t 不可外发 */
    cJSON_AddNumberToObject(envelope, "t", (double)time(NULL));
    cJSON *rdata = cJSON_Parse(payload);   /* 以 data 字段身份再入信封 */
    free(payload);
    if (!rdata) { cJSON_Delete(envelope); return -1; }
    cJSON_AddItemToObject(envelope, "data", rdata);
    char *out = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (!out) return -1;

    int qs = q_send(&s_pubq, out);
    if (qs != 0) {
        /* -2=满,-1=未就绪(tc.init 引导未完成);两种都是丢,但原因不同。 */
        printf(PRINT_TAG "reply dropped: %s (event=%s)\r\n",
               (qs == -2) ? "queue full" : "queue not ready", event);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* uploaded 事件:逐分片取凭证→下载→解密→拼装→回调                            */
/* ------------------------------------------------------------------------- */
static int handle_uploaded(const cJSON *data)
{
    const cJSON *frags = cJSON_GetObjectItem(data, "fragFn");
    const cJSON *file_type = cJSON_GetObjectItem(data, "fileType");
    if (!cJSON_IsArray(frags) || !cJSON_IsNumber(file_type) || file_type->valueint != 0) {
        printf(PRINT_TAG "ignore non-image or malformed transfer\r\n");
        publish_reply(data, "downloaded", -1);
        return -1;
    }

    int count = cJSON_GetArraySize(frags);
    printf(PRINT_TAG "uploaded image: %d fragments\r\n", count);
    int rc = 0;
    const uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    uint8_t *image = NULL;      /* 多分片累计缓冲(单分片零拷贝路径不分配) */
    size_t image_len = 0;
    uint8_t *keep_base = NULL;  /* 单分片零拷贝:解密缓冲即成图,回调返回后才许释放 */

    if (count < 1 || count > 8) rc = -1;   /* skill 声明 maxFc=1,>8 视为异常 */

    for (int i = 0; !rc && i < count; i++) {
        const cJSON *f = cJSON_GetArrayItem(frags, i);
        if (!cJSON_IsString(f) || !f->valuestring
            || strchr(f->valuestring, '/') || strstr(f->valuestring, "..")) {
            rc = -1;
            break;
        }
        storage_config_t current;
        memset(&current, 0, sizeof(current));
        int frag_rc = get_storage_config(&current);
        uint8_t *part = NULL;
        size_t part_len = 0;
        if (frag_rc == 0) {
            frag_rc = download_fragment(&current, f->valuestring, &part, &part_len);
        }
        if (frag_rc != 0) { rc = -1; break; }

        if (count == 1) {
            jpeg = part;
            jpeg_len = part_len;
            keep_base = part - 64;
        } else {
            if (!image) {
                /* 多分片兜底路径:此时单分片解密缓冲还活着,峰值=2×512KB,
                 * 一并探针。skill 声明 maxFc=1,正常不进此分支。 */
                printf(PRINT_TAG "multi-frag buffer need=%uB free(malloc)=%uB free(ram)=%uB\r\n",
                       (unsigned)MAX_IMAGE_BYTES,
                       get_malloc_remain_heap_size(),
                       get_ram_free_heap_size());
                image = (uint8_t *)malloc(MAX_IMAGE_BYTES);
                if (!image) {
                    free(part - 64);
                    rc = -1;
                    break;
                }
            }
            if (part_len > MAX_IMAGE_BYTES - image_len) {
                free(part - 64);
                rc = -1;
                break;
            }
            memcpy(image + image_len, part, part_len);
            image_len += part_len;
            free(part - 64);   /* download_fragment 返回内部指针,回收分配基址 */
        }
    }

    if (!rc && count == 1) {
        if (jpeg_len >= 4 && jpeg[0] == 0xFF && jpeg[1] == 0xD8) {
            if (s_ready_cb) s_ready_cb(jpeg, jpeg_len, s_ready_ud);
        } else {
            printf(PRINT_TAG "not a JPEG (no SOI)\r\n");
            rc = -1;
        }
    } else if (!rc && count > 1) {
        if (image_len >= 4 && image[0] == 0xFF && image[1] == 0xD8) {
            if (s_ready_cb) s_ready_cb(image, image_len, s_ready_ud);
        } else {
            printf(PRINT_TAG "not a JPEG (no SOI)\r\n");
            rc = -1;
        }
    } else if (!rc) {
        rc = -1;
    }
    free(keep_base);   /* 零拷贝路径:回调已返回,现在才能放 */
    free(image);
    printf(PRINT_TAG "App image transfer %s (%u bytes)\r\n",
           rc ? "FAILED" : "complete",
           (unsigned)(count == 1 ? jpeg_len : image_len));
    publish_reply(data, "downloaded", rc);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* 事件线程 + 引导线程                                                        */
/* ------------------------------------------------------------------------- */
static void event_worker(void *arg)
{
    (void)arg;
    for (;;) {
        char *json = q_recv_block(&s_evtq);
        if (!json) continue;
        cJSON *root = cJSON_Parse(json);
        free(json);
        if (!root) continue;
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *event = cJSON_GetObjectItem(data, "event");
        if (cJSON_IsString(event)) {
            printf(PRINT_TAG "handling image event: %s\r\n", event->valuestring);
            if (!strcmp(event->valuestring, "start")) {
                s_downloaded_count = 0;
                publish_reply(data, "startAnswer", 0);
            } else if (!strcmp(event->valuestring, "uploaded")) {
                if (handle_uploaded(data) == 0) s_downloaded_count++;
            } else if (!strcmp(event->valuestring, "uploadFinished")) {
                if (s_downloaded_count) publish_reply(data, "downloadFinish", 0);
            } else if (!strcmp(event->valuestring, "heartBeat")) {
                publish_reply(data, "heartBeat", 0);
            }
            /* query/delete:面板存储管理才需要,参考实现同样不处理 */
        }
        cJSON_Delete(root);
    }
}

static void app_image_boot(void *arg)
{
    (void)arg;
    if (q_init(&s_evtq) != 0 || q_init(&s_pubq) != 0) {
        printf(PRINT_TAG "queue init failed, image downlink disabled\r\n");
        return;
    }
    /* 先起事件线程再 init:tc.init 一返回云端就可能推事件,不能有真空窗。 */
    if (thread_fork("app_img_dl", 4, 8 * 1024, 0, 0, event_worker, NULL) != 0) {
        printf(PRINT_TAG "event worker fork failed, image downlink disabled\r\n");
        return;
    }
    if (tc_key_init() != 0) {
        printf(PRINT_TAG "thing.tc.init failed -- cloud will NOT push images. "
                         "Reboot to retry.\r\n");
        return;
    }
    printf(PRINT_TAG "image channel ready (tc channel registered, "
                     "expect protocol-64 downlinks)\r\n");
}

/* ------------------------------------------------------------------------- */
/* 对外接口                                                                  */
/* ------------------------------------------------------------------------- */
int app_image_start(iot_client_t *iot, app_image_ready_cb_t cb, void *ud)
{
    if (!iot || !cb) return -1;
    if (s_started) return 0;   /* 幂等 */
    s_started = 1;
    s_iot = iot;
    s_pal = iot->pal;
    s_ready_cb = cb;
    s_ready_ud = ud;
    memset(s_tc_key, 0, sizeof(s_tc_key));
    if (thread_fork("app_img_boot", 4, 8 * 1024, 0, 0, app_image_boot, NULL) != 0) {
        s_started = 0;
        return -1;
    }
    return 0;
}

void app_image_mqtt_message(const char *topic, size_t topic_len,
                            const uint8_t *data, size_t data_len)
{
    (void)topic;
    (void)topic_len;
    if (!data || data_len == 0 || data_len > MAX_EVENT_JSON) return;
    cJSON *root = cJSON_ParseWithLength((const char *)data, data_len);
    if (!root) return;   /* 非 JSON 下行(DP 等)与本模块无关,demo 侧已有全量打印 */
    const cJSON *protocol = cJSON_GetObjectItem(root, "protocol");
    const cJSON *pdata = cJSON_GetObjectItem(root, "data");
    const cJSON *req = cJSON_GetObjectItem(pdata, "reqType");
    if (cJSON_IsNumber(protocol)
        && protocol->valueint == MQTT_PROTOCOL_FILE_STORAGE_DL
        && cJSON_IsString(req) && !strcmp(req->valuestring, "fileStorageDl")) {
        char *copy = cJSON_PrintUnformatted(root);
        if (copy) {
            int qs = q_send(&s_evtq, copy);
            if (qs == -1) {
                /* 队列未就绪(tc.init 引导未完成)——此阶段云端本不该推,
                 * 打一行留痕防协议异常被静默吞掉。 */
                printf(PRINT_TAG "event before queue ready, dropped\r\n");
            } else if (qs == -2) {
                printf(PRINT_TAG "event queue full, transfer dropped\r\n");
            }
        }
    }
    cJSON_Delete(root);
}

static int s_pubq_stall_logged;   /* 断连重试期只打一次告警,恢复发布即复位 */

void app_image_poll_publish(iot_client_t *iot)
{
    if (!s_pubq.inited) return;
    for (;;) {
        char *json = q_peek(&s_pubq);
        if (!json) return;
        int rc = iot_client_publish(iot, (const uint8_t *)json, strlen(json));
        if (rc != 0) {
            /* P1:失败不丢——留在队头下轮泵重试(MQTT 短暂断连窗不再吞回执,
             * 图已下成功而云端收不到 downloaded 的误报消失)。重试节奏随保活
             * 循环;mqtt==NULL 时 publish 立即错返,无额外开销。 */
            if (!s_pubq_stall_logged) {
                printf(PRINT_TAG "reply publish failed rc=%d (deferred, will retry)\r\n", rc);
                s_pubq_stall_logged = 1;
            }
            return;
        }
        s_pubq_stall_logged = 0;
        printf(TAG_PRINTF "reply published (%u bytes)\r\n", (unsigned)strlen(json));
        q_drop_head(&s_pubq);
        free(json);
    }
}
