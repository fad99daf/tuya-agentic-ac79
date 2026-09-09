# 天气获取功能 集成说明（涂鸦云 ATOP 路线）

本文档说明如何在本工程（AC79 WiFi Story Machine）中新增“获取天气”功能。当前工程已启用涂鸦 Agentic（`app_config.h:229` 的 `CONFIG_TUYA_AGENTIC_ENABLE`），设备通过 UUID/AUTHKEY 配网激活后拿到 `devid` / `secret_key`，与涂鸦云的通信走 **ATOP 设备协议**。天气数据同样通过涂鸦云的 ATOP 接口获取，本文列出接口并给出可落地的集成方案。

工程当前没有任何天气逻辑，此功能为新增。

---

## 一、云端接口

### 接口标识
天气数据通过涂鸦云 ATOP 设备接口获取（与设备其它云端请求同一套通道 `/d.json`）：

| 项 | 值 |
| --- | --- |
| API 名（`a`） | `thing.weather.get` |
| 版本（`v`） | `2.0` |
| 路径（`path`） | `/d.json` |
| 方法 | HTTPS POST |
| 主机（`host`） | 中国区 `a1.tuyacn.com:443`（其它区见下表） |
| 请求体（明文，AES-GCM 加密后作为 `data=` 提交） | `{"codes":[<天气数据编码列表>], "t":<POSIX 时间戳>}` |
| 签名 | 用设备 `secret_key` 对 URL 参数做 MD5 签名（`atop_base_request` 内部自动完成） |
| 鉴权身份 | `devId=<devid>`（激活后获得） |

> 说明：`thing.weather.get` 是**设备侧 ATOP 接口**，用设备的 `devid` + `secret_key` 签名调用，本工程的涂鸦组件恰好只支持这套设备协议，因此可以直接复用。它不是 OpenAPI（`client_id/secret → token → /v1.0/...`），本工程不走那条路。

### 区域主机（根据设备激活区域选择，`iot_config_defaults.h`）
| 区域 | Host |
| --- | --- |
| 中国 CN（默认） | `a1.tuyacn.com` |
| 美国 US | `a1.tuyaus.com` |
| 欧洲 EU | `a1.tuyaeu.com` |
| 印度 IN | `a1.tuyain.com` |
| 新加坡 SG | `a1-sg.iotbing.com` |

实际使用时不必硬编码：可用 `iot_region_to_host(region, env)`（`iot_dns.h:142`），或在有 `iot_client_t*` 时用 `iot_client_resolve_atop_host()`（`iot_dp_internal.h:56`）解析。

### 请求体里的 codes
`codes` 是要请求的天气数据项编码数组（涂鸦 MCU 标准协议 wifi-weather 定义），常用项如：当前温度、湿度、天气类型、风向/风速、今明数日高低温、空气质量、日出日落、城市等。具体编码取值请以涂鸦「MCU 标准协议 / wifi-weather」文档为准，按需要的字段填入数组即可。

**当前天气 vs 未来天气（预报）用的是同一个接口 `thing.weather.get`，区别只在 codes：**
- 当前天气：codes 里放当前项（如 `w.temp` / `w.humidity` / `w.conditionNum`），返回单值。
- 未来天气（预报）：codes 里放带天数的预报项，涂鸦约定用 `.d{N}` 后缀表示第 N 天（`d1`=今天，`d2`=明天，最多到 `d7`）。例如请求未来 3 天的天气类型和高低温：`w.conditionNum.d1 / w.conditionNum.d2 / w.conditionNum.d3`、`w.thigh.d1..d3`、`w.tlow.d1..d3`。返回时每个带天数的 code 对应一个值，按天组装即可。

> 注意：中国大陆的预报不包含温度/气压等部分字段（属云端约定），若某天某项没返回，按缺省处理即可。具体哪些预报项、后缀写法以涂鸦 wifi-weather 协议文档为准。

### 返回结构
ATOP 返回经 AES-GCM 解密后是 JSON，天气字段在 `result` 下（按 `codes` 请求项返回对应值）。典型字段（以云端实际返回为准）：

