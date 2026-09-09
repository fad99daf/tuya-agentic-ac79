# AC791N 杰理 demo 接入 agentic-kit：设备时间校正（ATOP 校时）方案

> 适用范围：基于杰理 AC791N (wl82) AIoT SDK 的 demo 工程。
> **不需要修改 agentic-kit 任何代码**，全部改动都在 demo 侧新增/修改。

## 1. 问题背景

AC791N 方案无 RTC 电池，每次开机 `time()` 从 1970 年起步。而 agentic-kit 内部多处依赖系统时间：

- ATOP 请求签名的时间戳（`atop.c` 里 `time(NULL)`，用于云端验签/防重放）；
- mbedTLS 证书有效期校验（`MBEDTLS_HAVE_TIME`）。

设备时间不对会导致 TLS 证书被判定"未生效/已过期"、云端时间戳校验异常等问题。

## 2. 校时原理：用 ATOP 应答里的 `"t"` 校正

涂鸦云所有 ATOP 接口（`/d.json`）的**应答 JSON 根对象**都统一携带服务器时间字段 `"t"`（unix 秒），例如：

```json
{ "success": true, "t": 1755648000, "result": { ... } }
```

这是 ATOP 协议的信封字段，与具体业务无关——**只要发任意一个 ATOP 请求、收到应答，就能从应答里拿到当前服务器时间**，不需要专门的"校时接口"。

agentic-kit 已经具备利用这一点所需的全部能力（无需改动）：

- 应答解析函数把 `"t"` 填进公开结构体 `atop_base_response_t.t`（在判断 `success` 之前就填了，即使业务失败的应答也带 `t`）；
- `atop_base_request()` 是公开函数，demo 可直接调用；
- 其头文件所在目录已在 demo 编译的 `-I` 路径中。

### 用哪个接口发请求？带宽会不会很大？

由于 `"t"` 在应答**信封（根对象）**里、与业务无关，**任何 ATOP 接口都能用来校时**——建议直接选用产品业务中本来就会调用的**只读查询类**接口，从它的应答里顺带取 `"t"`，不额外引入新接口。

带宽方面无需担心"应答体太大"：`"t"` 在信封里，**不需要业务成功**。给查询接口传空参数，云端只会返回几百字节的业务错误应答（`success:false`，同样带 `"t"`），不会下发大业务数据。示例代码即采用这种方式，每次校时一问一答合计不到 1KB（不含 TLS 握手），每小时一次，带宽开销可忽略。

两条选型红线：

- 不要用**有副作用**的接口（写类接口会改设备云端状态；`thing.ai.agent.token.get` 每次真实签发 AI 会话 token，可能触发频控）；
- 不要传真实业务参数去拉大应答（如完整 schema），空参数的紧凑错误应答就是最优载体。

### 为什么不能只开机校准一次？——需要定时校准

无 RTC 设备的时间是我们用"基准时间 + 开机毫秒数推算"出来的（见第 3 节），晶振有漂移（典型几十 ppm，每小时可漂移亚秒级，长期运行会累积到秒级以上），所以需要周期性重校。方案：

- **开机激活后立即校一次**（首次从 1970 跳到真实时间）；
- **之后每 1 小时重校一次**（低频线程，每次一问一答，开销可忽略）；
- 配合下面的"5 秒死区"落地规则，正常漂移在死区内会被忽略，不会因网络抖动来回拨时间；漂移累积超 5 秒后的一次校准才真正写入。

### 落地规则与 5 秒死区（详解）

拿到云端时间后**不是无条件写入**，而是按以下规则判断：

| 条件 | 结果 |
| --- | --- |
| ① 设备尚未完成过时间同步（首次） | 无条件写入 |
| ② 云端时间 > 本地时间（本地慢了） | 写入（时间向前推进） |
| ③ 本地时间 − 云端时间 > 5 秒（本地快超过 5 秒） | 写入（时间回拨） |
| ④ 0 < 本地时间 − 云端时间 ≤ 5 秒 | **忽略，不调整** |

第 ④ 条就是"5 秒死区"。为什么需要它：

- 无 RTC 设备的本地时间靠"校准基准 + 开机毫秒表"推算，晶振有漂移，单次读数和云端差零点几秒到几秒是常态，属于**正常误差而非真实时间偏差**；
- 如果不设死区、每次都贴着云端写，一次校准后晶振漂移几秒，下次校准又拨回去，时间会**反复回拨**——回拨对定时器、超时判断、日志时序都是灾难；
- 死区把"5 秒以内的本地超前"视为噪声忽略，只有漂移累积超过 5 秒（按每小时亚秒的漂移速度，通常要连续运行十几小时以上）才真正回拨一次。

