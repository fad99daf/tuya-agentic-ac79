# 上游基线对照锚点(UPSTREAM-BASE)

> 用途:记录本仓 vendored agentic-kit 各子模块对齐的上游 SHA、vendor 时的机械适配、本地补丁台账。re-vendor 升级(阶段 1/2)时以本文件为对照锚点,防止"不知道自己基于哪天"再次发生。
> 上游仓库:本地克隆 `D:\code\agentic-kit`。首次建立:2026-09-30(阶段 0 热修落地时)。
> 完整对比分析与语义分歧裁决表:`D:\code\agentic-kit上游对齐报告-2026-09-29.md`。

## 1. 基线与当前对齐状态

| 子模块 / 文件 | vendor 基线 | 当前状态 |
|---|---|---|
| modules/iot-client | `48e3c0c`(0.5.0) | **阶段 2**(2026-09-30 落地,待 CB 构建+板测)= 48e3c0c + 本地补丁;新收 iot_ai_ctrl.c/h(9000 通道)、iot_internal.h、include/iot_client_config_defaults.h、iot_atop.c/h、上游版 iot_ota_verify.c;删旧 src/iot_config_defaults.h、src/iot_client_internal.h(头文件拆分进 include/iot_client_config_defaults.h + src/iot_internal.h);SG 域名表上游已带(85740c6 收编) |
| common | `48e3c0c` | 阶段 2:tai_log.h = 上游编译期门面(AGENTIC_KIT_LOG_LEVEL/AGENTIC_KIT_LOG)+ **本地永久扩展运行时层**(log_set_handler/set_level/get_level/default_handler/log_emit_valist,见 §2);log.c = 运行时状态版;tls.c = 上游 + 本地 tls_write 诊断 |
| modules/rtc-tcp-client | `48e3c0c` | 阶段 2 = 上游(PR#42 背压、tai_config_defaults.h 旋钮、attr111 user_data、timestamp_ms)+ ConnectionRefresh / 长流 v3 本地补丁;on_flow_control 留 NULL(阶段 3 接线) |
| modules/tuya-ble(仅配网 3 文件) | `49ab2af` | 基线;**阶段 3 前钉住不刷新** |
| third_party/coreHTTP、coreMQTT | `49ab2af` 同步快照 | 未改动(48e3c0c 范围内零差异,已复核) |
| pal(仅 pal.h) | `48e3c0c` | 阶段 2:+`sleep_ms` 必填(PR#42 背压暂停让出 CPU);pal_ac791n.c 用 vTaskDelay(pdMS_TO_TICKS) 实现 |

> 阶段 1 三路合并法:临时仓(D:\code\align1,用后可删)base=49ab2af 范围 / local=我方树逆向改名 / theirs=5f4d845 范围(LF 归一)→ 解冲突 → 正向改名+CRLF。冲突 10 处(真实 5 文件)全按裁决表落。
> 阶段 2 同法(D:\code\align2)base=5f4d845 / local=阶段1树逆向适配 / theirs=48e3c0c,7 冲突全解;审计 merged-vs-theirs 556 行逐行对账,本地补丁零丢失。

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

- `%zu` **未转换**:基线 49ab2af 本就带 8 处 %zu 且随整机出厂跑同一 log 门面,无异常证据 → 保留上游原样(树内现 11 处),逐行对齐优先。(**2026-10-06 已推翻:13 处 %zu 当时全在冷路径未执行属侥幸,stage-2 扩进热路径即崩;已全量扫除,见下方"平台适配记录"**)
- `<time.h>`/`time(NULL)`/`<inttypes.h>`/mbedtls 角括号 **零适配**:树内均有先例(atop.c time(NULL)×4、cipher_wrapper.c `<mbedtls/gcm.h>` 可编)。
- 应用层被迫改:`mqtt_auto_connect`→`mqtt_disable_auto_connect`(上游语义反转,false=自动连接;demo.c 四处改 `= false` 保持原"自动连接"行为不变)。
- 合并去重两处:iot_client.h 我方 12e90d0 移植的 iot_reset_type_t/回调 typedef(换上游完整文档版)、iot_dns.c PROD switch 重复 SG case(我方 85740c6 与上游 ee7fd65 双份)。

阶段 2 实际执行记录(2026-09-30):

- **日志双城方案(永久本地扩展)**:上游 ff8e85c 把日志改为编译期门面(AGENTIC_KIT_LOG_LEVEL 天花板 + AGENTIC_KIT_LOG 派发宏,默认派发 log_emit)。本 port 在 tai_log.h 尾部保住运行时 API(log_fn_t/log_set_handler/log_set_level/log_get_level/log_default_handler),log.c 为运行时状态版(handler+level 全局,默认落 log_emit 内运行时过滤)——demo.c 的 tuya_log_redirect/TUYA_SDK_LOG_LEVEL 调级链不折。编译期天花板照常生效(高于天花板的行在宏层就塌缩,零开销不变)。
- **旋钮前缀 + 头文件拆分(8987cf3)**:全部 `IOT_*_SIZE` 等旋钮改 `AGENTIC_KIT_*` 前缀;iot-client 旋钮集中在 include/iot_client_config_defaults.h、rtc 集中在 include/tai_config_defaults.h(本地 agentic_kit_config.h 拾取顺序不变)。旧 src/iot_config_defaults.h、src/iot_client_internal.h 删除,src/iot_internal.h = 旧 defaults 全文(域名表/版本 0.5.0-dev/IOT_LOG*/pal_strdup)。
- **pal_t.sleep_ms 必填**(b616a3e):pal_ac791n.c 以 vTaskDelay(pdMS_TO_TICKS) 实现(不足 1 tick 睡满 1 tick,保证 ≥ms;os_time_dly 按 tick 计不采用)。
- **9000 通道(387957b)**:分发链 reset→ota_confirm→ai_ctrl→DP→raw(iot_client_message.c);demo 四个 init 点注册 on_ai_ctrl(仅日志观察,不动状态)。
- **时间过滤双判对照(65ce503,demo 侧)**:on_audio START 锁存 `g_turn_start_ts = msg->timestamp_ms`(服务器媒体头时间戳);CHAT_BREAK 分支解析 breakAttributes.time 同轴比较,打 `[TUYA-AI] break-time hit/stale/no-start/no-time` 对照日志(只记不改判;hex 半字对打印避 %llu 雷)。板上确认与 event-id/对冲判属一致率后再收编。
- **tai_connect 返回时机核查**:confirmed-connect(同步等 SessionNew ack)在 5f4d845 与 48e3c0c 逐行一致,阶段 1 板测已覆盖此时序,无需 demo 适配(方案风险点销案)。
- **on_flow_control 留 NULL**:阶段 2 不做背压真接线(阶段 3 项);tai_config_t 尾字段 NULL=连续接收,行为与旧版一致。

平台适配记录(2026-10-06,激活 axi_rd_inv 崩机定案修复,**永久保留,re-vendor 重放必做**):

- **格式符扫除**:kit 8 文件(iot-client:atop.c / iot_client.c / iot_dp.c / iot_on_boarding.c;rtc-tcp-client:tai_client.c / tai_pkt_log.c / tai_protocol.c / tai_transport.c)全部 `%zu`→`%u`(35 处)、`%llu`→`(unsigned)` cast + `%u`(5 行 6 处:tai_client liveness/ping、tai_pkt_log case8/timestamp、tai_protocol CONNECTION_REFRESH_RESP)。
- **机制**:杰理 printf 把 'z'/'ll' 当 64 位长度符,va_arg 在 32 位 ABI 上吃双槽 → 参数流错位一格 → 后续 `%.4s` 把栈垃圾当指针解引用 → 非法 AXI 读(axi_rd_inv)。5 次崩机指纹逐值相同;F′(redirect 整体旁路)绿 / F‡(保留 vsnprintf)崩在 Token 行、字节未出;git 取证 stage-1 Token 行是 %u、13 处 %zu 全在冷路径 → "格式符曾跑绿"反例证伪。
- **安全性背书**:size_t 本平台(pi32v2)=32 位,%u 单槽读值不变;阶段 1 Token 行即 %u/%.4s 组合,板上打印正常。
- **重放铁律:凡上游新代码引入 %zu/%llu 一律重扫成 %u / (unsigned)%u**,不留侥幸(libopus fixed_debug.h/MacroDebug.h 的 %llu 是编译门控调试机件,不在扫描范围)。

## 3. 本地补丁台账(overlay 提交 → 内容)

| overlay 提交 | 日期 | 内容 | 主要落点 |
|---|---|---|---|
| 9992fe3 | 09-10 | ConnectionRefresh(attr23/24/25 + 30min 定时,治 TAI ~1h 回收) | rtc-tcp-client 多文件;上游 master 永无对应,**永久本地维护** |
| 85740c6 | 09-21 | SG 域名支持 | iot_config_defaults.h 等 |
| 7b42827 | 09-21 | 长流模式 v3 | rtc-tcp-client / tai 协议 |
| 12e90d0 | 09-29 | OTA 双 API / protocol15 路由 / 进度上报 / iot_ota_verify.c | atop.c / iot_ota.c / iot_client* |
| `align-phase0` | 09-30 | 阶段 0 热修(NST + EOF/link_dead) | tls.c / mqtt.c |
| (阶段 1) | 09-30 | re-vendor 至 5f4d845;保留全部上述补丁 + 静默分流/进度上报/send_only reset/诊断日志;换上游 protocol11/15 opt-in、iot_ota_verify.c、reset v5.0(与 factory_reset 共存) | 全 kit(除 tuya-ble/third_party)+ demo.c 字段名 + Makefile/.cbp +iot_atop.c |
| (阶段 2) | 09-30 | re-vendor 至 48e3c0c(0.5.0);AGENTIC_KIT_ 旋钮前缀+头拆分、编译期日志门面+本地运行时层、9000 通道、pal sleep_ms、时间过滤双判对照;全部本地补丁(ConnectionRefresh/长流v3/OTA 双 API/send_only/schema 自愈/[MQTT-RX]/tls_write 诊断/tai_current_event_id)对账存活 | 全 kit(除 tuya-ble/third_party)+ pal_ac791n.c + demo.c 接线 + Makefile/.cbp +iot_ai_ctrl.c |
| (格式符扫除) | 10-06 | 杰理 printf 64 位长度符崩机修复:%zu→%u 35 处 + %llu→(unsigned)%u 6 处,共 8 文件;详见 §2 平台适配记录 | iot-client 4 文件 + rtc-tcp-client 4 文件 |

另:tls.c 内嵌 tls_write 慢链路诊断(海外弱网定位,约 15 行,无上游对应,升级时保留)。

## 4. 分阶段对齐计划(摘要)

- **阶段 0(已完成 2026-09-30)**:热修 tls.c NST(957d1c4)+ mqtt.c EOF/link_dead(整文件=0a81046)。tag `align-phase0`。
- **阶段 1(代码已落地 2026-09-30,clang 预检 35/35 绿,板测 4/6 绿:配网/对话打断/DP/K6 通过,OTA+soak 并入阶段 2 一并测;tag 待打在 647ec00)**:re-vendor 至 `5f4d845`(重构前顶点)。收:DNS UE/WE 查询码、ATOP 缓冲可配置 + business error 契约、volatile 密钥清零、激活 v2.0、iot_client_reset v5.0、iot_atop_call、小帧合并、多模态、skip_version_report、session_token_ex、coreHTTP/coreMQTT 错误日志路由;protocol11/15 换上游 opt-in 版、iot_ota_verify.c 换上游版;Makefile/.cbp 增 iot_atop.c。tag `align-phase1-5f4d845`(板测绿后打)。
- **阶段 2(代码已落地 2026-09-30,clang 预检 26/26 零真错,待 CB 构建+板测;用户拍板与声学测试合并 soak,一并覆盖 ≥1 个 30min ConnectionRefresh 周期)**:re-vendor 至 `48e3c0c`(0.5.0)。AGENTIC_KIT_ 旋钮前缀 + 编译期日志(本地运行时层保住调级)、协议 9000 MQTT AI 控制通道(demo on_ai_ctrl 对照观察)、TCP 接收背压(pal sleep_ms 已补,on_flow_control 留 NULL 阶段 3 接线)、服务器时间过滤打断(demo 双判对照日志,确认一致率后收编)。tag `align-phase2-0.5.0`(板测绿后打)。
- **阶段 3(可选)**:tuya-ble 刷新(有 App 兼容反馈才做)、auto_connect 切自动、背压真接线、K6 在线路径换 iot_client_reset。

## 5. 每阶段验证节拍

JL clang `-fsyntax-only` 预检(真实 -I/-D 集)→ CB 全量构建(先清 .bc)→ 板上冒烟(配网 / 对话打断 / DP / OTA 双通道 / K6)→ soak → CHANGES.md 记增量;**上一阶段全绿才开下一阶段**。
