# 涂鸦 agentic-kit 集成到杰理 AC791N 工程方案

> 目标:让 AC791N 开发板连接涂鸦 AI Agent 云端,实现语音/文本对话。
> 适用:杰理 AC791N(wl82)AIoT SDK(`release/AC79NN_SDK_V1.2.0`)+ 涂鸦 agentic-kit(已下载到 `/Users/gaoshiqiang/code/agentic-kit`)。
> 前提:涂鸦云端配置(PID/设备三元组/token/region)你已掌握,本文只讲端侧集成。

---

## 0. 一句话结论

agentic-kit 的**开源部分**(`rtc-tcp-client` + `iot-client`)能整体编译进 AC791N,因为 AC791N 工程已经自带了它需要的全部底层库 —— `libmbedtls_3_4_0.a`、`cJSON.a`、`lwip_2_2_0.a`、`lib_mqtt.a`、`libcurl.a` 都已链接。

要做的事只有三件:
1. 写一个 ~150 行的 **PAL 平台适配文件**(`pal_ac791n.c`);
2. 改一份 **Makefile**(加源码/include/宏);
3. 写一个 **调用入口**(`tuya_agentic_demo.c`)并接到 Wi-Fi 联网成功事件。

> 闭源的 UDP `rtc-client`(`libstm.a`)没有 pi32v2/DSP 编译版本,**直接不用**;开源的 TCP 版完全够用。

---

## 1. 关键决策(已被工程验证可行)

| 决策 | 说明 |
|---|---|
| 用开源 `rtc-tcp-client`,不用 `rtc-client` | `libstm.a` 只有 mac/linux/ingenic-mips/rockchip-arm 版,无 DSP 版 |
| 复用宿主 mbedTLS(3.4.0)和 cJSON | 不编译 agentic-kit 自带的这两份,避免符号重复链接报错 |
| coreHTTP + coreMQTT 从源码编译 | AC791N 工程没有这两个(agentic-kit 的 HTTPS ATOP 与 MQTT 依赖它们) |
| PAL 照搬 `pal_freertos.c` | AC791N 正是 FreeRTOS + lwIP,模板几乎现成 |

---

## 2. 架构

```
        你的 App (wifi_story_machine)
              │
              │  DHCP 成功后启动 tuya_agentic_demo 任务
              ▼
   ┌─────────────────────────────────────────┐
   │  agentic-kit (从源码编译,复用宿主库)       │
   │  ┌───────────────┐   ┌──────────────┐   │
   │  │ rtc-tcp-client │   │ iot-client    │   │  ← 全开源,7 + 11 个 .c
   │  │  (tuya_ai.h)   │   │ (激活/DP/OTA) │   │
   │  └───────┬────────┘   └──────┬───────┘   │
   │          │   common/          │          │
   │          ├── tls.c ───────────┤ (基于 mbedTLS,无需改)
   │          └── rng.c / log.c ───┘          │
   │          coreHTTP / coreMQTT (从源码编译) │
   └────────────────────┬─────────────────────┘
                        │ PAL (你要写的 pal_ac791n.c)
                        ▼
   AC791N 宿主:lwIP socket / mbedTLS / thread_fork / os_mutex / malloc / timer_get_ms
```

---

## 3. 文件落地

### 3.1 路径约定

- **工程根** `ROOT` = `.../fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0/fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0`(注意两层嵌套,里面有 `apps/`、`cpu/`、`Makefile`)
- **agentic-kit 源** = `/Users/gaoshiqiang/code/agentic-kit`

### 3.2 新建目录 `apps/common/LLM/tuya_agentic/`

