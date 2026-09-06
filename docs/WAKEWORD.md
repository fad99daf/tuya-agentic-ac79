# 唤醒词("嘿tuya")子系统说明

> 版本:v6.1 正式门控(2026-09-05 标定回填,2026-09-06 gap_threshold 跨句拼装修复)
> 代码:`apps/common/LLM/tuya_agentic/kws/` + `tuya_agentic_demo.c` 集成点
> 引擎:涂鸦闭源 KWS 引擎(`kws/audio_subsys.a`,内嵌 40 类 CTC "heytuya" 模型,pi32v2 版)

---

## 0. 一句话

麦克风 PCM 在空闲/播放期间持续喂闭源 KWS 引擎,命中"嘿tuya"后播 `WakeHeyTuya.mp3` 应答、
开 15s 唤醒窗——**窗内本地 VAD 才允许起上行轮**。引擎初始化失败自动回退"常听"
(`tuya_kws_awake()` 恒真),行为与未接入唤醒词时完全一致。

---

## 1. 文件清单

| 文件 | 作用 |
|---|---|
| `kws/audio_subsys.a` | 涂鸦闭源引擎静态库(2.7MB,内嵌模型) |
| `kws/keyword_tflite.h` | 引擎 C API 头(逆向重建 v3:config 结构 14 字段 / result / `forward_pcm` 等) |
| `kws/kws_cxx_shim.cc` | C↔C++ ABI 垫片(引擎是 C++ 实现) |
| `kws/tuya_kws.c` | 薄封装:初始化/喂音/命中处理/唤醒窗/探针日志(全部踩坑与标定结论在注释里) |
| `kws/tuya_kws.h` | 对外接口说明(init / feed / awake / window_kick) |
| `tuya_agentic_demo.c` | 集成点:各喂音路径 + 唤醒门 + 吞咽窗 + 抑制抢答(见 §5) |
| `app_music.c` | `app_music_tuya_play_wake_prompt()` 播应答提示音;音乐打断排空帧喂引擎 |
| `include/app_config.h` | `TUYA_KWS_ENABLE`(默认开) |
| `board/wl82/Makefile`(+`.cbp`) | kws 目录 include / 源文件(tuya_kws.c、kws_cxx_shim.cc)/ audio_subsys.a 链接 |
| `cpu/wl82/tools/audlogo/WakeHeyTuya.mp3` | 唤醒应答提示音(16kHz/mono,取自 TuyaOpen 标准应答"我在"资源) |

---

## 2. 引擎要点(标定定案,勿动)

- **唯一推理入口是 `keyword_tflite_forward_pcm()`**(一站式:AcceptWaveform → 5×80=400 滑窗堆叠
  → Forward → 归一化 → 解码搜索 → 命中写 result)。**绝不调 `detect()`**——它把 80 宽单帧直接喂
  Forward,撞 `CHECK(80≠400)` → `exit(-1)` 杀任务,对本模型永久弃用。
- 5 个配置是踩坑后的定案:

| 配置 | 值 | 原因 |
|---|---|---|
| `tensor_arena` | 49152 | 默认 98304 必触发库 CHECK 崩溃(构造器上限 49152) |
| `search_mode` | 1 | 仅 greedy:单关键词场景内存最省(静态 30144B + 堆 50336B) |
| `normalize_skip` | 0 | 默认 1 时 greedy 跳过后验归一化,heytuya 输出 log_softmax(恒≤0)→ 正数阈值永不满足(v2/v3 全天零命中根因) |
| `frame_threshold` | 0 | 默认 3 的时长门把单窗(30ms)token 段拦掉 11/12;误触约束交给距离+阈值 |
| `gap_threshold` | 100 | 命中最长跨度。默认 250(2.5s)会把**不同语句**的 token 拼成唤醒词(2026-09-06 实测:4.6s 前一句的 k27 + 新话音的 k08/k22 拼成 score 0.447 假唤醒,另一次命中跨 246 帧) |

- 引擎创建只试一次,失败不重试;堆余量用同尺寸 `malloc(50336)` 预探,避免构造器拿空指针直接崩。