注意方向性：死区只挡"回拨"。本地时间**落后**于云端（规则②，比如两次校准之间网络中断了很久导致基准陈旧）是不受死区限制、立即向前推进的——向前拨不影响任何已设定的超时/定时逻辑。

举例（假设云端时间 t=1000000）：

| 本地推算时间 | 判定 | 动作 |
| --- | --- | --- |
| 1970（未同步过） | 规则① | 写入 |
| 999998（慢 2 秒） | 规则② | 写入（向前拨 2 秒） |
| 1000003（快 3 秒） | 死区④ | 忽略 |
| 1000009（快 9 秒） | 规则③ | 写入（回拨 9 秒） |

代码实现（`tuya_time_set_unix()`）：

```c
int tuya_time_set_unix(time_t unix_sec)
{
    if (unix_sec <= 0) {
        return -1;
    }
    if (g_base_sec != 0) {                    /* 非首次才做死区判断 */
        time_t diff = tuya_time_get_unix() - unix_sec;
        if (diff > 0 && diff <= 5) {          /* 本地快 0~5 秒：死区，忽略 */
            return -1;
        }
    }
    g_base_sec = unix_sec;
    g_base_ms  = (uint64_t)sys_timer_get_ms();
    return 0;
}
```

## 3. 杰理 SDK 侧的关键限制与思路

杰理 AC79 SDK **没有公开的"设置系统时间"API**：

- `time()` / `gettimeofday()` 在闭源库内是局部符号，SDK 头文件没有提供任何 setter；
- lwip 的 SNTP 组件默认 `SNTP_SET_SYSTEM_TIME` 是空操作，即使用 SNTP 拿到时间也写不进系统时间。

因此本方案的思路是：**在 demo 侧用全局强符号重载 `time()` / `gettimeofday()`**，内部维护一个时间基准：

```
time() 返回值 = g_base_sec（最近一次校准的云端时间）
              + (sys_timer_get_ms() - g_base_ms) / 1000（开机毫秒表流逝量）
```

这样：

- agentic-kit（编译进 demo 的源码）里所有 `time(NULL)` 调用自动拿到校准后的时间；
- 未同步时 `g_base_sec = 0`，退化为 1970 + uptime，与现状行为一致；
- 不需要碰 agentic-kit、不需要碰杰理闭源库。

整体流程：

```
开机 → 激活/直连成功（拿到 devid + local_key）
     → 立即发一次 ATOP 查询，取应答 "t" 写入时间基准（首校）
     → 启动周期线程，每 1 小时重发一次 ATOP 校准
     → 此后全局 time() 返回校准时间（ATOP 签名 / TLS 校验均受益）
```

## 4. 代码改动

### 4.1 新增 `tuya_agentic/tuya_time_sync.h`

```c
#ifndef __TUYA_TIME_SYNC_H__
#define __TUYA_TIME_SYNC_H__

#include <time.h>
#include <stdint.h>

/* 当前校准后的 unix 时间（未同步时 = 1970 + 开机秒数）。 */
time_t tuya_time_get_unix(void);

/*
 * 写入新的基准时间（落地规则）：
 *   - 首次同步无条件写入；
 *   - 云端时间超前本地 -> 写入（向前推进）；
 *   - 本地超前云端超过 5 秒 -> 回拨写入；
 *   - 本地快 0~5 秒 -> 死区，忽略（防抖动）。
 * 返回 0 表示已写入，-1 表示被死区规则忽略。
 */
int tuya_time_set_unix(time_t unix_sec);

/*
 * 发一次只读 ATOP 查询并按应答 "t" 校时。
 * pal       : tai_pal_ac791n() 返回的 PAL
 * devid     : 激活后的设备 ID（iot->devid）
 * local_key : 激活后的本地密钥（iot->local_key）
 * 返回 0 成功，非 0 失败。
 */
int tuya_time_sync_from_atop(const void *pal, const char *devid, const char *local_key);

/*
 * 启动周期校时线程：每 interval_sec 秒重发一次 ATOP 校时（默认 1 小时）。
 * devid/local_key 会被拷贝到内部缓存，之后调用方可释放/反初始化 iot。
 * 幂等：重复调用不会创建多个线程。返回 0 成功。
 */
int tuya_time_sync_periodic_start(const char *devid, const char *local_key,
                                  uint32_t interval_sec);

#endif /* __TUYA_TIME_SYNC_H__ */
```

### 4.2 新增 `tuya_agentic/tuya_time_sync.c`