```
apps/common/LLM/tuya_agentic/
├── agentic-kit/                 ← 从 /Users/gaoshiqiang/code/agentic-kit 复制以下子目录:
│   ├── modules/rtc-tcp-client/  (整个)
│   ├── modules/iot-client/      (整个)
│   ├── pal/                     (只留 pal.h)
│   ├── common/                  (tls.c tls.h log.c log.h rng.c rng.h)
│   ├── third_party/coreHTTP/    (整个)
│   └── third_party/coreMQTT/    (整个)
│   【不复制】third_party/mbedtls、third_party/cJSON、modules/rtc-client、modules/tuya-ble
├── pal_ac791n.c                 ← ★你写:PAL 实现
├── tuya_agentic.h               ← ★你写:对外入口
└── tuya_agentic_demo.c          ← ★你写:对话主流程(仿 volc/VolcEngineRTCLiteDemo.c)
```

### 3.3 复制命令

```bash
AK=/Users/gaoshiqiang/code/agentic-kit
DST=<ROOT>/apps/common/LLM/tuya_agentic/agentic-kit
mkdir -p "$DST/modules" "$DST/third_party"
cp -R "$AK/modules/rtc-tcp-client" "$AK/modules/iot-client" "$DST/modules/"
mkdir -p "$DST/pal" && cp "$AK/pal/pal.h" "$DST/pal/"
cp -R "$AK/common" "$DST/"
cp -R "$AK/third_party/coreHTTP" "$AK/third_party/coreMQTT" "$DST/third_party/"
```

---

## 4. PAL 实现:`pal_ac791n.c`(核心)

agentic-kit 的 PAL 契约只有一个结构体 `pal_t`(见 `pal.h`),共 9 组函数。下面这张映射表是整个移植的钥匙(宿主 API 与工程里 `apps/common/LLM/volc/src/platform/` 用的是同一套,已验证)。

### 4.1 映射表

| PAL 函数 | AC791N 宿主 API | 头文件 |
|---|---|---|
| `malloc` / `free` | `malloc` / `free` | `<stdlib.h>` |
| `time_ms()` | `timer_get_ms()`(转 `uint64_t`) | `system/timer.h` |
| `mutex_create/lock/unlock/destroy` | **必须递归锁**:见 4.3 | — |
| `thread_create(handle, func, arg)` | `thread_fork(...)` + trampoline | `system/os/os_api.h` |
| `thread_join` | `thread_kill(.., KILL_WAIT)` | `system/os/os_api.h` |
| `tcp_connect(host,port,to)` | `getaddrinfo` → `socket/connect` | `lwip/sockets.h`、`lwip/netdb.h` |
| `tcp_send/recv` | `send/recv`,would-block 返回 `TAI_ERR_AGAIN` | `lwip/sockets.h` |
| `tcp_close` | `close` | `lwip/sockets.h` |
| `tcp_poll(h,events,to)` | `select` (events: 1=可读 2=可写) | `lwip/sockets.h` |

### 4.2 完整骨架(填进 `pal_ac791n.c`)