---

## 3. 唤醒词与 token(标定结论)

- 模型 40 类 CTC id,blank 折叠后:"嘿tuya"的稳定声学指纹是 **[27,8] 对**(6/6 句,间隔恒 3 帧
  =30ms);词尾多变({22,5} / {22} / {1} / {38,1},部分被 blank 吃掉)。
- 注册两个正式关键词:`heytuya={27,8,22,5}`、`heytuya2={27,8,22}`。
- **greedy 扫描是"前缀窗 + 得分不足即跳过"规则**(逆向 IR 核实):每个起点只从 `len-容差` 的
  前缀窗开始试,得分不足直接跳过该起点、不再试更长的窗——**真正的触发路径就是前缀窗**,
  得分被 `(1-1/len)` 缩水:
  - len=2 的 `{27,8}`:首选窗=单 token 模糊窗,**结构性永不命中**(v6 实测 [27,8] 后验均值
    0.955 也没醒,这是 v6 教训的核心);
  - len=3 的 `{27,8,22}`:首选窗=[27,8] 稳定对本身,得分 =(2/3)×段均值;
  - 阈值 0.50 ⇒ [27,8] 均值 ≥0.75 即醒。标定集:4 句好发音(0.82~0.955)全中,
    1 句弱发音(0.503)正确不中。

---

## 4. 参数速查

| 参数 | 值 | 说明 |
|---|---|---|
| `TUYA_KWS_THRESHOLD` | 0.50 | 命中阈值(按前缀窗缩水后标定;旧值 0.6094 按整词标定,前缀窗结构性不可达,勿用) |
| `TUYA_KWS_AWAKE_MS` | 15000 | 唤醒窗:窗内允许 VAD 起轮;命中 / 起轮 / 答毕三点续期(`tuya_kws_window_kick`) |
| `TUYA_KWS_DUP_START_FRAMES` | 50 | 同句双命中拦截(\|Δstart\|<50 帧=同句):heytuya2 前缀窗与 heytuya 全词窗对同句先后命中(实测第二次可晚 4s 返回),只认第一次 |
| `cfg.gap_threshold` | 100 | 跨句拼装防护(默认 250);真词 [27,8] 间隔恒 3 帧、k22 尾 ≤40 帧,100 帧(1s)余量足 |
| 真词指纹 | k27→k08 恒 3 帧 | 日志判真假唤醒的第一依据;k22 在 ≤40 帧内跟上是第二依据;WAKE 行 score 0.49~0.66 为实测真词区间 |

---

## 5. 集成点(demo 侧)

**喂音路径**(PCM 16k/mono,引擎全程常开,不占独立任务):

- 空闲:idle drain 每 ~640ms 排 16 帧喂引擎(兼做空闲起轮能量门的数据源);
- 起轮 onset 帧进 prebuf 后不再经过喂音点,能量门处补喂一帧保持引擎流连续;
- barge-in 停播/静音验证后排 ~8 帧残响喂引擎(双讲下被 AEC 削掉的词尾,安静后有机会补认);
- 音乐打断(app_music 侧)同款排空喂引擎。

**唤醒后的行为**(`tuya_agentic_on_wake()` → demo 同线程处理):

- 播 `WakeHeyTuya.mp3`(TTS 播放态命中则置 pending,延迟到 `tts_barge_poll` 排空后补播);
- 开 900ms 词尾吞咽窗(`g_wake_swallow`,TuyaOpen `input_reset` 语义:提示音期间不吃新命令);
- 开 15s 唤醒窗;若上一拍有带历史 prefill 的抢答轮在途则取消之(唤醒词优先);
- TTS 播放中命中:走 wake_break(chat_break + 清播放缓冲 + 3s 残包排空窗)。

**唤醒门**(空闲起轮):`start_turn && !g_barge_in && (吞咽窗内 || !tuya_kws_awake())` → 吞回
idle(帧照常排空喂引擎)。能量确认过的抢答轮(g_barge_in)豁免唤醒窗——用户已在说话,窗过期
不能把打断后的命令拦回去。