```c
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "system/timer.h"          /* sys_timer_get_ms           */
#include "system/os/os_api.h"      /* os_time_dly                */
#include "cJSON.h"
#include "pal.h"                   /* pal_t                      */
#include "atop_base.h"             /* atop_base_request/response */

extern const pal_t *tai_pal_ac791n(void);   /* pal_ac791n.c */

/* ------------------------------------------------------------------ */
/* 时间基准：base_sec 为 0 表示尚未同步过（返回 1970+uptime，与无 RTC   */
/* 裸机行为一致）。写入只在首校/周期校时发生，不需要锁。                 */
/* ------------------------------------------------------------------ */
static time_t   g_base_sec = 0;    /* 同步时刻的 unix 秒数           */
static uint64_t g_base_ms  = 0;    /* 同步时刻的 sys_timer_get_ms()  */

time_t tuya_time_get_unix(void)
{
    uint64_t now_ms = (uint64_t)sys_timer_get_ms();
    return (time_t)(g_base_sec + (time_t)((now_ms - g_base_ms) / 1000));
}

int tuya_time_set_unix(time_t unix_sec)
{
    if (unix_sec <= 0) {
        return -1;
    }
    if (g_base_sec != 0) {
        time_t diff = tuya_time_get_unix() - unix_sec;
        /* 本地快 0~5 秒：死区内不调整 */
        if (diff > 0 && diff <= 5) {
            return -1;
        }
    }
    g_base_sec = unix_sec;
    g_base_ms  = (uint64_t)sys_timer_get_ms();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 全局强符号重载：杰理闭源库里的 time/gettimeofday 是局部符号，         */
/* demo 目标文件里的全局定义会在链接时直接生效，agentic-kit（编译进      */
/* demo 的 .c）对 time()/gettimeofday() 的引用全部解析到这里。           */
/* ------------------------------------------------------------------ */
time_t time(time_t *t)
{
    time_t now = tuya_time_get_unix();
    if (t) {
        *t = now;
    }
    return now;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        uint64_t now_ms = (uint64_t)sys_timer_get_ms();
        uint64_t el = now_ms - g_base_ms;
        tv->tv_sec  = (long)(g_base_sec + el / 1000);
        tv->tv_usec = (long)((el % 1000) * 1000);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ATOP 校时：任何 ATOP 应答（哪怕业务失败）的根对象都带 "t"。           */
/* TS_ATOP_API 为示例占位，可替换为产品业务中任一只读查询接口；          */
/* 传空参数即可——业务失败的紧凑错误应答同样携带 t。                      */
/* ------------------------------------------------------------------ */
#define TS_ATOP_API "<任一只读 ATOP 查询接口>"

int tuya_time_sync_from_atop(const void *pal_v, const char *devid, const char *local_key)
{
    const pal_t *pal = (const pal_t *)pal_v;

    if (pal == NULL || devid == NULL || devid[0] == '\0' ||
        local_key == NULL || local_key[0] == '\0') {
        printf("[TUYA-TIME] param error\r\n");
        return -1;
    }

    /* body 按所选接口的入参填空值即可，云端返回紧凑错误应答也带 "t" */
    char post_data[96];
    snprintf(post_data, sizeof(post_data),
             "{\"t\":%lu}",
             (unsigned long)tuya_time_get_unix());

    atop_base_request_t req;
    memset(&req, 0, sizeof(req));
    req.devid     = devid;
    req.key       = local_key;
    req.path      = "/d.json";
    req.timestamp = (uint32_t)tuya_time_get_unix();
    req.api       = TS_ATOP_API;
    req.version   = "1.0";
    req.data      = post_data;
    req.datalen   = strlen(post_data);

    atop_base_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int rt = atop_base_request(pal, &req, &resp);
    if (rt != 0) {
        printf("[TUYA-TIME] atop request fail rt=%d\r\n", rt);
        return -1;
    }

    time_t cloud_t = (time_t)resp.t;
    int has_t = (resp.t != 0);       /* 应答里没有 t（极端情况）也算失败 */
    atop_base_response_free(pal, &resp);

    if (!has_t) {
        printf("[TUYA-TIME] response has no 't'\r\n");
        return -1;
    }

    int applied = (tuya_time_set_unix(cloud_t) == 0);
    printf("[TUYA-TIME] cloud t=%lu local=%lu -> %s\r\n",
           (unsigned long)cloud_t, (unsigned long)tuya_time_get_unix(),
           applied ? "applied" : "dead zone, kept");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 周期校时：缓存凭据，起一个低频线程定时重发 ATOP 校时。                 */
/* ------------------------------------------------------------------ */
static const pal_t *g_ts_pal;
static char         g_ts_devid[32 + 1];
static char         g_ts_key[32 + 1];
static uint32_t     g_ts_interval_sec = 3600;
static int          g_ts_started = 0;

static void *tuya_time_sync_periodic_task(void *arg)
{
    (void)arg;

    /* 首次等一个完整周期再校（启动时调用方刚同步过）；os_time_dly(100)≈1s */
    os_time_dly(g_ts_interval_sec * 100);
    while (1) {
        tuya_time_sync_from_atop(g_ts_pal, g_ts_devid, g_ts_key);
        os_time_dly(g_ts_interval_sec * 100);
    }
    return NULL;
}

int tuya_time_sync_periodic_start(const char *devid, const char *local_key,
                                  uint32_t interval_sec)
{
    if (g_ts_started) {
        return 0;
    }
    if (devid == NULL || devid[0] == '\0' ||
        local_key == NULL || local_key[0] == '\0') {
        return -1;
    }
    if (interval_sec == 0) {
        interval_sec = 3600;
    }
    g_ts_pal = tai_pal_ac791n();
    snprintf(g_ts_devid, sizeof(g_ts_devid), "%s", devid);
    snprintf(g_ts_key,   sizeof(g_ts_key),   "%s", local_key);
    g_ts_interval_sec = interval_sec;
    g_ts_started = 1;

    void *thd = NULL;
    if (g_ts_pal->thread_create(&thd, tuya_time_sync_periodic_task, NULL) != 0) {
        g_ts_started = 0;
        printf("[TUYA-TIME] periodic thread create fail\r\n");
        return -1;
    }
    printf("[TUYA-TIME] periodic sync started, every %us\r\n", g_ts_interval_sec);
    return 0;
}
```

