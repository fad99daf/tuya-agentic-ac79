# 涂鸦 Agentic 集成 — 改动清单

> 工程:`fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0`(杰理 AC791N / wl82 AIoT SDK)
> 说明:本工程**非 git 仓库**。区分"原版/改动"的依据是文件时间戳——SDK 原版统一为 `2026-03-03 10:09`,凡 `2026-07` 月修改/新增的即为本次涂鸦集成改动。
> 配套方案文档:`agentic-kit-integration.md`(7-17,早期方案,与最终落地有出入,见文末"与方案文档的差异")。

---

## 0. 总览

改动分三类:

| 类别 | 说明 |
|---|---|
| **A. 新建集成模块** | `apps/common/LLM/tuya_agentic/` 下手写的胶水代码(PAL / 配网 / 对话主流程 / bool 补丁 / **OTA 编排** / **音乐技能**) |
| **B. 改动的 SDK 原有文件** | 10 个:Makefile、user_cfg.c、app_music.c、app_main.c、app_config.h、wifi_app_task.c、audio_input.c/.h、board_7916A.c、AC791N_...cbp |
| **C. 引入的依赖(原样,未改)** | 涂鸦开源 `rtc-tcp-client`/`iot-client`/`tuya-ble`/`common` + AWS `coreHTTP`/`coreMQTT` |

---

## A. 新建集成模块 `apps/common/LLM/tuya_agentic/`

> 整个目录是本次新增。其中 `agentic-kit/` 子目录是从涂鸦 agentic-kit 复制引入的依赖(C 类,原样未改);其余是手写文件。

### A1. `tuya_agentic_demo.c`(48 KB)— 对话主流程 ⭐改动最重

入口由 `late_initcall(tuya_agentic_main_init)` **开机自启动**(fork 16KB 栈线程),不挂 DHCP 钩子(见 B5)。核心内容:

**① 修复"无法打断"根因**
- 文件顶 `#include "app_config.h"`(L12-14,带长注释)。原 demo.c 漏 include → `TUYA_BARGE_IN_ENABLE` 不可见 → barge-in 三处 `#ifdef` 全被编译掉,功能从未进固件。

**② 设备三元组 / WiFi 凭据持久化(VM 176~183,裸 index,未进 syscfg_id.h)**
- `176=devid` / `177=secret_key` / `178=local_key`(各 32B)
- `179=ssid` / `180=pwd`(各 65B,直连路径开机重连用)
- `181=schema_id` / `182=schema JSON`(DP 用,见 A1⑥/iot_dp)
- `183=region`(1B,配网激活时云端下发;直连重启据此选 ATOP/MQTT 域名,**不硬编码**——设备不预知部署区,海外区设备重启后才不会打到中国区域名)

**③ `tuya_agentic_main()` 启动流程**
- **直连路径**(VM176 有 devid):读三元组 + ssid/pwd + region(VM183,无效兜底 AY)→ `wifi_enter_sta_mode` → 等 DHCP → `iot_client_init` → `tuya_ai_run`
- **首次配网路径**(无 devid):播"请配置网络"语音 → `tuya_ble_netcfg_start`(BLE 阻塞等 App 下发 ssid/pwd/token)→ 停 BLE 释放 RAM → 连 WiFi → `iot_client_init_on_boarding_with_token` 激活(region 由 token 前 2 字符自动解析,激活响应再回 `region` 字段确认)→ **写 176~183**(三元组+WiFi 凭据+DP schema_id+region)→ hold MQTT ~3s 让 App 确认配网成功 → 注册 DP 下行回调 → `tuya_ai_run`(**MQTT 常驻**:与 AI 的 TLS 是各自独立 TCP 连接可并存;`tuya_mqtt_ka` 线程每 5s `iot_client_process` 维持心跳并收 DP 下行,App 里设备保持在线可控制)
- **历史 bug 修复**:`syscfg_read` 返回值判断从 `==0`(误判每次重配)改为 `>0`

**④ `tuya_ai_run()` 对话循环**
- **上行 ASR = 固定 PCM**:`tai_send_audio_start(ctx, TAI_AUDIO_PCM, 1, 16, 16000)`,帧长 `TUYA_OPUS_FRAME_LEN=1280`(16k/16bit/mono 40ms)。注释说明:上行**不用** opus——杰理 opus `format_mode=0` 是"百度无头"非标准格式,涂鸦 ASR 解不了会秒回空。
- **下行 TTS = opus(可选)**:`#ifdef TUYA_DOWNLINK_OPUS_ENABLE` 分支把 `session_attrs` 改成请求 `opus/16k/1`;`on_audio` 前 5 帧核对 `opus_cbr_pktlen`(全一致=CBR)。真正的 opus 解码在 `audio_input.c`(见 B6)。
- **停说判定(云端 VAD,TUYA_SERVER_VAD_ENABLE)**:开口永远由本地 VAD(`get_recoder_state`)负责,这里切换的是"停说"——开云端 VAD 后,上行循环等云端 `TAI_EVT_SERVER_VAD` 事件收尾(云端模型更强,更准);带本地超时兜底(本地 VAD 持续判静音 >2s 强制收尾,防云端事件丢失导致一直上行)。`asr.enableVad:true` 放 `chatAttributes`。

