# 上游基线对照锚点(UPSTREAM-BASE)

> 用途:记录本仓 vendored agentic-kit 各子模块对齐的上游 SHA、vendor 时的机械适配、本地补丁台账。re-vendor 升级(阶段 1/2)时以本文件为对照锚点,防止"不知道自己基于哪天"再次发生。
> 上游仓库:本地克隆 `D:\code\agentic-kit`。首次建立:2026-09-30(阶段 0 热修落地时)。
> 完整对比分析与语义分歧裁决表:`D:\code\agentic-kit上游对齐报告-2026-09-29.md`。

## 1. 基线与当前对齐状态

| 子模块 / 文件 | vendor 基线 | 当前状态 |
|---|---|---|
| modules/iot-client(除下述例外) | `49ab2af`(2026-07-17) | 基线 + 四轮本地补丁(见 §3) |
| modules/iot-client/src/**mqtt.c** | `49ab2af` | **== 上游 `0a81046`(2026-08-26)整文件**(阶段 0,2026-09-30) |
| common/**tls.c** | `49ab2af` | 基线 + 本地诊断 + 上游 `957d1c4` NST 守卫(阶段 0) |
| modules/rtc-tcp-client | `49ab2af` | 基线 + ConnectionRefresh / 长流 v3 等本地补丁 |
| common(其余) | `49ab2af` | 基线 + 少量本地补丁 |
| modules/tuya-ble(仅配网 3 文件) | `49ab2af` | 基线;**阶段 3 前钉住不刷新** |
| third_party/coreHTTP、coreMQTT | `49ab2af` 同步快照 | 未改动 |

与上游 blob 比对命令示例(注意行尾):

```
diff --strip-trailing-cr <(git -C D:/code/agentic-kit show 0a81046:modules/iot-client/src/mqtt.c) \
     apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/mqtt.c
```

## 2. vendor 时的机械适配(re-vendor 重放清单)

| 适配 | 说明 |
|---|---|
| `common/log.h` → `tai_log.h` | 避杰理 SDK log.h 符号冲突;仅改文件名与 include |
| `iot_ota_*` → `tuya_iot_ota_*` | 同名符号冲突;跨 atop.c / iot_ota.c / iot_client*.c 及头文件 |
| CRLF 行尾 | overlay 与主仓副本均为 CRLF;与上游 blob 比对必须 `--strip-trailing-cr` |

阶段 1 起预计新增的重放项(依 2026-09-29 报告):`%zu`→`%u`、mbedtls 头文件引号路径、AC79 三件套适配。

## 3. 本地补丁台账(overlay 提交 → 内容)

| overlay 提交 | 日期 | 内容 | 主要落点 |
|---|---|---|---|
| 9992fe3 | 09-10 | ConnectionRefresh(attr23/24/25 + 30min 定时,治 TAI ~1h 回收) | rtc-tcp-client 多文件;上游 master 永无对应,**永久本地维护** |
| 85740c6 | 09-21 | SG 域名支持 | iot_config_defaults.h 等 |
| 7b42827 | 09-21 | 长流模式 v3 | rtc-tcp-client / tai 协议 |
| 12e90d0 | 09-29 | OTA 双 API / protocol15 路由 / 进度上报 / iot_ota_verify.c | atop.c / iot_ota.c / iot_client* |
| `align-phase0` | 09-30 | 阶段 0 热修(NST + EOF/link_dead) | tls.c / mqtt.c |

另:tls.c 内嵌 tls_write 慢链路诊断(海外弱网定位,约 15 行,无上游对应,升级时保留)。

## 4. 分阶段对齐计划(摘要)

- **阶段 0(已完成 2026-09-30)**:热修 tls.c NST(957d1c4)+ mqtt.c EOF/link_dead(整文件=0a81046)。tag `align-phase0`。
- **阶段 1**:re-vendor 至 `5f4d845`(重构前顶点)。收:DNS UE/WE 查询码、ATOP 缓冲可配置 + business error 契约、volatile 密钥清零、激活 v2.0、iot_client_reset v5.0、iot_atop_call、小帧合并、多模态等;重放机械适配 + §3 全部补丁;protocol15 换上游 config 字段版、iot_ota_verify.c 换上游版;Makefile/.cbp 增 iot_atop.c。tag `align-phase1-5f4d845`。
- **阶段 2**:re-vendor 至 `48e3c0c`(0.5.0)。AGENTIC_KIT_ 旋钮前缀 + 编译期日志(经 `AGENTIC_KIT_LOG` 宏重映射保住运行时调级)、协议 9000 MQTT AI 控制通道、TCP 接收背压(pal_ac791n.c 需补 `sleep_ms`)、服务器时间过滤打断(与 tai_current_event_id 并行双判后收编)。**KWS 联调开始前建议完成**。tag `align-phase2-0.5.0`。
- **阶段 3(可选)**:tuya-ble 刷新(有 App 兼容反馈才做)、auto_connect 切自动、背压真接线、K6 在线路径换 iot_client_reset。

## 5. 每阶段验证节拍

JL clang `-fsyntax-only` 预检(真实 -I/-D 集)→ CB 全量构建(先清 .bc)→ 板上冒烟(配网 / 对话打断 / DP / OTA 双通道 / K6)→ soak → CHANGES.md 记增量;**上一阶段全绿才开下一阶段**。