- 当前天气：`temp`（温度）、`humidity`（湿度）、`conditionNum`（天气类型编码）、`realFeel`（体感）、`pressure`（气压）、`uvi`（紫外线）
- 风力：`windDir`（风向）、`windSpeed`（风速）、`windLevel`（风力等级，中国区）
- 高低温：`thigh` / `tlow`
- 空气质量：`aqi` / `pm25` / `pm10` / `o3` / `no2` / `co` / `so2` / `rank`（rank 中国区）
- 日出日落：`sunrise` / `sunset`
- 城市：`province` / `city` / `area`

> 注意：天气数据准确与否取决于设备在涂鸦 App 里设置的地理位置，需在 App 中给设备设好位置。

---

## 二、工程已有的可复用基础设施

### 1. ATOP 通用签名请求原语（核心）
本工程已内置完整、可用、带签名与 AES-GCM 加密的 ATOP 客户端，天气功能直接复用它即可，无需自己实现签名。

- 头文件：`apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/atop_base.h`
- 通用请求接口（`atop_base.h:71`）：
  ```c
  int atop_base_request(const pal_t *pal,
                        const atop_base_request_t *request,
                        atop_base_response_t *response);
  void atop_base_response_free(const pal_t *pal, atop_base_response_t *response);
  ```
- 请求结构 `atop_base_request_t`（`atop_base.h:29`）：填 `path="/d.json"`、`api`、`version`、`devid`、`key`(=secret_key)、`data`(明文 JSON)、`datalen`、`host`、`port`、`timestamp` 即可。函数内部自动完成 URL 参数拼装、MD5 签名、AES-GCM 加密、HTTPS POST、响应解密与 cJSON 解析。
- 响应结构 `atop_base_response_t`（`atop_base.h:47`）：`success`、`result`（cJSON 子树）、`t`。用完必须 `atop_base_response_free`。

**最佳复制模板**：`atop_ai_token_get()`（`apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/atop.c:492`）。它就是一个约 40 行的 `atop_base_request` 薄封装：用 `devid`+`secret_key`，用 cJSON 构造请求体，调用 `atop_base_request`，从 `result` 读结果。天气封装照抄这个形状、把 `api` 换成 `thing.weather.get`、`version` 换成 `2.0`、请求体换成 `{"codes":[...],"t":...}` 即可。

### 2. 设备身份 / 凭据（devid、secret_key、region）
运行期身份在 `iot_client_t`（`iot_client.h:141`）：`char devid[32]`、`char secret_key[32]`、`iot_region_t region`、`iot_env_t env`（字段公开，直接读）。

持久化存储在杰理 syscfg VM（见 `tuya_agentic_demo.c:1208` 一带的 VM 索引定义与 `:2367-2420` 的读取）：
- `VM_TUYA_DEVID_IDX (176)` → devid
- `VM_TUYA_SECRET_IDX (177)` → secret_key
- `VM_TUYA_LOCALKEY_IDX (178)` → local_key
- `VM_TUYA_REGION_IDX (183)` → region

天气功能取 `devid`/`secret_key` 有两种方式：持有 `iot_client_t*` 时直接读字段；否则用 `syscfg_read(VM_TUYA_DEVID_IDX/…SECRET_IDX, ...)` 读出来（读法参考 `tuya_agentic_demo.c:2367`）。

### 3. PAL 适配器
`atop_base_request` 需要 `const pal_t *pal`，本工程用 `tai_pal_ac791n()` 获取（声明 `apps/common/LLM/tuya_agentic/tuya_agentic.h:11`，用例 `tuya_agentic_demo.c:1050`）。

### 4. 联网状态 / 线程 / 定时器
- 联网判断：`wifi_get_sta_connect_state()`（`include_lib/net/wifi/wifi_connect.h:430`），等它返回 `WIFI_STA_NETWORK_STACK_DHCP_SUCC`。
- 线程：`thread_fork(name, prio, stk_size, q_size, pid, func, parm)`（`include_lib/system/os/os_api.h:804`，`stk_size` 单位为 4 字节字）。
- 定时器：周期 `sys_timer_add(priv, cb, msec)`、单次 `sys_timeout_add(...)`、删除 `sys_timer_del(id)`（`include_lib/system/timer.h`）。

> 重要：ATOP 请求是**阻塞的 HTTPS 调用**，必须放在 `thread_fork` 出来的工作线程里跑；定时器/事件回调里只做“通知”，不要直接发请求。

---

## 三、集成步骤