**⑤ Barge-in(打断,TUYA_BARGE_IN_ENABLE)** — 多层防护:
- **1000ms 冷却**(`TUYA_BARGE_COOLDOWN_MS`/`g_barge_cooldown_until`/`barge_cooldown_expired()`):`chat_break` 后置位冷却,窗口内忽略老轮在途 TTS 残响二次触发误判;差值用 `(int)` 比较抗回绕。
- **stale mic 排空**:barge-in 新上行前排掉 TTS 期间积压的旧 mic 数据(保留最近 3 帧 = 1280×3,封顶 6 帧 ≤240ms 防 session 卡死),否则 cbuf 压着旧残音,新上行先发静音/回声 → ASR 收垃圾。
- **能量统计 `opus_frame_stat`**(已修):从旧的按字节 `s += p[i]` 累加(负样本高字节恒 0xFF,导致静音/说话 sum 全卡 ~15万、act 全卡 60-90% 区分不出)改为小端拼 int16 后 `Σ|v|`,`act=avg|sample|/100` 封顶 100。被 5 处复用(idle 底噪基线/turn-start 门/barge 确认/逐帧统计/AEC 诊断)。
- **3 帧能量确认 `barge_in_energy_confirmed`**:VAD 触发后连读 3 帧,3-of-3 的 sum 均 ≥ `BARGE_MIN_ENERGY(100000)` 才算真话音,滤 ≤2 帧的 AEC 残留/噪音 spike。
- **onset 帧补发 `g_barge_prebuf[1280*3]`**:barge-in 新上行时补发触发前最响那段(~120ms),否则只剩尾音/静音 → ASR 空。
- **play-drain-wait**:`TAI_EVT_END` 后不立刻听,等 `_device_get_play_level()>640`(≈DAC 真播完,35s 兜底)再进入监听,治"云端发完 ≠ 喇叭播完"导致的自说自话。
- **turn-start 能量门**(非 barge 轮):无唤醒词+单麦开麦易自言自语,VAD 触发后再核 1 帧能量才起轮。
- **idle 持续排空**:空闲不断丢 mic,保证 VAD 触发时无积压;**绝不**在 VAD 触发时 clear(会吞刚触发的话音)。

**⑥ `tuya_clear_provision_and_reset()`(L944)** — K6 长按入口:把 VM 176~180 全写 0 → **`tuya_ble_netcfg_stop()` 先停 BT 广播** → `os_time_dly(300)`(BT 控制器 idle + VM 落盘)→ `cpu_reset()` → 重启后读不到 devid 自动重新配网。

   > **修复(软复位后 BT 脏状态 → 配网失败)**:`cpu_reset()` 内部走 `P33_SYSTEM_RESET`,虽是整机软复位,但**不像掉电 / reset 键那样彻底重置 BT 控制器**。若复位前 BT 处于广播/连接活跃态,带脏射频状态复位会导致重启后 BLE 链路异常(`conn nack` 雪崩 → 5s supervision timeout 断开,reason 0x08),配网必失败。**实测复现**:长按 K6(走软复位)后配网失败,紧接着按 reset 键(冷启动)则配网成功。**解法**:复位前先 `tuya_ble_netcfg_stop()` 停广播 + 延迟 3s 让 BT 控制器进 idle,再软复位。已验证修复。

**⑦ 产品三件套**(L46-48,构建期硬编码):`TUYA_PRODUCT_KEY` / `TUYA_UUID` / `TUYA_AUTH_KEY`。

### A2. `tuya_agentic.h`(695 B)— 对外头
声明 3 个入口:`tai_pal_ac791n()`、`tuya_agentic_demo()`(文本对话)、`tuya_agentic_main()`(完整 boot)。无改动点。

### A3. `pal_ac791n.c`(9 KB)— PAL 平台适配层
把 agentic-kit 的 14 个 PAL 回调桥接到 AC79 宿主(FreeRTOS + lwIP + mbedTLS):
- **TCP**:照搬 `pal_freertos.c`。`getaddrinfo` → 非阻塞 `connect`+`select` 限时 → 查 `SO_ERROR` → 回阻塞模式 + `TCP_NODELAY`;`EAGAIN/EWOULDBLOCK` 返 `PAL_ERR_AGAIN`。`MSG_NOSIGNAL/MSG_DONTWAIT` 自定义(AC79 无)。
- **mutex — 必须递归锁**:`xSemaphoreCreateRecursiveMutex`(需 `configUSE_RECURSIVE_MUTEXES=1`)。iot-client 的 DP schema 更新会重入锁,普通锁死锁。
- **thread**:`thread_fork`+trampoline 桥接 `void* fn(void*)` ↔ `void fn(void*)`,`thread_kill(KILL_WAIT)` 回收,栈 6KB(跑 mbedTLS 握手)。
- **time**:`sys_timer_get_ms`;**malloc/free**:libc。
- **配套兼容**:`flockfile/funlockfile` 空实现(AC79 newlib 缺,agentic-kit log 用到);配合 demo.c 的 `log_set_level(0)` 关日志绕过 `fprintf(stderr)` 崩溃(AC79 无 stderr)。