```c
/* pal_ac791n.c — agentic-kit PAL for JieLi AC791N (FreeRTOS + lwIP + mbedTLS) */
#include "pal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* 宿主头 —— 与 apps/common/LLM/volc/src/platform 用的完全一致 */
#include "system/timer.h"        /* timer_get_ms            */
#include "system/os/os_api.h"    /* thread_fork / os_mutex_* */
#include "lwip/sockets.h"        /* socket/connect/select/… */
#include "lwip/netdb.h"          /* getaddrinfo             */

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4        /* 杰理 newlib 缺该宏,补上(volc_time.c 同款) */
#endif

/* ---------------- 内存 / 时间 ---------------- */
static void   *ac_malloc(size_t n)        { return malloc(n); }
static void    ac_free(void *p)           { free(p); }
static uint64_t ac_time_ms(void)          { return (uint64_t)timer_get_ms(); }

/* ---------------- 递归互斥锁(关键!)---------------- */
/* 若 os_mutex 非递归,改用 xSemaphoreCreateRecursiveMutex(需 configUSE_RECURSIVE_MUTEXES=1)
   或 pthread_mutexattr_settype(PTHREAD_MUTEX_RECURSIVE)。 */
static void   *ac_mutex_create(void)      { OS_MUTEX *m = malloc(sizeof(OS_MUTEX));
                                            os_mutex_create(m); return m; }
static void    ac_mutex_lock(void *m)     { os_mutex_pend((OS_MUTEX *)m, 0); }
static void    ac_mutex_unlock(void *m)   { os_mutex_post((OS_MUTEX *)m); }
static void    ac_mutex_destroy(void *m)  { os_mutex_del((OS_MUTEX *)m, OS_DEL_ALWAYS);
                                            free(m); }

/* ---------------- 线程 ---------------- */
/* PAL 要 pthread 风格 void* fn(void*);thread_fork 要 void fn(void*),加 trampoline 桥接。 */
typedef struct { void *(*fn)(void *); void *arg; } ac_thr_arg;
static void ac_thr_entry(void *p) { ac_thr_arg *a = (ac_thr_arg *)p; a->fn(a->arg); }

static int ac_thread_create(void **handle, void *(*fn)(void *), void *arg)
{
    ac_thr_arg *a = (ac_thr_arg *)malloc(sizeof(*a));
    if (!a) return -1;
    a->fn = fn; a->arg = arg;
    int *pid = (int *)malloc(sizeof(int));
    if (!pid) { free(a); return -1; }
    /* thread_fork(name, prio, stack_in_ints, qsize, pid_slot, entry, arg) */
    thread_fork("tuya_pal", 4, 4 * 1024 /* ints = 16KB 栈 */, 0, pid, ac_thr_entry, a);
    *handle = pid;
    return 0;
}
static int ac_thread_join(void *handle)
{
    /* 杰理 join 不一定可靠,参照 volc:用 KILL_WAIT 回收 */
    int *pid = (int *)handle;
    thread_kill(pid, KILL_WAIT);
    free(pid);
    return 0;
}

/* ---------------- TCP socket(基本照搬 pal_freertos.c)---------------- */
typedef struct { int fd; } ac_sock_t;

static void set_nonblock(int fd, int nb)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
static int wait_writable(int fd, uint32_t timeout_ms)
{
    fd_set wfds; struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&wfds); FD_SET(fd, &wfds);
    int r = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (r <= 0) return -1;
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    return err == 0 ? 0 : -1;
}

static void *ac_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%u", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return NULL;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return NULL; }

    set_nonblock(fd, 1);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return NULL; }
    if (rc < 0 && timeout_ms > 0 && wait_writable(fd, timeout_ms) != 0) { close(fd); return NULL; }
    set_nonblock(fd, 0);                       /* 回到阻塞,靠 SO_RCVTIMEO/SO_SNDTIMEO 控超时 */

    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    ac_sock_t *s = (ac_sock_t *)malloc(sizeof(*s));
    if (!s) { close(fd); return NULL; }
    s->fd = fd;
    return s;
}
static int ac_tcp_send(void *h, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    ac_sock_t *s = (ac_sock_t *)h;
    /* 可在此用 setsockopt SO_SNDTIMEO 应用 timeout_ms */
    int n = send(s->fd, buf, len, 0);
    if (n > 0) return n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return TAI_ERR_AGAIN;                  /* -7,调用方会重试 */
    return TAI_ERR_NET;                        /* -3,致命 */
}
static int ac_tcp_recv(void *h, uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    ac_sock_t *s = (ac_sock_t *)h;
    int n = recv(s->fd, buf, buf_len, 0);
    if (n > 0) return n;
    if (n == 0) return 0;                      /* EOF */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return TAI_ERR_AGAIN;
    return TAI_ERR_NET;
}
static void ac_tcp_close(void *h)
{
    ac_sock_t *s = (ac_sock_t *)h;
    if (s) { close(s->fd); free(s); }
}
static int ac_tcp_poll(void *h, int events, uint32_t timeout_ms)
{
    ac_sock_t *s = (ac_sock_t *)h;
    fd_set rfds, wfds;
    FD_ZERO(&rfds); FD_ZERO(&wfds);
    if (events & 1) FD_SET(s->fd, &rfds);
    if (events & 2) FD_SET(s->fd, &wfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(s->fd + 1,
                   (events & 1) ? &rfds : NULL,
                   (events & 2) ? &wfds : NULL,
                   NULL, &tv);
    int ret = 0;
    if (r > 0) {
        if ((events & 1) && FD_ISSET(s->fd, &rfds)) ret |= 1;
        if ((events & 2) && FD_ISSET(s->fd, &wfds)) ret |= 2;
    }
    return ret;                                /* 0=超时,>0=就绪位掩码 */
}

/* ---------------- 导出 ---------------- */
const pal_t *tai_pal_ac791n(void)
{
    static const pal_t pal = {
        .tcp_connect  = ac_tcp_connect,
        .tcp_send     = ac_tcp_send,
        .tcp_recv     = ac_tcp_recv,
        .tcp_close    = ac_tcp_close,
        .tcp_poll     = ac_tcp_poll,
        .time_ms      = ac_time_ms,
        .malloc       = ac_malloc,
        .free         = ac_free,
        .mutex_create = ac_mutex_create,
        .mutex_lock   = ac_mutex_lock,
        .mutex_unlock = ac_mutex_unlock,
        .mutex_destroy= ac_mutex_destroy,
        .thread_create= ac_thread_create,
        .thread_join  = ac_thread_join,
    };
    return &pal;
}
```