> 编译说明：`atop_base.h`、`pal.h`、`cJSON.h` 所在目录已在 demo 的 Makefile `-I` 路径中，无需新增 include 路径。`pal->thread_create` 在 AC791N PAL 里用 `thread_fork` 创建 24KB 栈线程，足够跑一次 TLS 握手。

### 4.3 修改 `tuya_agentic/tuya_agentic_demo.c`——两处激活路径各挂一次

**（a）开机直连路径**：激活成功、拷出 local_key 之后、`iot_client_deinit(iot)` 之前：

```c
    strncpy(local_key_buf, (const char *)iot->local_key, sizeof(local_key_buf) - 1);

    /* ATOP 校时：借激活后的凭据发一次轻量 ATOP，取应答 "t" 对齐
     * 系统 time()，并启动每小时一次的周期校准。失败不阻塞主流程。 */
    tuya_time_sync_from_atop(pal, (const char *)iot->devid, (const char *)iot->local_key);
    tuya_time_sync_periodic_start((const char *)iot->devid, (const char *)iot->local_key, 3600);
```

文件头部补：

```c
#include "tuya_time_sync.h"
```

**（b）BLE 配网首次激活路径**：`iot_client_init_on_boarding_with_token()` 成功之后：

```c
    iot_client_t *iot = iot_client_init_on_boarding_with_token(&ob, s_main_creds.token);
    if (!iot) { printf("[TUYA] on_boarding_with_token fail\r\n"); return; }
    printf("[TUYA] activated, devid=%s\r\n", iot->devid);
    tuya_time_sync_from_atop(tai_pal_ac791n(), (const char *)iot->devid, (const char *)iot->local_key);
    tuya_time_sync_periodic_start((const char *)iot->devid, (const char *)iot->local_key, 3600);
```

> `tuya_time_sync_periodic_start()` 会把 devid/local_key 拷进内部缓存并只启动一个线程，所以两处路径都调用也只会跑一个校时线程；`iot_client_deinit()` 之后线程照常工作（`atop_base_request()` 每次自建连接，不依赖 iot 对象）。

### 4.4 修改 `apps/wifi_story_machine/board/wl82/Makefile`——源文件列表

在 `tuya_agentic_demo.c` 一行后追加：

```make
	../../../../apps/common/LLM/tuya_agentic/tuya_time_sync.c \
```

## 5. 验证方法

1. 烧录后看串口日志，激活成功后应出现：

   ```
   [TUYA-TIME] cloud t=1755648000 local=55 -> applied
   [TUYA-TIME] periodic sync started, every 3600s
   ```

2. 运行 1 小时后应再次出现 `[TUYA-TIME] cloud t=... -> applied` 或 `dead zone, kept`（晶振正常漂移小于 5 秒时是后者，符合预期）。
3. 临时在主循环打印 `time(NULL)` 确认不再从 1970 起步、且随运行时间递增。

## 6. 已知边界

- **校时频度**：1 小时一次是默认值，可通过 `tuya_time_sync_periodic_start()` 第三个参数调整。每次校时是一问一答的 HTTPS 请求，开销可忽略。
- **链接风险**：`time()` 重载依赖"闭源库中该符号为局部符号"这一事实（已在 AC79NN_SDK_V1.2.0 的符号表中确认）。若个别 SDK 版本报 `time` 重复定义，去掉 `gettimeofday` 重载只保留 `time()` 再试。
- **请求侧的 `t` 不用于校时**：请求 URL/body 里的 `t` 是云端验签/防重放用的，校时只取**应答**中的 `t`。