### A4. `le_net_cfg_tuya.c`(10 KB)+ `.h` — 涂鸦 BLE 配网传输层(全新)
AC79 上手写的涂鸦 BLE GATT 配网传输层(SDK 原版无):
- `.h`:涂鸦 3 个 128-bit 特征 UUID(write/notify/read)、手写 `profile_data[]` GATT 服务表、handle 宏(`WRITE_VAL=0x0006`/`NOTIFY_VAL=0x0008`/`NOTIFY_CCC=0x0009`/`READ_VAL=0x000b`)、对外 API `tuya_ble_netcfg_start/stop`。
- `.c`:`tuya_ble_hal_random`(协议随机数 HAL,简易 LCG,注释提正式应用 AC79 TRNG)、`tuya_hal_send`(`att_server_notify` 回传)、`prov_complete_cb`(解出凭据→post 信号量)、ATT read/write 回调(write 数据转发到大栈 worker 任务,**不在 btstack 4KB 栈里跑 mbedTLS**)、`tuya_pkt_handler`(HCI 连接/断开/MTU,断开重启广播)、`tuya_ble_netcfg_start`(init→换 profile→开广播→阻塞等配网)、worker 任务 `tuya_prov_w`(2KB 栈,`early_initcall`)。

### A5. `tuya_bool_compat.h` + `tuya_inc/stdbool.h` — bool 冲突兼容补丁
解决 **JL clang 4.0.1 的 `stdbool.h` `#define bool _Bool`** 与 **AC79 `cpu.h` `typedef unsigned char bool`** 在同一翻译单元冲突("cannot combine with previous 'char'")。两套机制:
- `tuya_inc/stdbool.h`:放在**最高优先级 `-I` 目录**(Makefile L155),覆盖系统 stdbool.h,改成 `typedef unsigned char bool`(与 cpu.h 完全一致),从根本上消除 `_Bool` 宏冲突。
- `tuya_bool_compat.h`:作为 **`-include` 强制头**,先 `#define __STDBOOL_H` 让后续任何 `<stdbool.h>` 整体跳过,再提供同款 typedef。
- 同类型 typedef 重复,clang 作为扩展允许 + Makefile 已 `-w`。C++ 下整段跳过。

### A6. `tuya_ota.c`(11 KB)+ `tuya_ota.h`(2.9 KB)— 涂鸦云 OTA 编排层 ⭐新增
把涂鸦的云协议(ATOP over HTTPS)和杰理的下载烧写链路(`get_update_data`)串起来。`TUYA_OTA_ENABLE` 门控。
- **`tuya_ota_check_and_upgrade(client)`**:开机连 AI 之前调(此时 iot_client 活着,无并发冲突)。独立线程 `tuya_ota_chk` 跑 `tuya_iot_ota_check_upgrade`,主线程信号量限时等待 `TUYA_OTA_CHECK_TIMEOUT_MS=7000`——防 ATOP HTTPS 握手失败时卡满 SDK 内部 5s 超时拖慢开机。
- **流程**:无升级返回 0;有升级→`report_status(UPGRADING)`→播"正在升级"→`get_update_data(url)` 下载烧写(成功则内部 `net_fclose` 自动 `system_reset`)。成功抢在 2s reset 窗口内上报 FINI。
- **版本号管理(手动方案)**:`tuya_get_effective_sw_ver()` 直接返回 `TUYA_FIRMWARE_VERSION`。跨 OTA 自动持久化版本号(VM/USER/BTIF/RTC)经实测**全部不可靠**(均会被擦除/覆盖),故采用手动方案——每次发版前在 `app_config.h` 改 `TUYA_FIRMWARE_VERSION` 与涂鸦平台一致。`tuya_save_upgraded_sw_ver`/`tuya_clear_upgraded_sw_ver` 保留为空实现(调用方仍调用,不报错)。
- **`TUYA_OTA_ENABLE=0` 时**提供空实现,调用方无需 `#ifdef` 包裹。
- **双备份**:配合 `CONFIG_DOUBLE_BANK_ENABLE=1`(两份固件并存 ~4.6MB),`CONFIG_AUDIO_PACKRES_LEN` 缩到 512KB 腾空间。OTA 提示音(OtaInUpdate/OtaSuccess/OtaFailed.mp3)是杰理官方 SDK 自带的,overlay 不含。