### 4.3 三个必看的坑(都来自 agentic-kit 的硬要求)

1. **互斥锁必须是递归的**。iot-client 的 DP schema 更新会自重入锁,普通互斥锁会死锁。先确认 `os_mutex` 是否递归(查 `include_lib/system/os/os_api.h` 注释);若不是,改用 `xSemaphoreCreateRecursiveMutex`(需 `configUSE_RECURSIVE_MUTEXES=1`)或 `pthread_mutexattr_settype(PTHREAD_MUTEX_RECURSIVE)`。
2. **`mbedtls_hardware_poll` 熵源**。agentic-kit 的 `rng.c` 走 mbedTLS 熵。复用宿主 `libmbedtls_3_4_0.a` 后,**它大概率已带** `mbedtls_hardware_poll`(杰理自己的 HTTPS 也需要,已对接芯片 TRNG)。若链接时报 `undefined reference to mbedtls_hardware_poll`,补一个读 TRNG 的小函数并定义 `MBEDTLS_ENTROPY_HARDWARE_ALT`。
3. **worker 线程栈**。`tai_connect` 起的后台线程跑 mbedTLS 握手 + 收发,默认 4096 words(~16KB)。`thread_fork` 第 3 个参数是 **int 数**,`4*1024`=16KB 够用;若握手崩栈再加。

---

## 5. Makefile 改动(精确清单)

编辑 `apps/wifi_story_machine/board/wl82/Makefile`,三处增量。
> 注意路径前缀 `../../../../`(从 board/wl82/ 回到工程根)。

### 5.1 `INCLUDES` 末尾追加

```makefile
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/rtc-tcp-client/include \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/include \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/pal \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/common \
	-I../../../../apps/common/LLM/tuya_agentic \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreHTTP/source/include \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreHTTP/source/interface \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreHTTP/source/dependency/3rdparty/llhttp/include \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreMQTT/source/include \
	-I../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreMQTT/source/interface \
```
> mbedTLS、cJSON 的 include **不加** —— 用宿主已有的 `include_lib/net/mbedtls_3_4_0` 和 `include_lib/net/cJSON_common`。

### 5.2 `DEFINES` 末尾追加

```makefile
	-DHTTP_DO_NOT_USE_CUSTOM_CONFIG \
	-DMQTT_DO_NOT_USE_CUSTOM_CONFIG \
	-DIOT_DO_NOT_USE_CUSTOM_CONFIG \
```

### 5.3 `c_SRC_FILES` 追加