1. **前提**：设备已通过涂鸦配网激活（`CONFIG_TUYA_AGENTIC_ENABLE` 已开），flash 里已有 `devid`/`secret_key`（`tuya_agentic_demo` 首次配网后会写入 VM 176/177），并在涂鸦 App 里给设备设了地理位置。
2. **加功能开关**：在 `apps/wifi_story_machine/include/app_config.h` 增加 `#define CONFIG_WEATHER_ENABLE 1`。
3. **新增天气封装 + 调用文件**：在 `apps/wifi_story_machine/` 下新建 `weather_get.c`（内容见下）。
4. **加入编译**：把 `weather_get.c` 按现有写法加进 `apps/wifi_story_machine/board/wl82/Makefile`（该 Makefile 已包含 iot-client 的 include 路径 `-I .../iot-client/src` 与 cJSON、wifi、timer 路径，无需额外添加）。
5. **联网后启动**：在应用初始化或联网成功后调用 `weather_start()`（示例内部会等联网、周期刷新）。

---

## 四、示例代码（可直接落地）

`apps/wifi_story_machine/weather_get.c`：

```c
#include "system/includes.h"
#include "wifi/wifi_connect.h"
#include "app_config.h"
#include "cJSON.h"

/* 涂鸦 iot-client 头（include 路径已在本板 Makefile 中） */
#include "atop_base.h"          /* atop_base_request / atop_base_request_t */

#if defined(CONFIG_WEATHER_ENABLE) && CONFIG_WEATHER_ENABLE

/* 本工程 PAL 与 syscfg VM 索引（与 tuya_agentic_demo 一致） */
extern const pal_t *tai_pal_ac791n(void);
#define VM_TUYA_DEVID_IDX     176
#define VM_TUYA_SECRET_IDX    177
#define VM_TUYA_REGION_IDX    183

#define WEATHER_API           "thing.weather.get"
#define WEATHER_API_VERSION   "2.0"
#define WEATHER_REFRESH_MS    (30 * 60 * 1000)   /* 30 分钟刷新一次 */

#define WEATHER_FORECAST_DAYS 3   /* 预报天数(1~7) */

/* 当前天气 codes:按涂鸦 wifi-weather 协议实际编码填写(占位,请按需替换) */
#define WEATHER_CODES_CURRENT "\"w.temp\",\"w.humidity\",\"w.conditionNum\""

/* 未来天气(预报) codes:带 .dN 后缀表示第 N 天(d1=今天...d7)。
 * 此处请求未来 3 天的天气类型与高低温,请按需增减字段/天数。 */
#define WEATHER_CODES_FORECAST \
    "\"w.conditionNum.d1\",\"w.thigh.d1\",\"w.tlow.d1\"," \
    "\"w.conditionNum.d2\",\"w.thigh.d2\",\"w.tlow.d2\"," \
    "\"w.conditionNum.d3\",\"w.thigh.d3\",\"w.tlow.d3\""

/* 解析后的当前天气(字段名以云端实际返回为准) */
struct weather_info {
    int  temp;
    int  humidity;
    int  condition;   /* 天气类型编码 */
};

/* 解析后的单日预报 */
struct weather_forecast_day {
    int  condition;   /* 天气类型编码 */
    int  temp_high;
    int  temp_low;
};

static u16 s_weather_timer;
static volatile u8 s_weather_busy;

/* 根据激活区域返回 ATOP 主机；读不到兜底中国区 */
static const char *weather_host_by_region(void)
{
    u8 region = 0; /* AY(中国)=0 */
    if (syscfg_read(VM_TUYA_REGION_IDX, &region, 1) <= 0) {
        region = 0;
    }
    switch (region) {
    case 3:  return "a1.tuyaeu.com";   /* EU  */
    case 5:  return "a1.tuyain.com";   /* IN  */
    case 6:  return "a1-sg.iotbing.com"; /* SG */
    default: return "a1.tuyacn.com";   /* CN 及默认 */
    }
}

/* 取整数字段:支持带 .dN 后缀的预报字段名 */
static int weather_get_int(cJSON *result, const char *name, int *out)
{
    cJSON *it = cJSON_GetObjectItem(result, name);
    if (it && cJSON_IsNumber(it)) {
        *out = it->valueint;
        return 0;
    }
    return -1;
}

/* 通用:发一次 thing.weather.get(codes 由调用方给),result 回调给上层解析。
 * 阻塞式 ATOP HTTPS,必须运行在工作线程里。 */
static int weather_request(const char *codes,
                           int (*on_result)(cJSON *result, void *ud), void *ud)
{
    const pal_t *pal = tai_pal_ac791n();
    char devid[32] = {0}, secret[32] = {0};

    if (syscfg_read(VM_TUYA_DEVID_IDX, devid, sizeof(devid)) <= 0 ||
        devid[0] == 0 || devid[0] == 0xFF) {
        printf("weather: no devid, device not provisioned\n");
        return -1;
    }
    syscfg_read(VM_TUYA_SECRET_IDX, secret, sizeof(secret));

    /* 构造请求体 {"codes":[...],"t":<ts>} */
    uint32_t ts = (uint32_t)time(NULL);
    int need = snprintf(NULL, 0, "{\"codes\":[%s], \"t\":%u}", codes, ts) + 1;
    char *post_data = (char *)malloc(need);
    if (!post_data) {
        return -1;
    }
    snprintf(post_data, need, "{\"codes\":[%s], \"t\":%u}", codes, ts);

    atop_base_request_t req = {
        .path      = "/d.json",
        .key       = secret,          /* 用设备 secret_key 签名+加密 */
        .api       = WEATHER_API,
        .version   = WEATHER_API_VERSION,
        .devid     = devid,
        .timestamp = ts,
        .data      = (void *)post_data,
        .datalen   = strlen(post_data),
        .host      = weather_host_by_region(),
        .port      = 443,
        .cacert    = NULL,
    };

    atop_base_response_t resp = {0};
    int rt = atop_base_request(pal, &req, &resp);
    free(post_data);
    if (rt != 0) {
        printf("weather: atop request fail, rt=%d\n", rt);
        atop_base_response_free(pal, &resp);
        return -1;
    }

    int ret = (resp.result && on_result) ? on_result(resp.result, ud) : -1;
    atop_base_response_free(pal, &resp);
    return ret;
}

/* --- 当前天气 --- */
static int on_current_result(cJSON *result, void *ud)
{
    struct weather_info *info = (struct weather_info *)ud;
    weather_get_int(result, "temp",         &info->temp);
    weather_get_int(result, "humidity",     &info->humidity);
    weather_get_int(result, "conditionNum", &info->condition);
    return 0;
}

static int weather_fetch_current(void)
{
    struct weather_info info = {0};
    if (weather_request(WEATHER_CODES_CURRENT, on_current_result, &info) != 0) {
        return -1;
    }
    printf("weather(now): temp=%d humidity=%d condition=%d\n",
           info.temp, info.humidity, info.condition);
    /* TODO: 把 info 交给显示 / 播报 / 业务逻辑 */
    return 0;
}

/* --- 未来天气(预报) --- */
static int on_forecast_result(cJSON *result, void *ud)
{
    struct weather_forecast_day *days = (struct weather_forecast_day *)ud;
    char name[24];
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        int d = i + 1;   /* d1..dN */
        snprintf(name, sizeof(name), "conditionNum.d%d", d);
        weather_get_int(result, name, &days[i].condition);
        snprintf(name, sizeof(name), "thigh.d%d", d);
        weather_get_int(result, name, &days[i].temp_high);
        snprintf(name, sizeof(name), "tlow.d%d", d);
        weather_get_int(result, name, &days[i].temp_low);
    }
    return 0;
}

static int weather_fetch_forecast(void)
{
    struct weather_forecast_day days[WEATHER_FORECAST_DAYS] = {0};
    if (weather_request(WEATHER_CODES_FORECAST, on_forecast_result, days) != 0) {
        return -1;
    }
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        printf("weather(d%d): condition=%d high=%d low=%d\n",
               i + 1, days[i].condition, days[i].temp_high, days[i].temp_low);
    }
    /* TODO: 把 days[] 交给显示 / 播报 / 业务逻辑 */
    return 0;
}

/* 工作线程:先等联网,再拉当前天气 + 未来预报 */
static void weather_task(void *priv)
{
    while (wifi_get_sta_connect_state() != WIFI_STA_NETWORK_STACK_DHCP_SUCC) {
        os_time_dly(100);   /* 1s 查一次 */
    }
    s_weather_busy = 1;
    weather_fetch_current();
    weather_fetch_forecast();
    s_weather_busy = 0;
}

/* 定时器回调:只“通知”,不在此做阻塞请求 */
static void weather_timer_cb(void *priv)
{
    if (s_weather_busy) {
        return;
    }
    thread_fork("weather_task", 10, 1024 * 10, 0, NULL, weather_task, NULL);
}

/* 对外入口 */
void weather_start(void)
{
    thread_fork("weather_task", 10, 1024 * 10, 0, NULL, weather_task, NULL);
    if (s_weather_timer == 0) {
        s_weather_timer = sys_timer_add(NULL, weather_timer_cb, WEATHER_REFRESH_MS);
    }
}

void weather_stop(void)
{
    if (s_weather_timer) {
        sys_timer_del(s_weather_timer);
        s_weather_timer = 0;
    }
}

#endif /* CONFIG_WEATHER_ENABLE */
```