### A7. `tuya_music.c` + `.h` — 涂鸦音乐技能(解析)⭐新增
"播放XXX的歌"时云端回音乐 SKILL JSON(带试听 mp3 URL)。本文件并行重组文本流并解析出 URL/歌名/歌手,播放交给 `app_music` 网络解码链(demo.c ⑤ 交接块编排):
- **并行重组**:on_text 每片 `tuya_music_text_accum`(START 开缓冲 / MIDDLE 追加 / END 交付解析),on_event `TAI_EVT_END` 兜底 flush(SDK 会丢空文本帧);4KB 会话级缓冲,新会话 `tuya_music_reset` 清残留。
- **判音乐(两级,照搬 agentic-kit music_play_demo)**:① 快速通道——带引号字面量 `"code":"music"` 在全流任意位置命中即算;② 变体兜底——取 data 一刀截断验 code,验完还原。**关键坑**:一轮文本流是 ASR/SKILL/NLG 多个完整 JSON 信封首尾拼接,全文档第一个 `"data"` 属 ASR 信封——按 `"data"` 钻取永远轮不到音乐信封(实测踩坑,歌播不出来即此);钻取必须从只在音乐信封出现的 `"general"` 键进(general→data→audios→[0])。
- **demo.c ⑤ DAC 交接**:TTS 排空(播完"正在为您播放…")→ `_device_net_audio_play(0)` 停 TTS 解码器让出 DAC(前后各 300ms,防 subdevice_dac 格式重配断言)→ `app_music_tuya_play_url` → 等播完(dec_end 回调/busy=0/600s 兜底)→ 恢复 TTS 播放器回听音。barge-in 打断 TTS 报幕时丢弃 pending 音乐(请求已过时)。
- **音乐 barge-in(说话可停乐)**:等待循环逐帧 `opus_frame_stat`,VAD 在线 + **3 帧连续**(120ms)sum≥`BARGE_MIN_ENERGY` 才停乐(判据同 `barge_in_energy_confirmed`,断一帧重数,滤音乐瞬态拍子);开头 1s 不设防(DAC 交接瞬态/曲首重拍);每秒打 `[MUSIC-DBG]` 能量基线供调门槛(AEC 对连续音乐的效果未验证,若"音乐自己把自己打断"就调高门槛);停乐后排 ~300ms 残响再回听音。**打断词本身与音乐混叠不作数**——语义是"停乐后听",下一句等音乐停了再说。
- **⚠️ 试听版仅 ~30s 片段**;完整歌曲需在涂鸦平台购买音乐高级能力授权。

### A8. `tuya_opus_enc.c` + `.h` — 上行 opus 编码封装 ⭐新增
本地 libopus 定点编码器的薄封装(init/frame/deinit/ok 共 4 个 API),供 demo.c 在**上行发送处**逐帧编码(1280B PCM → ~80B opus 包)。关键实现:
- 5 个符号重定向 `#define`(opus_encoder_create→topus_* 等)必须放在 `#include "opus.h"` **之前**——否则头文件原型不跟着改名,产生隐式声明告警 + 链接错。
- 文件整体 `#ifdef TUYA_UPLINK_OPUS_ENABLE` 包裹,开关关闭时为空翻译单元(不占体积)。
- init 失败 → demo.c 打印告警并自动回退 PCM 上行(对话仍可用,只是带宽大),不废会话。

### A9. `libopus/` — libopus 1.4 定点编码器子集 + 符号隔离预编译 ⭐新增
上行 opus 的编码器本体(~190 文件/3MB,含预编译 `libopus_tuya.a`)。**为什么自带**:杰理闭源 `lib_opus_enc.a / lib_opus_stenc.a / lib_opus_dec.a` 也是 libopus 改的,与源码直编有 141/74/137 个同名全局符号,混链必报 multiple definition;且闭源库不暴露编码参数接口,无法对齐涂鸦云端要求的 16k/mono/VOIP/CBR16k/40ms。
- **裁剪**:只留定点编码路径——删 decoder、x86/arm SIMD、浮点、tests/dump_modes;恢复链接必需的 PLC.h、NLSF_decode.c、repacketizer.c;`src/packet_shim.c` 补 `opus_packet_get_nb_frames`;自带 `config.h`(OPUS_BUILD/FIXED_POINT/VAR_ARRAYS);`silk/fixed/` 下补 typedef.h、debug.h 转发 shim(quoted include 先搜本目录,父目录同名头与杰理 generic/lwip 头撞名)。
- **`build_tuya_libopus.sh`(两遍编译符号隔离)**:第一遍无重命名编译 + llvm-nm 收集全部全局符号,生成 rename.h(`#define sym topus_sym`);第二遍 `-include rename.h` 重编(定义与引用同步改名,libc 名不动),llvm-ar 打包成 `libopus_tuya.a`;末尾自检(归档内全部 topus_ 前缀 + 与 3 个杰理 opus 库零交集,失败即退出)。**产物是 LTO bitcode 归档**——必须,普通 ELF 对象在工程 LTO 链接里会被 R_PI32V2_LONG_JUMP_23M2 长跳转搬迁卡死(2026-08-28 实测)。⚠️ 脚本对 SDK 根/工作目录全部相对定位(路径含中文时杰理 LLVM 工具会失败),工具链取 `/c/JL/pi32/bin`(与 Makefile 约定一致)。
- **仓库已带预编译好的 `libopus_tuya.a`**:不改 libopus 源码**无需**跑此脚本,直接编工程即可;只有改了 `libopus/` 下源码才需重跑再编。
- 许可 BSD-3-Clause(`COPYING` 随附,二进制分发需保留声明)。