```makefile
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/rtc-tcp-client/src/tai_attrs.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/rtc-tcp-client/src/tai_client.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/rtc-tcp-client/src/tai_crypto.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/rtc-tcp-client/src/tai_packet.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/rtc-tcp-client/src/tai_pkt_log.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/rtc-tcp-client/src/tai_protocol.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/rtc-tcp-client/src/tai_transport.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/atop.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/atop_base.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/cipher_wrapper.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/http_client_interface.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_client.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_client_message.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_dns.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_dp.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_on_boarding.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/iot_ota.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/mqtt.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/common/tls.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/common/log.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/common/rng.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreHTTP/source/core_http_client.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreHTTP/source/dependency/3rdparty/llhttp/src/api.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreHTTP/source/dependency/3rdparty/llhttp/src/http.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreHTTP/source/dependency/3rdparty/llhttp/src/llhttp.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreMQTT/source/core_mqtt.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreMQTT/source/core_mqtt_serializer.c \
	../../../../apps/common/LLM/tuya_agentic/agentic-kit/third_party/coreMQTT/source/core_mqtt_state.c \
	../../../../apps/common/LLM/tuya_agentic/pal_ac791n.c \
	../../../../apps/common/LLM/tuya_agentic/tuya_agentic_demo.c \
```

**不要加**(否则报错或冲突):
- `modules/iot-client/src/iot_pal_defaults.c` —— 它写死返回 posix PAL,我们要用自己的;
- `modules/tuya-ble/src/tuya_ble_prov.c` —— 不用 BLE 配网;
- `third_party/mbedtls/**` 与 `third_party/cJSON/**` 的源码 —— 与宿主库符号冲突。

### 5.4 `LFLAGS` 不用动

`libmbedtls_3_4_0.a`、`cJSON.a`、`lwip_2_2_0.a`、`lib_mqtt.a` 都已在该变量里链接。

---

## 6. 调用入口:`tuya_agentic_demo.c`

完整流程(对照 agentic-kit 的 `examples/posix/ai/` 文本对话 demo,字段名一致)。

### 6.1 demo 主流程

```c
/* tuya_agentic_demo.c */
#include "pal.h"
#include "iot_client.h"
#include "tuya_ai.h"
#include "tuya_agentic.h"
extern const pal_t *tai_pal_ac791n(void);

/* 涂鸦云端配出来的凭据(你说云端配置已会):填你的设备三元组或用配网 token */
static const char *DEVID    = "tuya_xxx";
static const char *SEC      = "xxx";
static const char *LOCALKEY = "xxx";

/* RTC 回调(均在 worker 线程触发,数据指针仅在回调期间有效) */
static void on_text(tai_ctx_t *c, const tai_text_msg_t *m, void *ud) {
    printf("[AI]%.*s\n", (int)m->len, m->text);
}
static void on_audio(tai_ctx_t *c, const tai_audio_msg_t *m, void *ud) {
    /* 把 m 的 PCM 送音频播放(阶段3) */
}
static void on_event(tai_ctx_t *c, const tai_event_msg_t *m, void *ud) {
    /* TAI_EVT_END = 本轮回答结束;TAI_EVT_MCP_CMD 需回 tai_send_mcp_response */
}
static void on_disconnect(tai_ctx_t *c, const tai_disconnect_msg_t *m, void *ud) {}

void tuya_agentic_demo(void *arg)
{
    /* ① 平台/SDK 初始化 */
    const pal_t *pal = tai_pal_ac791n();
    iot_init(pal);

    /* ② 配置并激活设备(MQTT) */
    iot_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.region            = AY;          /* AY=中国大陆 */
    cfg.env               = PROD;
    cfg.mqtt_disable_tls  = false;       /* mqtts */
    cfg.mqtt_auto_connect = 1;
    memcpy(cfg.devid,      DEVID,    32);
    memcpy(cfg.secret_key, SEC,      32);
    memcpy(cfg.local_key,  LOCALKEY, 32);
    iot_client_t *iot = iot_client_init(&cfg);

    /* ③ ATOP over HTTPS 拿会话 token */
    char token[4096];
    iot_client_get_session_token(iot, NULL, token, sizeof(token));

    /* ④ 解析 token 得 host/port/sni/agent_token/biz_code/biz_tag/derived_client_id
          解析函数在 examples/posix/ai 的 demo 里(parse_connect_token),直接搬过来 */
    parsed_t cp = parse_token(token, LOCALkey);

    /* ⑤ 组装 tai_config_t */
    tai_config_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.host             = cp.host;
    tc.port             = cp.port;
    tc.tls_sni          = cp.tls_sni;
    tc.device_id        = cp.derived_client_id;
    tc.local_key        = LOCALkey;
    tc.agent_token      = cp.agent_token;
    tc.biz_code         = cp.biz_code;
    tc.biz_tag          = cp.biz_tag;
    tc.protocol_version = TAI_VER_21;
    tc.client_type      = TAI_CLIENT_DEVICE;
    tc.sign_level       = TAI_SIGN_HMAC_SHA256;
    tc.pal              = pal;
    tc.on_text          = on_text;
    tc.on_audio         = on_audio;
    tc.on_event         = on_event;
    tc.on_disconnect    = on_disconnect;

    /* ⑥ 建会话(TLS + ClientHello + SessionNew + 起 worker 线程) */
    void *mem = pal->malloc(tai_ctx_size());
    tai_ctx_t *ctx = tai_ctx_init(mem, &tc);
    tai_connect(ctx);

    /* ⑦ 对话:先跑通文本 */
    const char *q = "你好,介绍一下你自己";
    tai_send_text(ctx, q, strlen(q));
    /* on_text 回调会收到回答。
       语音:tai_send_audio_start(TAI_AUDIO_OPUS, ch, bit, sr)
            → 循环 tai_send_audio_chunk(opus_pkt, len) → tai_send_audio_end() */
}
```