调用处（应用初始化或联网成功后加一行）：

```c
extern void weather_start(void);
weather_start();
```

---

## 五、注意事项

- **必须已激活**：`thing.weather.get` 用 `devid`+`secret_key` 签名，设备未经涂鸦配网激活（flash 无 devid）时无法调用。示例里已判空。
- **codes 与返回字段以协议为准**：示例的 codes（`WEATHER_CODES_CURRENT` / `WEATHER_CODES_FORECAST`）和解析字段（`temp/humidity/conditionNum`、`conditionNum.dN/thigh.dN/tlow.dN`）是占位，请对照涂鸦「MCU 标准协议 / wifi-weather」文档，填入你真正需要的编码，并按实际返回的 `result` 结构解析。**尤其是预报字段的 `.dN` 后缀写法与返回 key，务必按协议核对**——若你的协议里预报项的请求编码或返回 key 与示例不同，改 `WEATHER_CODES_FORECAST` 和 `on_forecast_result` 里的字段名即可。
- **当前与未来同一接口**：当前天气和未来预报都用 `thing.weather.get`，只是 codes 不同；示例里 `weather_fetch_current()` 与 `weather_fetch_forecast()` 各发一次请求。若想省一次请求，也可把两组 codes 合并到一次请求里再分别解析。
- **阻塞调用放工作线程**：ATOP 是阻塞 HTTPS，务必在 `thread_fork` 线程里执行；定时器/WiFi 事件回调只负责触发。
- **线程栈**：示例给 `1024*10`（4 字节字为单位）；HTTPS + TLS + JSON 解析对栈要求较高，若返回体大或解析层级深，注意观察是否溢出，必要时在 `apps/wifi_story_machine/app_main.c` 的 forked 任务栈登记表（约 50-66 行）按现有方式补一条。
- **区域主机**：示例按 VM 里的 region 选主机；也可直接用 `iot_region_to_host()`。
- **App 需设位置**：数据准确性依赖涂鸦 App 里给设备设置的地理位置。
- **地区差异**：`windLevel`、AQI `rank` 等字段仅中国区返回。