---

## 6. 开关与降级

| 场景 | 行为 |
|---|---|
| `TUYA_KWS_ENABLE=0`(app_config.h) | 整块不编译,回到无唤醒词常听 |
| 引擎初始化失败(堆不足/库异常) | 自动回退 `tuya_kws_awake()` 恒真=门控失效,行为同常听;日志 `create fail, fallback always-listen`;只试一次不重试 |
| `TUYA_KWS_TOKENS_READY=0`(tuya_kws.c) | 标定模式:只注册探针不注册正式词,awake 恒假,话音全走 idle 排空喂引擎——对着设备说词,看 `[KWS-PROBE]` 采 token 序列 |

---

## 7. 探针([KWS-PROBE])

- k00~k39 每类一个单 token 关键词(阈值 0.0001),命中**只打印不唤醒**;
- 注册在正式关键词**之后**,库按注册序返回每窗首个命中 → 正式词优先消费窗口,探针只兜未被
  正式命中的窗。**所以探针日志不完整**(盲区),判真假唤醒以正式 `WAKE` 行 + 帧距为准;
- `[KWS-PROBE] alive fed=N hits=M err=K`:每 ~10s 心跳(err>0 = 引擎返回 -1,ABI 不匹配);
- 用途:漏唤醒时看引擎实际输出的 token 序列与分数(调参依据)。

---

## 8. 调参指南

- **漏唤醒**:看探针里 k27/k08 是否成对出现(间隔 ~3 帧)。成对 → 阈值偏高,0.50 → 0.45;
  不成对 → 发音/麦克风/前端问题,不是阈值;
- **误唤醒**:核对 WAKE 行的 `f=start..end` 跨度与 k27→k08 帧距——恒 3 帧=真;跨度/帧距大
  =跨句拼装(gap_threshold 已修,再出现考虑升阈值至 0.55);
- 阈值安全范围 **0.45 ~ 0.55**;gap_threshold 不要超过 150(再大跨句拼装风险回来)。

---

## 9. 资源与 flash

- `WakeHeyTuya.mp3` 放 `cpu/wl82/tools/audlogo/`,打包工具自动进资源区,播放走
  `app_music_play_voice_prompt()`(本地文件解码,16kHz/mono);
- 可选:删除 audlogo 下不用的原厂提示音(约 60 个 mp3)省 flash——开发树已删,不影响本功能;
  overlay 只带 `WakeHeyTuya.mp3` 一个,不覆盖你的 audlogo 其余内容。

---

## 10. 日志关键字速查

| 日志 | 含义 |
|---|---|
| `[TUYA-KWS] ready: gating ON heytuya={27,8,22,5} heytuya2={27,8,22}` | 引擎就绪,正式门控开 |
| `[TUYA-KWS] WAKE 'heytuya2' score=0.561 f=573..576` | 正式命中(跨度小 + score≥0.5 = 真词特征) |
| `WAKE dup 'heytuya' f=.. (last start ..), suppressed` | 同句第二次命中被拦(正常现象) |
| `[KWS-PROBE] hit=k27 score=.. f=..` | 探针命中(不唤醒,采 token 用) |
| `[KWS-PROBE] alive fed=N hits=M err=K` | 链路心跳(err 恒 0 为正常) |
| `[TUYA] wake alert: WakeHeyTuya.mp3` | 应答提示音起播 |
| `[TUYA] wake tail drained (vad closed)` | 词尾吞咽窗结束 |

---

## 11. 已知问题

- **探针盲区**(§7):被正式词消费的窗口探针看不到,probe 序列不完整;
- **48k 设备率下 AEC 劣化**(音乐交接后 opus 播放器重开把设备锁 48k,根因未修):故事/歌曲
  回声尖峰可能触发 VAD,由能量门 + 静音验证兜底(见 `CHANGES.md` 故事回声治理节);
  **唤醒词本身不受影响**——劣化态下真唤醒仍正常触发(2026-09-06 实测 4 次全真);
- **弱发音不唤醒**(后验 ~0.5):标定行为,稍大音量/靠近即中。