> `parse_token()` 不是 agentic-kit 的公开 API,在 `examples/posix/ai/` 的 demo 源码里,把那段(token base64 解码 + cJSON 解析出 connect_conf/session_conf)整体拷到你的 demo 文件即可。

### 6.2 接到开发板启动流程

在 `apps/wifi_story_machine/wifi_app_task.c` 的 `WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC`(拿到 IP)分支里,对照同文件已有的 `thread_fork("Volc_demo", ...)` 那一行,加一条:

```c
thread_fork("tuya_agentic", 4, 6*1024, 0, 0, tuya_agentic_demo, NULL);
```

Wi-Fi 一通即自动连涂鸦 AI Agent。

---

## 7. 分阶段验证(按顺序,别一次全上)

| 阶段 | 目标 | 判定 |
|---|---|---|
| 0 | 改完 Makefile,`make ac791n_wifi_story_machine` **编译链接通过** | 无 undefined symbol / 无符号重复 |
| 1 | 跑到 `iot_client_get_session_token` 成功返回 token | 证明 DNS + MQTT + TLS + ATOP 全通(PAL 的 socket/mutex/thread/time/mbedTLS 都对) |
| 2 | `tai_connect` + `tai_send_text` → `on_text` 收到回复 | 证明 RTC TCP + worker 线程跑通 |
| 3 | `tai_send_audio_*` 喂麦克风 OPUS、`on_audio` 播 PCM | 接入音频管线(`apps/common/LLM/audio/audio_input.c` + opus 编码库) |

阶段 0 大概率有 1~2 个小编译错(coreMQTT/llhttp 对 pi32v2 的个别警告/类型问题),逐个修即可。

---

## 8. 需要确认/留意的点(本环境无法验证)

