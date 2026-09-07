# tuya-agentic-ac79

把 **涂鸦 agentic-kit**(AI Agent 云端语音对话)移植到 **杰理 AC791N (wl82) AIoT SDK** 的端侧对接代码。底层通过涂鸦 tRTC 实时通道与云端 AI 交互。

> 本仓只含 **对接代码**(新增的 `tuya_agentic/` + 对官方 SDK 的少量改动),**不含杰理 SDK 本体**,需配合官方 SDK 使用。README 写法参考了 [xiaozhi-esp32 涂鸦版](https://github.com/fad99daf/xiaozhi-esp32)。

---

## 功能特性

- **语音对话** — 实时语音交互(ASR + LLM + TTS)
- **唤醒词"你好涂鸦"+"嘿涂鸦"** — 涂鸦闭源 KWS 引擎(`kws/audio_subsys.a` v2 算法包,默认模型 fsmn_v8_0515_avg)常开识别,官方 token 注册双唤醒词:你好涂鸦={23,4,27,9,22,5,38,1}(主)、嘿涂鸦={27,8,22,5,38,1},阈值均 0.7。命中播"我在"应答并开 15s 唤醒窗,窗内本地 VAD 才起轮;同句双命中拦截、跨句拼装防护;引擎初始化失败自动回退"常听"。参数速查/调参指南见 [`docs/WAKEWORD.md`](docs/WAKEWORD.md)
- **上行 ASR** — opus(默认,本地 libopus 1.4 定点软编码,16k/mono/CBR 16kbps/40ms,~2KB/s 仅为 PCM 的 1/16)/ PCM(可选,32KB/s)。mic 管线保持 PCM(VAD/AEC/能量门/barge-in 全不受影响),仅在发送前逐帧编码;编码器初始化失败自动回退 PCM。TCP / UDP 两种传输下均已实测调通
- **下行 TTS** — opus(默认,~2KB/s 治拥挤网络卡顿)/ PCM(可选,稳定)。opus 已调通:CBR + `sample_rate=0` 自动重采样
- **传输层可选(TCP / UDP)** — 默认 TCP(`rtc-tcp-client` 源码直连,行为与历史版本一致);可切涂鸦团队预编译的 STM OPEN SDK(`stm/libstm_tuya.a`,UDP/DTLS 与 TCP 竞速、UDP 优先、不通自动回落),`TUYA_TRANSPORT_STM_ENABLE` 一个开关切换,两个后端同时编译共存。切换细节与已知差异见 `stm/README.md`
- **DNS 降噪强化** — 嘈杂环境 ASR 优化:强制开 DNS 位(不受 flash 旧配置影响)+ 降噪强度 over_drive=3,实测底噪基线 9k~64k → 3k~6k;若小声说话被吃,在 `user_cfg.c` 覆盖块回调 2.0~2.5
- **打断 (barge-in)** — AEC + VAD + 多帧能量确认,用户可随时打断 TTS(依赖 AEC)。空闲起轮与播报中打断用两道独立能量门(`BARGE_MIN_ENERGY` 10 万 / `BARGE_CONFIRM_ENERGY` 60 万),后者专门抗 TTS 回声的 AEC 残留误触发(实测残留确认帧 <40 万、真人插话 >110 万,取 60 万居中)
- **音乐播放** — "播放XXX的歌":云端音乐 SKILL 回试听 mp3 URL,设备解析后走杰理网络解码链播放(https 自动 TLS);TTS 报幕后让出 DAC、播完自动恢复,播放中说话可打断停乐(VAD+3 帧能量门)。⚠️ 试听片段 ~30s,完整歌曲需在涂鸦平台购买音乐高级能力授权
- **云端 VAD 停说判定** — 开口永远本地 VAD;停说由云端事件决定(TAI 2.1 协议经 ChatBreak 通知停说,代码兼容处理 ServerVad),带本地 2s 静音超时兜底(TCP 通道)。STM/UDP 通道下该事件暂不可区分,自动退化为本地静音兜底,见 `stm/README.md`
- **MQTT 常驻 + DP 下行** — MQTT 与 AI 的 TLS 各自独立连接并存,`tuya_mqtt_ka` 线程维持心跳收 DP 下行;App 里设备保持在线,云端下发的 DP/MCP 命令实时可收(`on_dp_downlink` / `on_event`)
- **TTS 首字预蓄水** — 每轮 TTS 开头先攒 ~160ms 音频再喂解码器,治首帧短包 underrun 卡顿
- **涂鸦云 OTA** — 开机连 AI 前检查升级,有新固件则下载烧写自动重启(双备份方式)
- **涂鸦 BLE 一键配网** — 用「涂鸦智能 App」蓝牙配网,速度快
- **凭据掉电保存** — 设备三元组(devid/secret/localkey)激活后写入 VM,后续开机直连
- **K6 长按重置配网** — 清除凭据并重新进入配网

> 说明:本端侧实现**不包含** 图片理解/生成、设备 MCP 等功能(端侧未实现)。云端 AI 能力以涂鸦平台配置为准。

---

## 硬件要求

- **杰理 AC791N (wl82)** 开发板(基于 SDK 的 `wifi_story_machine` 工程)
- 麦克风(上行 ASR)、喇叭(下行 TTS)
- 按键 **K1–K8**(PB1 上的 AD 阶梯);其中 **K6 = KEY_PHOTO,长按 = 清除配网重置**
- 串口(烧录 + 日志,默认 115200)

---

## 0. 前置条件

### SDK 版本(重要)

本对接代码基于 **杰理 AC79 AIoT SDK V1.2.0 release 包**(gitee 仓库 Releases 里的 `fw-AC79_AIoT_SDK-release-AC79NN_SDK_V1.2.0.zip`)。若用 git clone 方式,请 checkout **`AC79NN_SDK_V1.2.12_2026-03-07`**——该 tag 内容与 V1.2.0 release 包一致(patch 基线已按此逐字节验证;上游并没有名为 `AC79NN_SDK_V1.2.0` 的 tag)。其他版本源码不同,patch / overlay 会对不上。官方仓库:<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>

### 涂鸦 IoT 平台准备

使用前,在 [涂鸦 IoT 开发平台](https://iot.tuya.com) 完成以下准备:

| 前提 | 说明 | 获取方式 |
| --- | --- | --- |
| **产品 PID** | 标识一类设备,绑定 AI Agent | 平台创建产品后获得 |
| **设备授权码** (uuid / authkey) | 每台设备独立,用于激活换云端凭据 | 平台领取免费测试授权码 |

大致流程:
1. 在平台**创建产品**,获得 **产品 PID**
2. 为产品配置 **AI Agent**(系统提示词、TTS 语音类型、语言等)
3. **领取测试授权码**(uuid + authkey)
4. 把 PID / uuid / authkey 填入 `tuya_agentic_demo.c` `YOUR_PID_HERE` 处(文件内搜索定位)(见下文「快速开始」第 4 步)

> 测试阶段可在平台免费领取少量授权码;量产需联系涂鸦商务。

---

## 1. 快速开始

```bash
# 1) 取官方 SDK 并锁版本(与 V1.2.0 release 包一致的 tag)
git clone https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git
cd fw-AC79_AIoT_SDK
git checkout AC79NN_SDK_V1.2.12_2026-03-07

# 2) 取本对接仓(任意位置)
git clone <本仓地址> ../tuya-agentic-ac79

# 3) 合并(把对接代码覆盖进 SDK)
#    Linux / macOS / Git Bash:
bash ../tuya-agentic-ac79/apply.sh .
#    Windows CMD:
#    ..\tuya-agentic-ac79\apply.bat .

# 4) 填你自己的涂鸦凭证:编辑 SDK 里
#    apps/common/LLM/tuya_agentic/tuya_agentic_demo.c `YOUR_PID_HERE` 处(文件内搜索定位)
#    (仓库里是占位符,见下文「涂鸦凭证」)

# 5) 编译
make ac791n_wifi_story_machine
```

> ⚠ **命令行 make 注意**:杰理自带的老 GNU make(`C:/JL/mc/bin/make.exe`)不认识 Makefile 里的 `$(file >...)`,会**静默跳过 objs.txt 重写**——链接吃上次构建留下的旧 obj 列表(比如 CodeBlocks 的),产出混有旧代码的固件且不报错。命令行构建必须带全参数:`make -j8 MKDIR="mkdir -p" RM="rm -rf" LINK_AT=0`(本机无 `mkdir_win` 工具,故同时覆盖 MKDIR/RM;必要时先删 `sdk.elf` 再编)。用杰理 CodeBlocks IDE 构建不受影响。

合并后,`apps/common/LLM/tuya_agentic/` 即为对接代码;`apps/wifi_story_machine/` 已在开机时自启动涂鸦流程。

---

## 2. 两种合并方式

| 方式 | 命令 | 适用 |
|---|---|---|
| **overlay 覆盖**(默认,推荐) | Linux/Mac/Git Bash: `bash apply.sh <SDK根>` · Windows: `apply.bat <SDK根>` | 干净的官方 SDK,整文件覆盖 |
| **patch**(只打改动) | `cd <SDK根> && git apply patches/tuya-agentic-v1.2.0.patch` | 想逐行审查 / SDK 已有自己的修改 |

- overlay **覆盖 10 个 SDK 文件** + **新增 `tuya_agentic/` 整目录**。
- patch 只改那 10 个文件(新增目录仍需 overlay 复制)。
- 新增目录含自带 libopus 定点编码器与**预编译好的 `libopus_tuya.a`**——正常使用**无需**本地重编;只有修改了 `libopus/` 下源码才需重跑其 `build_tuya_libopus.sh`(需杰理 LLVM 工具链 `/c/JL/pi32/bin`)再编工程。`stm/` 下的 `libstm.a`(涂鸦原厂库)/`libstm_tuya.a`(平台适配补丁版)同理为预编译库,仅换涂鸦新版原厂库时才需重跑 `patch_libstm.sh`

> ⚠ **重跑 apply 会覆盖 `demo.c`,把你填的凭证盖回占位符**。若已填过凭证,后续改动请直接手改目标文件,或重跑后再填一次。

---

## 3. 涂鸦凭证

合并后,在 `apps/common/LLM/tuya_agentic/tuya_agentic_demo.c` **`YOUR_PID_HERE` 处(文件内搜索定位)**填**你自己**的涂鸦产品三件套。仓库里是占位符(**不含任何作者私有凭证**),请替换:

```c
#define TUYA_PRODUCT_KEY    "YOUR_PID_HERE"      /* PID:     涂鸦 IoT 平台 → 你的产品 → 产品ID */
#define TUYA_UUID           "YOUR_UUID_HERE"     /* UUID:    涂鸦 IoT 平台 → 设备 → UUID        */
#define TUYA_AUTH_KEY       "YOUR_AUTHKEY_HERE"  /* AuthKey: 涂鸦 IoT 平台 → 设备 → AuthKey     */
```

`TUYA_ACTIVATION_TOKEN`(默认占位 `xxxxxxxx`)是配网激活 token:仅在手动测激活时从平台/App 取一次填入,正常 App 配网**不需要**手填。

设备三元组(devid / secret / localkey)首次 BLE 配网时由云端激活并写入 VM(索引 176~180),掉电不丢,**无需手填**。

---

## 4. 配置开关

编辑 `apps/wifi_story_machine/include/app_config.h`:

| 宏 | 说明 | 默认 |
|---|---|---|
| `CONFIG_TUYA_AGENTIC_ENABLE` | 涂鸦集成总开关(控制 Makefile 编入 + K6 重置分支 + 自启动) | 开 |
| `TUYA_TRANSPORT_STM_ENABLE` | 语音传输层:0=TCP(rtc-tcp-client 源码);1=涂鸦 STM 库(UDP 优先,UDP/TCP 竞速自动回落),详见 `stm/README.md` | 0 |
| `TUYA_BARGE_IN_ENABLE` | 用户打断 TTS(强依赖 AEC,实验性) | 开 |
| `TUYA_KWS_ENABLE` | 唤醒词"你好涂鸦"(主)+"嘿涂鸦"门控(闭源引擎常开识别,失败自动回退常听),详见 `docs/WAKEWORD.md` | 开 |
| `TUYA_DOWNLINK_OPUS_ENABLE` | 下行 TTS 用 opus(治卡顿);注释则用 PCM | 开 |
| `TUYA_UPLINK_OPUS_ENABLE` | 上行 ASR 用本地 libopus 定点软编码(~2KB/s);注释则 PCM(32KB/s) | 开 |
| `TUYA_SERVER_VAD_ENABLE` | 云端 VAD 停说判定(开口仍本地 VAD;本地 2s 静音兜底) | 开 |
| `TUYA_MUSIC_ENABLE` | 音乐技能:解析音乐 SKILL 交网络解码链播放,支持说话停乐 | 开 |
| `TUYA_OTA_ENABLE` / `TUYA_FIRMWARE_VERSION` | 涂鸦云 OTA;版本号为手动方案(发版前改宏与平台一致) | 1 / "1.0.11" |

---

## 5. 配网与重置

### BLE 配网(首次)

1. 设备开机,无三元组 → 自动进入 BLE 广播,周期播报「请配置网络」
2. 手机打开 **涂鸦智能 App**(或 OEM app)→ 添加设备
3. App 搜到设备 → 通过 BLE 下发 WiFi 凭据 + 配网 token
4. 设备连 WiFi → 云端激活 → 三元组写入 VM → 连 AI
5. 后续开机读 VM 直接连 AI,无需重复配网

### 重置配网

- **长按 K6 键** → 清除 VM 三元组 → 软复位 → 重启自动重新配网
- (复位前会先停 BT 广播,避免软复位后 BT 控制器状态不干净导致配网失败,详见 FAQ)

---

## 6. 常见问题 (FAQ)

| 现象 | 原因 / 解决 |
|---|---|
| **涂鸦 App 搜不到蓝牙** | 烧的是占位符版,`demo.c` 里还是 `YOUR_PID_HERE`。填入真实 PID/uuid/authkey 重新编译 |
| **长按 K6 后配网失败**(按 reset 键却成功) | 软复位(P33)不像掉电那样彻底重置 BT 控制器。已在复位前停 BT + 延迟修复;若仍偶发,按 reset 键冷启动或重试 |
| **下行 TTS 有啸叫/杂音** | 若开 opus 后有异常,可切回 PCM(注释掉 `TUYA_DOWNLINK_OPUS_ENABLE`)。当前 opus 已调通(CBR + sample_rate=0 自动重采样) |
| **配网时连上就断(conn nack → 超时)** | 多为 2.4G 射频干扰。关掉手机 WiFi 再配、靠近设备、多试几次 |
| **音乐只播 ~30 秒就停** | 平台试听片段限制;完整歌曲需在涂鸦平台购买音乐高级能力授权 |
| **音乐放着放着自己停了** | 音乐 barge-in 误触发:音乐回采穿透了能量门。看日志 `[MUSIC-DBG]` 基线 sum,调高 `BARGE_CONFIRM_ENERGY`(`tuya_agentic_demo.c`) |
| **嘈杂环境 ASR 识别不准** | 看串口 `idle drain avg_sum`(底噪基线)与说话帧 `act%`。DNS 已强制开且 over_drive=3(`user_cfg.c` 覆盖块);若**小声说话被吃/识别反而变差**(过压制),把 `DNS_over_drive` 回调 2.0~2.5。注意 DNS 只压稳态噪声(风扇/嗡嗡),旁边人声/电视等非稳态噪声无解,需离麦近一点 |
| **说"你好涂鸦"没反应** | 看日志 `[KWS-PROBE]` 里"你"段 k23/k04、"好"段 k27/k09 是否依次出现:出现但分数贴线=阈值偏高(0.7 可降到 0.65),完全不出=发音/前端问题。调参详见 `docs/WAKEWORD.md` |
| **命令行 make 出的固件行为像旧代码** | 老 GNU make 静默跳过 objs.txt 重写,链接吃了上次构建的旧 obj 列表。带全参数重编并先删 `sdk.elf`:`make -j8 MKDIR="mkdir -p" RM="rm -rf" LINK_AT=0` |
| **patch 打不上 / 行号错位** | SDK 版本不对。必须用 `AC79NN_SDK_V1.2.12_2026-03-07`(内容 = 官方 V1.2.0 release 包,patch 基线) |

---

## 7. 已知问题 / 注意

- **下行 opus 已调通**(CBR + `sample_rate=0` 让解码器自动输出 48k 重采样到 DAC),默认开启。早期 `sample_rate=16000` 强制对齐会致慢速低沉音、去掉 CBR 会致解码器卡死,均已修复。
- **上行 opus 为本地 libopus 定点软编码**(符号全 `topus_` 前缀隔离,与杰理闭源 opus 库零冲突;仓内带预编译 `libopus_tuya.a`)。曾在另一场景记录过 clang+LTO 对本地 libopus **浮点解码**的崩溃(已弃用解码,仅保留定点**编码**);若上电/说话再现异常,注释 `TUYA_UPLINK_OPUS_ENABLE` 回 PCM 上行排查。修改 libopus 源码需重跑 `build_tuya_libopus.sh` 再编工程。
- **音乐打断是"停乐后听"**:打断的那句话与音乐混叠、不作数(不补发 ASR),音乐停了再说下一句。若出现"音乐自己把自己打断",按 `[MUSIC-DBG]` 日志调高 `BARGE_CONFIRM_ENERGY`(AEC 对连续音乐的效果未验证)。
- **OTA 版本号为手动方案**:`TUYA_FIRMWARE_VERSION` 每次发版前要改成与涂鸦平台填的一致。跨 OTA 自动持久化版本号(VM/USER/BTIF/RTC)经验证全部不可靠,故不采用。
- **barge-in 强依赖 AEC**,单麦环境下属实验性功能,调参细节见 [`docs/CHANGES.md`](docs/CHANGES.md)。播报中被 TTS 回声"自打断"(回复错乱、频繁掐断)时,调高 `BARGE_CONFIRM_ENERGY`(默认 60 万,2026-09-04 实测残留确认帧 <40 万、真人插话 >110 万)。
- **传输层默认 TCP**。切 UDP(`TUYA_TRANSPORT_STM_ENABLE 1`)的用法、原理与已知差异(云端 VAD 事件不可区分、MCP 回应走 TEXT 通道等)见 `stm/README.md`;STM 库为涂鸦团队预编译(LLVM IR 归档,725KB)。
- **版本绑定**:patch 基线 = 官方 V1.2.0 release 包内容(git tag `AC79NN_SDK_V1.2.12_2026-03-07`,已验证)。跟随官方 SDK 升级需重新生成 patch。

---

## 8. 改了哪些文件

完整清单见 [`docs/CHANGES.md`](docs/CHANGES.md)。摘要:

- **新增** `apps/common/LLM/tuya_agentic/`:`tuya_agentic_demo.c`(主流程)、`pal_ac791n.c`(PAL)、`le_net_cfg_tuya.c/.h`(BLE 配网)、`tuya_music.c/.h`(音乐技能解析)、`tuya_ota.c/.h`(OTA)、`tuya_opus_enc.c/.h` + `libopus/`(上行 opus 编码,含预编译库)、`stm/`(涂鸦 STM OPEN SDK 适配层:UDP 传输可选后端,`tuya_stm_ai.c` + `stm_port_ac79_shim.c` + 预编译库 + 重打补丁脚本)、`kws/`(唤醒词"你好涂鸦"+"嘿涂鸦":闭源引擎 `audio_subsys.a` 声学团队 v2 包 + 逆向 API 头 + C++ 垫片 + 薄封装,见 `docs/WAKEWORD.md`)、bool 兼容补丁、引入的 `agentic-kit/`
- **改动 SDK**(10 个文件):`audio_input.c/.h`、`user_cfg.c`(AEC)、`app_music.c`(K6 + 音乐播放导出 + 唤醒应答提示音)、`Makefile`、`AC791N_WIFI_STORY_MACHINE.cbp`、`app_config.h`、`wifi_app_task.c`、`app_main.c`(btstack 栈 768→2048)
- **新增资源** `cpu/wl82/tools/audlogo/WakeHeyTuya.mp3`(唤醒应答提示音,overlay 直拷;不覆盖 audlogo 下其余库存文件)
- **可选调试改动**:`board_7916A.c`(串口波特率,只为看日志)

---

## 9. 目录结构

```
tuya-agentic-ac79/
├── README.md            ← 本文件
├── README.en.md         ← 英文版
├── apply.sh             ← 合并脚本(Linux/Mac/Git Bash)
├── apply.bat            ← 合并脚本(Windows)
├── overlay/             ← 按 SDK 相对路径的对接代码
├── patches/
│   └── tuya-agentic-v1.2.0.patch   ← 10 个改动文件的 diff
└── docs/
    ├── INTEGRATION.md   ← 集成架构与原理
    ├── WAKEWORD.md      ← 唤醒词子系统:v2 引擎与官方 token / 参数 / 调参 / 日志
    └── CHANGES.md       ← 完整改动清单
```

---

## License

对接代码遵循 Apache-2.0(与杰理 SDK 一致)。`agentic-kit/` 下引入的涂鸦开源模块、AWS coreHTTP / coreMQTT 遵循各自原始许可;`tuya_agentic/libopus/`(libopus 1.4 子集,含预编译 `libopus_tuya.a`)遵循 BSD-3-Clause(见其 `COPYING`);`tuya_agentic/stm/` 下的 `libstm.a` / `libstm_tuya.a` 为涂鸦团队提供的预编译库,版权归涂鸦所有,随本仓仅为方便适配该 SDK 使用。

## 致谢

- [涂鸦 agentic-kit](https://github.com/tuya) — AI Agent 端侧 SDK
- [杰理 AC79 AIoT SDK](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK) — 芯片 SDK
- [xiaozhi-esp32 涂鸦版](https://github.com/fad99daf/xiaozhi-esp32) — README 结构参考