---

## B. 改动的 SDK 原有文件

### B1. `apps/wifi_story_machine/board/wl82/Makefile`
- **DEFINES**(L151):追加 `-DHTTP_DO_NOT_USE_CUSTOM_CONFIG -DMQTT_DO_NOT_USE_CUSTOM_CONFIG -DIOT_DO_NOT_USE_CUSTOM_CONFIG`(让 coreHTTP/coreMQTT/iot-client 用默认配置,不引各自 *_config.h)
- **INCLUDES**(L155, L244-255):追加 12 条 `-I` —— `tuya_inc`(最高优先级覆盖 stdbool.h)、`tuya_agentic`、`pal`、`common`、`rtc-tcp-client/include`、`iot-client/include`+`src`、`coreHTTP` include+interface+llhttp、`coreMQTT` include+interface、`tuya-ble/include`
- **c_SRC_FILES**(L635-666):追加 35 个涂鸦源文件 —— rtc-tcp-client(7)+iot-client(11)+common(3)+coreHTTP(4)+coreMQTT(3)+`pal_ac791n.c`+`tuya_agentic_demo.c`+`tuya_ble_prov.c`+`le_net_cfg_tuya.c`+`tuya_music.c`(音乐SKILL解析);另**删掉**未用到的第三方 LLM 源文件/头路径(volc/onesdk/duer/QYAI/tc_iot 等,见与 SDK 原版 Makefile 的 diff)
- **LFLAGS**:不动(复用宿主 `libmbedtls_3_4_0.a` / `cJSON.a` / `lwip_2_2_0.a` / `lib_mqtt.a`);**追加 `tuya_agentic/libopus/libopus_tuya.a`**(上行 opus 编码库,排在 lib_opus_dec.a 之后)
- **上行 opus 构建接入**(Makefile 与 `.cbp` 同步改):INCLUDES 追加 5 条 —— `libopus` 根(在 tuya_inc 之后靠前)与 `include`/`celt`/`silk`/`silk/fixed` 4 子目录(**列表末尾**)。⚠️ 顺序敏感:silk 子目录靠前会让其 `typedef.h` 劫持 SDK 同名头(→btstack u8/u16 编译错),libopus 根靠后又会被 volc 的 `config.h` 遮蔽(→OPUS_BUILD 未定义);`c_SRC_FILES` 追加 `tuya_opus_enc.c`。CodeBlocks 工程文件 `AC791N_WIFI_STORY_MACHINE.cbp` 做同套改动(用户走 CB 编译时一致)。

### B2. `apps/common/config/user_cfg.c` — AEC/DNS 参数覆盖(barge-in + 嘈杂环境 ASR)
`get_cfg_file_aec_config()` 末尾(L283-309)硬覆盖 6 项 AEC/DNS 参数 + 中文注释,让连续 TTS 期间的回声残留被压住、用户话音能穿透触发 VAD(barge-in 前提),并强化嘈杂环境降噪(ASR):

| 参数 | 原值 → 新值 | 作用 |
|---|---|---|
| `AEC_DT_AggressiveFactor` | 1.0 → **2.0** | 回声消除更激进 |
| `ES_AggressFactor` | -3.0 → **-6.0** | 非线性残留抑制更深 |
| `ES_MinSuppress` | 4.0 → **2.0** | 允许更深抑制 |
| `EnableBit` | flash 值 → **\|= BIT(5)** | 强制开 DNS 位:实际 aec_mode 读自 flash syscfg,旧值可能没存 BIT(5)(mode=7 而非 39),`CONFIG_DNS_ENC_ENABLE` 定义了也等于没开 |
| `DNS_over_drive` | 1.0 → **3.0**(2026-08-28,经 2.0) | DNS 降噪强度(0~6):2 压 TTS 残留回声;3 为嘈杂环境 ASR(实测底噪基线 9k~64k → 3k~6k,SNR 不足时云端捞字难)。小声说话被吃/识别变差回调 2.0~2.5 |
| `DNS_gain_floor` | → **0.1**(显式钉住) | 最大降噪深度下限(默认值),防 flash 值漂移;再小易压语音 |

> 注意:VM 176~180 的三元组存取**不在本文件**,而在 `tuya_agentic_demo.c`。本文件只有这一处 AEC 覆盖改动。