1. **`os_mutex` 是否递归** —— 决定 PAL 锁写法。查 `include_lib/system/os/os_api.h`。
2. **`mbedtls_hardware_poll` 是否已存在于 `libmbedtls_3_4_0.a`** —— 不在就补(见 4.3-2)。
3. **mbedTLS 版本 3.4 vs agentic-kit 头文件 3.6** —— 必须用**宿主 3.4 的头**(`include_lib/net/mbedtls_3_4_0`),不要 include agentic-kit 自带的 mbedtls 头,否则结构体布局不一致。
4. **`include_lib/` 和 `lib/` 目录** —— 在这个 release 包的工程根下不存在(属杰理完整 SDK/工具链)。你本机若能正常编译 wifi_story_machine,说明环境已具备。
5. **堆预算** —— 一个 RTC 会话约 45KB + TLS 16K×2 + MQTT 4K,留 ~120KB 余量;AC791N 578KB SRAM 足够。
6. **lwIP socket fd 从 0 开始** —— 若有库假设 fd≥3,留意(参考 qcloud port 的 `LWIP_SOCKET_FD_SHIFT`)。

---

## 附录:关键路径速查

| 用途 | 路径 |
|---|---|
| 工程根 | `fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0/fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0` |
| 顶层编译入口 | `Makefile` → `make ac791n_wifi_story_machine` |
| 子工程 Makefile(要改) | `apps/wifi_story_machine/board/wl82/Makefile` |
| 新模块目录(要建) | `apps/common/LLM/tuya_agentic/` |
| PAL 契约 | agentic-kit `pal/pal.h` |
| PAL 参考实现 | agentic-kit `pal/pal_freertos.c`、`pal/pal_posix.c` |
| 最贴近的集成模板 | agentic-kit `examples/esp-idf/components/agentic_kit/CMakeLists.txt` |
| 同平台 PAL 范例(宿主 API 怎么用) | `apps/common/LLM/volc/src/platform/*.c` |
| 同平台 TCP/TLS 范例 | `apps/common/LLM/tc_iot_twetalk/qcloud_iot_explorer_sdk/port/HAL_TCP_free_rtos.c`、`apps/common/LLM/volc/src/common/volc_tls.c` |
| App 启动联网点(要接入) | `apps/wifi_story_machine/wifi_app_task.c` 的 `WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC` 分支 |
| 文本对话 demo 参考 | agentic-kit `examples/posix/ai/` |

---

## agentic-kit 公共 API 速查

**iot-client**:`iot_init(pal)` / `iot_client_init(&cfg)` / `iot_client_init_on_boarding_with_token(&cfg, token)` / `iot_client_get_session_token(client, NULL, token, len)` / `iot_get_ca_certificate(client, host, port, &ca)`。

**rtc-tcp-client**:`tai_ctx_size()` / `tai_ctx_init(mem, &cfg)` / `tai_connect(ctx)` / `tai_disconnect(ctx)` / `tai_send_text(ctx, s, n)` / `tai_send_audio_start(codec, ch, bits, sr)` + `tai_send_audio_chunk(ctx, pkt, n)` + `tai_send_audio_end(ctx)` / `tai_send_image(ctx, ...)`。

**region 枚举**:`AY`=中国、`AZ`=美西、`EU`=欧洲、`IN`=印度、`SG`=新加坡等;`env`:`PROD`/`PRE`/`TEST`。

---

## 增量:唤醒词子系统(2026-09)

唤醒词"嘿tuya"由涂鸦闭源 KWS 引擎(`tuya_agentic/kws/audio_subsys.a`)常开识别,命中后播应答提示音并开 15s 唤醒窗,**窗内本地 VAD 才允许起轮**;引擎初始化失败自动回退"常听"(与未接入时行为一致)。

接入点:Makefile/cbp 增加 kws 目录(include + `tuya_kws.c`/`kws_cxx_shim.cc` + 链接 `audio_subsys.a`)、`app_config.h` 的 `TUYA_KWS_ENABLE`、`app_music.c` 的应答提示音、资源 `cpu/wl82/tools/audlogo/WakeHeyTuya.mp3`。

引擎标定结论(greedy"前缀窗+跳过"触发机制、5 个必改配置、[27,8] token 指纹、跨句拼装防护)、参数速查与调参/日志指南见 [`WAKEWORD.md`](WAKEWORD.md)。
