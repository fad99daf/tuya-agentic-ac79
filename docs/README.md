# 文档索引

本目录收录 tuya-agentic-ac79 的集成文档与参考实现说明。

## 已合入功能的文档

对应功能已在本仓对接代码中实现,文档描述其架构、原理与调参。

| 文档 | 内容 |
| --- | --- |
| [INTEGRATION.md](INTEGRATION.md) | 集成架构与原理:agentic-kit 移植到 AC791N 的整体设计、数据链路、PAL 对接 |
| [WAKEWORD.md](WAKEWORD.md) | 唤醒词子系统("你好涂鸦"+"嘿涂鸦"):v2 引擎、官方 token、参数、调参、日志 |
| [CHANGES.md](CHANGES.md) | 完整改动清单:新增文件、改动的 SDK 文件、逐项说明 |

## 参考实现说明(未合入代码)

以下功能**尚未合入本仓代码**,文档为可落地的参考实现说明,需要对应功能的开发者可照文档自行集成。

| 文档 | 内容 |
| --- | --- |
| [WEATHER.md](WEATHER.md) | 天气获取功能集成说明:复用 ATOP `thing.weather.get` 接口拉取当前/未来天气,含可直接落地的示例代码 |
| [FACTORY-RESET.md](FACTORY-RESET.md) | 恢复出厂设置说明:云端下发解绑(`reset_callback` / MQTT protocol 11)与设备端主动恢复出厂(`tuya.device.reset` v4.0),含本地清理与失败策略 |
| [TIME-SYNC.md](TIME-SYNC.md) | 设备时间校正方案:无 RTC 设备用 ATOP 应答里的 `t` 字段校时,全局重载 `time()`/`gettimeofday()`,含 5 秒死区落地规则与示例代码 |

## 云端接口参考

`thing.ai.*` 设备侧 ATOP 接口规格(官方对外文档,2026-09-14 版),调用通道与 [WEATHER.md](WEATHER.md) 描述的 `atop_base_request` 相同。

| 文档 | 内容 |
| --- | --- |
| [AI-AGENT-API.md](AI-AGENT-API.md) | C端智能体业务开放能力-设备侧接口,共 14 个:智能体配置(设备详情/语言列表)、角色服务(模板/自定义角色增删改查、角色绑定)、聊天服务(历史记录/记忆/上下文清理)、音色服务(官方音色分页) |