### B3. `apps/wifi_story_machine/app_music.c` — 四处改动(无 TTS 解码)
- **音乐技能播放导出(末尾新增块)**:`app_music_tuya_play_url(url, on_dec_end)`(试听 mp3 URL → `net_music_dec_file`,https 自动 TLS → mp3 解码 → DAC;提示音在播则链式接续)、`app_music_tuya_music_stop/busy`(demo.c ⑤ 交接块用:busy=0 感知下载/解码失败,不必傻等 600s)。
- `app_music_play_netcfg_prompt()`(L3572):播 `NetCfgEnter.mp3`,供 demo.c 配网提示任务周期播报。
- `app_music_play_ota_prompt(int type)`(L3580):**OTA 提示音播报**,供 `tuya_ota.c` 跨文件调用(0=正在升级 / 1=升级成功 / 2=升级失败)。`app_music_play_voice_prompt` 是 static,需在 app_music.c 内包一层导出。
- K6(`KEY_PHOTO`)长按(L4447-4457,`#if CONFIG_TUYA_AGENTIC_ENABLE`):调 `tuya_clear_provision_and_reset()`。K6 短按仍是绘本识别(不动)。
- KEY_POWER 长按注释(L4396-4400):说明电源键是自锁拨动开关按不出长按,原绑在此的"清配网"已移到 K6。
- **下行 opus 解码不在本文件**,在 `audio_input.c`(见 B6)。

### B4. `apps/wifi_story_machine/include/app_config.h` — 新增宏 + 2 处 flash 布局
```c
#define CONFIG_TUYA_AGENTIC_ENABLE     // 涂鸦 AgenticKit 总开关(控制 Makefile 编入 + K6 重置分支 + DHCP 钩子行为)
#define TUYA_DOWNLINK_OPUS_ENABLE      // 下行 TTS opus(已开启;已调通,~2KB/s 治拥挤网卡顿。注释掉切回 PCM)
#define TUYA_UPLINK_OPUS_ENABLE        // 上行 ASR opus(已开启;本地 libopus 1.4 定点软编码,~2KB/s 为 PCM 的 1/16。编码器 init 失败自动回退 PCM。注释掉切回 PCM 32KB/s)
#define TUYA_BARGE_IN_ENABLE           // 用户打断 TTS 开关(已开启;强依赖 AEC、有竞态、实验性)
#define TUYA_SERVER_VAD_ENABLE         // 云端 VAD 停说判定(已开启;开口仍本地VAD,停说由云端TAI_EVT_SERVER_VAD决定,带本地2s超时兜底)
#define TUYA_MUSIC_ENABLE               // 涂鸦音乐技能(已开启;SKILL解析→app_music网络解码播放,支持barge-in停乐)
#define TUYA_OTA_ENABLE         1      // 涂鸦云 OTA(开机连 AI 前检查一次,有升级则下载烧写重启)
#define TUYA_FIRMWARE_VERSION   "1.0.11"// 出厂基线版本号;发版前改成与涂鸦平台填的一致
```
flash 布局改动(为双备份 OTA 腾空间):
- `CONFIG_AUDIO_PACKRES_LEN` 0x180000(1.5MB)→ **0x80000(512KB)**:提示音实际只占 222KB,512KB 够装;省下的 1MB 给双备份固件区。
- `CONFIG_DOUBLE_BANK_ENABLE` 0→**1**:开启双备份升级(OTA 需要,两份固件 ~4.6MB 并存)。

另有 AEC 段一行注释(L358):`CONFIG_AEC_LINEIN_CHANNEL_ENABLE` 硬件回采实测未生效、已回退软件参考(探索项,注释掉)。

### B5. `apps/wifi_story_machine/wifi_app_task.c` — 仅注释
`WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC` 分支(L661-662)留注释:涂鸦流程改为 `tuya_agentic_main` boot 时 `late_initcall` 自启动(它要在 DHCP 前先跑 BLE 配网),**不再挂 DHCP 钩子**——这是与 SDK 原版"DHCP 成功后 thread_fork 启动第三方 LLM demo"(对比 `CONFIG_VOLC_LLM_ENABLE`/`CONFIG_ONESDK_LLM_ENABLE` 写法)最大的架构差异。

### B6. `apps/common/LLM/audio/audio_input.c` + `.h` — 上行采集 + 下行播放桥(承载 opus 解码)
用 SDK `audio_server` 起 enc(mic→`pcm_cbuff_w`)和 dec(`pcm_cbuff_r`→DAC),双 cbuf 环形缓冲,给 demo.c 提供全部音频接口。TUYA 分支关键点:
- `AUDIO_RECORD_VOICE_UPLORD_LEN` = **1280**(PCM 16k/16bit/mono 40ms);`_device_get_voice_data` `mdelay(40)` 按帧节拍取数。
- `audio_recoder_init` 的 `CONFIG_TUYA` 块:`format="pcm"`、bitrate 24000、frame_ms 60;**VAD 阈值调低** `vad_start_threshold=100`(原 150)、`vad_stop_threshold=600`(原 800,省 ~200ms);**AEC** `wideband=1`、`hw_delay_offset=50`、`output_way=1` 硬件回采。
- **下行 opus 解码**(`audio_player_net_init`,L496-512,`TUYA_DOWNLINK_OPUS_ENABLE` 开):`dec_type="opus"`、**`channel=0`**(让解码器自动)、**`sample_rate=0`**(让解码器自动输出 48k,audio_server 自动重采样到 DAC 16k)、`AUDIO_ATTR_OPUS_CBR_PKTLEN_TYPE` + `opus_cbr_pktlen=80`(CBR 模式必须保留,解码器靠它确定帧边界,去掉会卡死)。默认关→`dec_type="pcm"`(稳但拥挤网 32KB/s 易卡)。
  > **调通历程**:① `sample_rate=16000` + 80B帧重组→啸叫/低沉慢速(48k PCM 按 16k 播);② 去掉 CBR→解码器初始化卡死(audio_server timeout);③ 最终 `sample_rate=0` 自动重采样 + CBR pktlen=80 + 整包直写→**调通,实测云端 codec=111 帧长 400/640B 变长,解码器内部按标准 Opus 帧边界切分**。
