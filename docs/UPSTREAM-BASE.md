# 上游基线对照锚点(UPSTREAM-BASE)

> 用途:记录本仓 vendored agentic-kit 各子模块对齐的上游 SHA、vendor 时的机械适配、本地补丁台账。re-vendor 升级(阶段 1/2)时以本文件为对照锚点,防止"不知道自己基于哪天"再次发生。
> 上游仓库:本地克隆 `D:\code\agentic-kit`。首次建立:2026-09-30(阶段 0 热修落地时)。
> 完整对比分析与语义分歧裁决表:`D:\code\agentic-kit上游对齐报告-2026-09-29.md`。

## 1. 基线与当前对齐状态

| 子模块 / 文件 | vendor 基线 | 当前状态 |
|---|---|---|
| modules/iot-client | `5f4d845`(重构前顶点) | **阶段 1**(2026-09-30 落地,待板测)= 5f4d845 + 本地补丁;新收 iot_atop.c/iot_atop.h/iot_client_internal.h、iot_ota_verify.c 换上游版、protocol11/15 换上游 opt-in 版;mqtt.c == 5f4d845 整文件 |
| common | `5f4d845` | 阶段 1:tls.c = 上游 + 本地 tls_write 诊断;新收 core_http_config.h/core_mqtt_config.h(日志路由,已改 incl tai_log.h) |
| modules/rtc-tcp-client | `5f4d845` | 阶段 1 = 上游(小帧合并等)+ ConnectionRefresh / 长流 v3 本地补丁 |
| modules/tuya-ble(仅配网 3 文件) | `49ab2af` | 基线;**阶段 3 前钉住不刷新** |
| third_party/coreHTTP、coreMQTT | `49ab2af` 同步快照 | 未改动(5f4d845 范围内零差异,已复核) |

> 阶段 1 三路合并法:临时仓(D:\code\align1,用后可删)base=49ab2af 范围 / local=我方树逆向改名 / theirs=5f4d845 范围(LF 归一)→ 解冲突 → 正向改名+CRLF。冲突 10 处(真实 5 文件)全按裁决表落。

与上游 blob 比对命令示例(注意行尾):

```
diff --strip-trailing-cr <(git -C D:/code/agentic-kit show 5f4d845:modules/iot-client/src/mqtt.c) \
     apps/common/LLM/tuya_agentic/agentic-kit/modules/iot-client/src/mqtt.c
```

## 2. vendor 时的机械适配(re-vendor 重放清单)

| 适配 | 说明 |
|---|---|
| `common/log.h` → `tai_log.h` | 避杰理 SDK log.h 符号冲突;仅改文件名与 include;阶段 1 起新含 core_http_config.h/core_mqtt_config.h |
| `iot_ota_*` → `tuya_iot_ota_*` | 同名符号冲突;跨 atop.c / iot_ota.c / iot_client*.c 及头文件;阶段 1 起含上游新 verify/progress 代码 |
| CRLF 行尾 | overlay 与主仓副本均为 CRLF;与上游 blob 比对必须 `--strip-trailing-cr`;上游仓本身混存 CRLF blob,比对/合并前先归一 LF |

阶段 1 实际执行记录(2026-09-30,推翻 09-29 报告的两项预判):

- `%zu` **未转换**:基线 49ab2af 本就带 8 处 %zu 且随整机出厂跑同一 log 门面,无异常证据 → 保留上游原样(树内现 11 处),逐行对齐优先。
- `<time.h>`/`time(NULL)`/`<inttypes.h>`/mbedtls 角括号 **零适配**:树内均有先例(atop.c time(NULL)×4、cipher_wrapper.c `<mbedtls/gcm.h>` 可编)。
- 应用层被迫改:`mqtt_auto_connect`→`mqtt_disable_auto_connect`(上游语义反转,false=自动连接;demo.c 四处改 `= false` 保持原"自动连接"行为不变)。
- 合并去重两处:iot_client.h 我方 12e90d0 移植的 iot_reset_type_t/回调 typedef(换上游完整文档版)、iot_dns.c PROD switch 重复 SG case(我方 85740c6 与上游 ee7fd65 双份)。

## 3. 本地补丁台账(overlay 提交 → 内容)

| overlay 提交 | 日期 | 内容 | 主要落点 |
|---|---|---|---|
| 9992fe3 | 09-10 | ConnectionRefresh(attr23/24/25 + 30min 定时,治 TAI ~1h 回收) | rtc-tcp-client 多文件;上游 master 永无对应,**永久本地维护** |
| 85740c6 | 09-21 | SG 域名支持 | iot_config_defaults.h 等 |
| 7b42827 | 09-21 | 长流模式 v3 | rtc-tcp-client / tai 协议 |
| 12e90d0 | 09-29 | OTA 双 API / protocol15 路由 / 进度上报 / iot_ota_verify.c | atop.c / iot_ota.c / iot_client* |
| `align-phase0` | 09-30 | 阶段 0 热修(NST + EOF/link_dead) | tls.c / mqtt.c |
| (阶段 1) | 09-30 | re-vendor 至 5f4d845;保留全部上述补丁 + 静默分流/进度上报/send_only reset/诊断日志;换上游 protocol11/15 opt-in、iot_ota_verify.c、reset v5.0(与 factory_reset 共存) | 全 kit(除 tuya-ble/third_party)+ demo.c 字段名 + Makefile/.cbp +iot_atop.c |

另:tls.c 内嵌 tls_write 慢链路诊断(海外弱网定位,约 15 行,无上游对应,升级时保留)。

## 4. 分阶段对齐计划(摘要)

- **阶段 0(已完成 2026-09-30)**:热修 tls.c NST(957d1c4)+ mqtt.c EOF/link_dead(整文件=0a81046)。tag `align-phase0`。
- **阶段 1(代码已落地 2026-09-30,clang 预检 35/35 绿,待 CB 构建+板测,绿后 tag)**:re-vendor 至 `5f4d845`(重构前顶点)。收:DNS UE/WE 查询码、ATOP 缓冲可配置 + business error 契约、volatile 密钥清零、激活 v2.0、iot_client_reset v5.0、iot_atop_call、小帧合并、多模态、skip_version_report、session_token_ex、coreHTTP/coreMQTT 错误日志路由;protocol11/15 换上游 opt-in 版、iot_ota_verify.c 换上游版;Makefile/.cbp 增 iot_atop.c。tag `align-phase1-5f4d845`(板测绿后打)。
- **阶段 2**:re-vendor 至 `48e3c0c`(0.5.0)。AGENTIC_KIT_ 旋钮前缀 + 编译期日志(经 `AGENTIC_KIT_LOG` 宏重映射保住运行时调级)、协议 9000 MQTT AI 控制通道、TCP 接收背压(pal_ac791n.c 需补 `sleep_ms`)、服务器时间过滤打断(与 tai_current_event_id 并行双判后收编)。**KWS 联调开始前建议完成**。tag `align-phase2-0.5.0`。
- **阶段 3(可选)**:tuya-ble 刷新(有 App 兼容反馈才做)、auto_connect 切自动、背压真接线、K6 在线路径换 iot_client_reset。

## 5. 每阶段验证节拍

JL clang `-fsyntax-only` 预检(真实 -I/-D 集)→ CB 全量构建(先清 .bc)→ 板上冒烟(配网 / 对话打断 / DP / OTA 双通道 / K6)→ soak → CHANGES.md 记增量;**上一阶段全绿才开下一阶段**。