---

## 六、参考文件索引

| 用途 | 路径 |
| --- | --- |
| ATOP 通用签名请求接口/结构 | `apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/atop_base.h` |
| 天气封装最佳复制模板（`atop_ai_token_get`） | `apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/atop.c:492` |
| 设备身份结构 `iot_client_t`（devid/secret_key/region） | `apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/include/iot_client.h:141` |
| 凭据持久化 VM 索引与读写 | `apps/common/LLM/tuya_agentic/tuya_agentic_demo.c:1208,2367,2507` |
| 区域→主机 / 主机解析 | `iot_dns.h:142`、`iot_dp_internal.h:56`、`iot_config_defaults.h:22` |
| PAL 获取 | `apps/common/LLM/tuya_agentic/tuya_agentic.h:11`（用例 `tuya_agentic_demo.c:1050`） |
| 联网状态 | `include_lib/net/wifi/wifi_connect.h:430` |
| cJSON | `include_lib/net/cJSON_common/cJSON.h` |
| 线程 / 延时 | `include_lib/system/os/os_api.h:804` |
| 定时器 | `include_lib/system/timer.h` |
| 功能开关 / 涂鸦三元组 | `apps/wifi_story_machine/include/app_config.h`（`:229` 开关，`:72-77` UUID/AUTHKEY/token） |
| 编译加入 | `apps/wifi_story_machine/board/wl82/Makefile` |