- **下行 opus 整包直写**(`_device_write_voice_data`,L213):云端帧长可变(400B/640B),整包直写 cbuf,**不做帧重组**(早期 80B 重组方案已废弃——解码器内部按标准 Opus 帧边界自行切分)。
- PCM 下行缓冲满时**不再 `cbuf_clear`**(会瞬间丢整缓冲→截断),改丢本次新数据 + 计数 `dl_full_cnt` 诊断。
- 下行播放缓冲 TUYA 分支开到 **64×=1MB(≈32s)**,吸收长答案突发下发,配合 demo.c play-drain-wait(35s > 32s)。
- `AUDIO_PLAY_VOICE_VOLUME=50`(原 80 太大)。

---

## C. 引入的依赖(原样,未改本地源码)

`apps/common/LLM/tuya_agentic/agentic-kit/` 下,从涂鸦 agentic-kit 复制引入,经甄别(`modules`/`common`/`pal` 下无中文注释、无宿主头 include、无 hack/适配标记)确认**未做本地源码改动**,所有适配都靠 PAL 层 + bool 补丁 + Makefile 完成:

- `modules/rtc-tcp-client/` — 涂鸦开源 RTC TCP 客户端(`tai_*`)
- `modules/iot-client/` — 涂鸦开源 IoT 客户端(激活/DP/OTA/MQTT)
- `modules/tuya-ble/` — 涂鸦 BLE 配网协议(仅 `tuya_ble_prov.c` 被编入)
- `common/` — `tls.c`(基于宿主 mbedTLS)、`rng.c`、`log.c`
- `third_party/coreHTTP/`、`third_party/coreMQTT/` — AWS 第三方,AWS 原版(跳过甄别)

---

## 排除项(时间戳变化但与涂鸦无关 / 未改)

| 文件 | 时间戳 | 实际情况 |
|---|---|---|
| `apps/demo/demo_DevKitBoard/include/app_config.h` | 07-24 | 无任何涂鸦/agentic/中文痕迹,与 wifi_story 版不同源。**非本次改动**(早期实验或 touch),排除 |
| `apps/wifi_story_machine/app_main.c` | 07-27 | 涂鸦流程绕开 app_main,无 tuya 相关代码。**未实质改动** |
| `apps/wifi_story_machine/board/wl82/board_7916A.c` | 07-27 | K6→`KEY_PHOTO` 的 AD 阶梯映射是 **SDK 原版**;涂鸦只借用该键,未改本文件 |

---

## 与方案文档(`agentic-kit-integration.md`)的差异

文档是 7-17 早期方案,最终落地(7-26~29)有以下演进:

| 方案文档说法 | 实际落地 |
|---|---|
| 不用 BLE 配网,不编 `tuya_ble_prov.c` | **用了 BLE 配网**:新增 `le_net_cfg_tuya.c/.h` + 编入 `tuya_ble_prov.c` |
| DHCP 成功后 `thread_fork("tuya_agentic")` 启动 | 改为 `late_initcall` **开机自启动**(要在 DHCP 前先跑 BLE 配网) |
| PAL 骨架 + 文本 demo | demo.c 实际 48KB,远超骨架:含完整配网/激活/barge-in/opus/AEC/能量统计 |
| — | 新增 barge-in、下行 opus、AEC 调优、VM 三元组持久化(均为文档之后的工作) |

---

## 数据流一句话

`mic → audio_input.c(enc+VAD+AEC)→ pcm_cbuff_w → demo.c _device_get_voice_data → 上行发送(TUYA_UPLINK_OPUS_ENABLE 开:tuya_uplink_send_frame 逐帧 libopus 定点编码 ~80B/40ms;关:PCM 1280B/40ms 直发)→ tai_send_audio_chunk`;`云端 opus → demo.c on_audio → _device_write_voice_data(整包直写)→ pcm_cbuff_r → audio_input.c dec(CBR opus_cbr_pktlen=80, sample_rate=0 自动重采样 48k→DAC)→ DAC`。barge-in = AEC + VAD + 3 帧能量确认,触发后 `chat_break` + 清 rbuf + 排 stale mic + 1000ms 冷却 + 补发 onset 帧。停说判定:`TUYA_SERVER_VAD_ENABLE` 时等云端 `TAI_EVT_SERVER_VAD`(本地 2s 静音兜底),否则本地 VAD 直接判停说。OTA = 开机连 AI 前 `tuya_ota_check_and_upgrade`(ATOP 查升级 → 下载烧写 → 自动重启)。音乐 = 云端音乐 SKILL(`tuya_music.c` 并行重组+解析)→ TTS 报幕排空后让出 DAC → `app_music_tuya_play_url`(net_download https → mp3 解码 → DAC)→ 播完或 barge-in(VAD+3 帧能量门)停乐后恢复 TTS 播放器回听音。

