# agentic-kit 恢复出厂设置说明

## 结论

agentic-kit 当前**没有一个一键式的 `iot_factory_reset()` API**，但恢复出厂所需的两个基础能力已经具备：

| 场景 | 现状 | 使用方式 |
| --- | --- | --- |
| App / 云端下发解绑、恢复出厂 | 已支持 | 注册 `reset_callback`，接收 MQTT protocol 11 通知 |
| 设备端主动恢复出厂 | 可通过通用 ATOP 调用 | `iot_atop_call()` 调用 `tuya.device.reset` v4.0 |
| 清理设备本地凭据和业务数据 | SDK 不自动处理 | 由产品应用负责 |

因此，杰理芯片侧可以在现有 agentic-kit 能力上完成恢复出厂流程；如果希望获得类似 TuyaOpen `tuya_iot_reset()` 的一键封装，可以在 agentic-kit 中继续增加具名接口。

> 请确认客户使用的 agentic-kit 版本已包含 `reset_callback` 和 `iot_atop_call()`。旧版本需要先升级，或按同样协议自行补齐。

## 1. 云端下发解绑 / 恢复出厂

当用户在 App 侧移除设备，或云端下发恢复出厂时，设备会收到 MQTT protocol 11 通知。agentic-kit 会将其分类后回调应用：

```c
typedef enum {
    IOT_RESET_REMOTE_UNBIND = 0,  /* 用户移除设备，可重新绑定 */
    IOT_RESET_REMOTE_FACTORY,     /* 云端下发恢复出厂 */
} iot_reset_type_t;

typedef void (*iot_reset_callback_t)(iot_reset_type_t type, void *user_data);
```

注册示例：

```c
static volatile int g_reset_requested = 0;

static void on_reset(iot_reset_type_t type, void *user_data)
{
    (void)type;
    (void)user_data;

    /* 回调运行在 MQTT 收包线程，只置标志，不做耗时操作 */
    g_reset_requested = 1;
}

iot_client_config_t cfg = {
    /* ... */
    .reset_callback = on_reset,
};
```

推荐处理流程：

1. `on_reset()` 中只设置复位标志；
2. 应用主循环检测到标志后退出 `iot_client_process()` 循环；
3. 断开 MQTT，释放 IoT client；
4. 清理本地激活凭据、schema、DP 状态和需要恢复出厂的业务数据；
5. 重启设备，或重新进入配网模式。

**不要在 `reset_callback` 里直接调用 `iot_client_deinit()` 或断开 MQTT。** 该回调仍在 MQTT 收包路径上执行，立即释放连接可能造成正在使用的上下文被销毁。正确做法是回到应用主循环后统一处理。

## 2. 设备端主动恢复出厂

长按按键、检测到多次异常重启等设备侧触发恢复出厂时，需要先通知云端解除绑定，再清理本地数据。TuyaOpen 的主动恢复出厂使用 ATOP 接口：

```text
API:     tuya.device.reset
Version: 4.0
Body:    {"t":<当前 Unix 时间戳，单位秒>}
```

agentic-kit 可以通过通用 ATOP 入口直接调用：

```c
#include "iot_atop.h"

#include <stdio.h>
#include <time.h>

static int app_request_cloud_factory_reset(iot_client_t *client)
{
    char body[32];
    snprintf(body, sizeof(body), "{\"t\":%ld}", (long)time(NULL));

    iot_atop_request_t req = {
        .api     = "tuya.device.reset",
        .version = "4.0",
        .data    = body,
    };

    iot_atop_response_t resp = {0};
    int rc = iot_atop_call(client, &req, &resp);

    if (rc == OPRT_ATOP_BUSINESS_ERROR) {
        /* 已到达云端，但云端拒绝；error_code / error_msg 用于定位原因 */
        printf("factory reset rejected: %s(%s)\n",
               resp.error_code, resp.error_msg);
    } else if (rc != OPRT_OK) {
        /* DNS、TLS、HTTP 等传输层失败，可按产品策略重试 */
        printf("factory reset transport error: %d\n", rc);
    }

    iot_atop_response_free(client, &resp);
    return rc;
}
```

云端返回 `OPRT_OK` 后，再执行本地清理：

```text
触发恢复出厂
    ↓
调用 tuya.device.reset v4.0
    ↓
确认 OPRT_OK
    ↓
退出业务循环 / 停止 MQTT 收包
    ↓
iot_client_deinit()
    ↓
擦除本地激活数据和业务数据
    ↓
重启或进入配网模式
```

**不要先擦除本地凭据再调用云端 reset。** `devid`、`secret_key` 是设备向云端证明身份并完成解绑的必要信息，提前清除后设备将无法通知云端解除绑定。

## 3. 本地需要清理的数据

agentic-kit 不管理产品存储，恢复出厂时应用至少应清理以下激活相关数据：

- `devid`
- `secret_key`
- `local_key`
- `schema_id`
- DP schema
- DP 状态快照
- 已持久化的云端 endpoint 信息
- 产品定义中属于恢复出厂范围的业务数据

如果动态 CA、网络参数等是为了加速重连而缓存的，可以按产品策略保留或清除；内置根证书、设备证书、产品配置等不应误删。

## 4. 失败策略建议

设备端主动恢复出厂建议采用“云端优先”的策略：

1. 先调用 `tuya.device.reset` v4.0；
2. 网络或传输失败时重试，重试期间保留本地凭据；
3. 成功后再清理本地数据并重启；
4. 如果多次失败，提示用户检查网络，或由产品明确选择本地强制复位。

本地强制复位会先清除设备凭据，设备可重新配网，但云端可能仍保留旧绑定关系。此时需要用户在 App 中移除旧设备，或等设备后续具备网络时再补云端解绑。是否允许这条路，应由产品定义决定。

## 5. 杰理平台注意事项

- **实时时钟必须可用**：`tuya.device.reset` 的请求体和 ATOP 签名都包含时间戳，设备时间偏差过大可能被云端拒绝。杰理侧需确保启动后能获取可信时间。
- **不要在 `reset_callback` 中做网络请求**：回调只置标志，ATOP 调用和存储擦除都放到应用主流程处理。
- **主动 reset 与远端通知要幂等**：主动调用 `tuya.device.reset` 后，设备仍可能收到 protocol 11 通知。两条路径应汇入同一个“待清理”标志，避免重复擦除或重复重启。
- **以返回码判断云端结果**：`OPRT_OK` 表示云端接受；`OPRT_ATOP_BUSINESS_ERROR` 表示云端拒绝；不要依赖 `resp.result` 的具体形态判断成败。
- **保持单线程调用模型**：所有 MQTT 相关操作保持在应用所在的 IoT process 线程，不要在回调或异步 worker 中直接操作同一个 client。

## 6. 给客户的建议答复

> agentic-kit 目前没有一键式恢复出厂 API，但已经提供完成该流程所需的能力：云端下发场景可注册 `reset_callback` 接收 protocol 11；设备端主动恢复出厂可通过 `iot_atop_call()` 调用 `tuya.device.reset` v4.0。收到云端成功返回后，由应用断开连接、擦除本地激活数据和业务数据，并重启或进入配网模式。若需要降低接入成本，我们可以进一步在 agentic-kit 中封装 `iot_factory_reset()`，统一处理云端通知、主动 reset 和本地清理回调。