---

## 2026-08 ~ 09 增量改动(STM 传输 / 上行 Opus 修正 / 打断确认门拆分)

> 以上 A/B/C 节为 7 月首版清单;本节记录其后到 2026-09-04 的增量。

### D. 新增 `tuya_agentic/stm/` 子目录(可选 UDP 传输后端,默认关)

| 文件 | 作用 |
|---|---|
| `libstm.a` | 涂鸦团队用杰理工具链预编译的 STM OPEN SDK(LTO bitcode 归档,725KB)。**原厂库,勿直接链接** |
| `libstm_tuya.a` | `patch_libstm.sh` 的补丁版(引擎线程栈 1KB→12KB、lwip `O_NONBLOCK` 常量翻译),Makefile/cbp 链接的是它 |
| `patch_libstm.sh` | 重打补丁脚本,仅换涂鸦新版原厂库时重跑(需 llvm-ar/bcrename/nm) |
| `stm_port_ac79_shim.c` | 补齐层:`stm_ac79_pthread_create`(设栈)、`stm_ac79_fcntl`(O_NONBLOCK 翻译) |
| `tuya_stm_ai.c/.h` | 适配层:以 `tstm_*` 实现 `tuya_ai.h` 的整套 `tai_*` 会话 API |
| `tuya_ai_select.h` | 编译期重定向:`TUYA_TRANSPORT_STM_ENABLE=1` 时 demo 的 `tai_*` 调用转到 `tstm_*`;=0 原样走 TCP |
| `include/`、`README.md`、`OPEN-SDK接入文档.md` | 原厂公开头文件与接入文档/说明 |

- 开关:`app_config.h` `TUYA_TRANSPORT_STM_ENABLE`(0=TCP 默认,1=UDP 优先)。两个后端同时编译共存,UDP 联调已完成(2026-09-04),默认仍回 TCP。
- 已知差异:云端 VAD 事件在 STM 通道不可区分(退化为本地静音兜底);MCP 回应走公开 TEXT 通道(`TUYA_STM_MCP_VIA_TEXT`)。详见 `stm/README.md`。

### E. 上行 Opus 在 STM/UDP 通道的 codec 定论(踩坑记录)

- 现象三连:`codec=3` 整轮静默(云端不认);`codec=111` 不带帧参数 → 解码器能跑但切不了帧(ASR 乱码/空);`codec=111 + bitrate=16000/frame_duration=40/frame_size=80` → **中文 ASR/NLG/TTS 全通**(2026-09-04 实测)。
- 依据:最新官方 agentic-kit 源码 `tuya_ai.h` 定义 `TAI_AUDIO_OPUS=111`;TCP 协议层(`tai_protocol.c`)对 Opus 自动从帧长推导帧参数(80B/帧 → 40ms → 16000bps),STM 通道无此逻辑,由适配层 `tuya_stm_ai.c` 显式补发。
- 2026-08-31 "codec=3 正确/111 乱码"的旧结论**作废**(当日受 MCP TEXT 污染干扰且缺帧参数)。TCP 通道不受影响(协议层自动推导)。

### F. barge-in 确认门与起轮门拆分(`tuya_agentic_demo.c`)

- 新增 `BARGE_CONFIRM_ENERGY=600000`:仅用于 TTS/音乐**播放中**的 3 帧打断确认;`BARGE_MIN_ENERGY=100000` 保留给**空闲起轮**单帧能量门。
- 证据(2026-09-04 深圳天气轮日志):TTS 回声 AEC 残留骗过 3/3 确认(各帧 sum <40 万)→ 播报被掐、碎片轮错乱;真人插话确认帧全部 >110 万。60 万居中(误触发余量 1.6×/真人余量 1.9×)。

### G. `audio_input.c` 录音 cbuf 三处修复(治上行丢话/抢丢 onset)

- 取数节拍:无条件 `mdelay(40)` 改为"不足一帧才短轮询等待",消除 STM 发送(~10ms)叠加导致的消费慢于生产、cbuf 涨满丢话。
- cbuf 满时保新帧:空间不足只丢最旧一帧再完整写入,替代旧逻辑"写 0 就 clear 整仓"(旧逻辑会把用户抢答的 onset 整段清掉)。
- 新增 TTS underrun 诊断计数(区分网络断流卡顿与解码颤音)。

### H. `app_config.h` 开关变化

- 新增 `TUYA_TRANSPORT_STM_ENABLE`(默认 0)、`TUYA_STM_LOG_PRINT_LEVEL`、`TUYA_STM_MCP_VIA_TEXT=1`/`TUYA_STM_MCP_INSTR_TYPE=1000`(STM 模式 MCP 回应通道,详见 `stm/README.md`)。
- 上行/下行 Opus、barge-in、云端 VAD、音乐等既有开关语义不变。
