# 唤醒词("你好涂鸦"+"嘿涂鸦")子系统说明

> 版本:v7 官方 token(2026-09-07 声学团队 v2 算法包,当日真机验证唤醒"你好涂鸦"生效)
> 代码:`apps/common/LLM/tuya_agentic/kws/` + `tuya_agentic_demo.c` 集成点
> 引擎:涂鸦闭源 KWS 引擎(`kws/audio_subsys.a` v2,pi32v2 版,默认模型 fsmn_v8_0515_avg)

---

## 0. 一句话

麦克风 PCM 在空闲/播放期间持续喂闭源 KWS 引擎,命中唤醒词后播 `WakeHeyTuya.mp3` 应答、
开 15s 唤醒窗——**窗内本地 VAD 才允许起上行轮**。引擎初始化失败自动回退"常听"
(`tuya_kws_awake()` 恒真),行为与未接入唤醒词时完全一致。

---

## 1. 文件清单

| 文件 | 作用 |
|---|---|
| `kws/audio_subsys.a` | 涂鸦闭源引擎静态库 v2(6.6MB bitcode,内嵌声学团队模型库,默认模型见 §2) |
| `kws/keyword_tflite.h` | 引擎 C API 头(逆向重建 v3:config 结构 14 字段 / result / `forward_pcm` 等) |
| `kws/kws_cxx_shim.cc` | C↔C++ ABI 垫片(引擎是 C++ 实现) |
| `kws/tuya_kws.c` | 薄封装:初始化/喂音/命中处理/唤醒窗/探针日志(v7 官方 token) |
| `kws/tuya_kws.h` | 对外接口说明(init / feed / awake / window_kick) |
| `tuya_agentic_demo.c` | 集成点:各喂音路径 + 唤醒门 + 吞咽窗 + 抑制抢答(见 §5) |
| `app_music.c` | `app_music_tuya_play_wake_prompt()` 播应答提示音;音乐打断排空帧喂引擎 |
| `include/app_config.h` | `TUYA_KWS_ENABLE`(默认开) |
| `board/wl82/Makefile`(+`.cbp`) | kws 目录 include / 源文件(tuya_kws.c、kws_cxx_shim.cc)/ audio_subsys.a 链接 |
| `cpu/wl82/tools/audlogo/WakeHeyTuya.mp3` | 唤醒应答提示音(16kHz/mono,取自 TuyaOpen 标准应答"我在"资源;文件名沿用,与唤醒词无绑定) |

---

## 2. 引擎与模型(v2 算法包,2026-09-07)

- **v2 包构成**(llvm 工具核实):229+ 编译单元 = wekws 运行时 + TFLite Micro + tflite_signal
  + speexdsp + rnn_vad,并编入声学团队一整套模型(`*_model_data` 约 30 个:nhty_hty/anna/
  tino/多轮 epoch 快照等)。**引擎对象实际引用的默认模型只有一个:
  `g_fsmn_v8_0515_avg_int8_model_data`**(create(NULL) → "use default model" 路径),其余模型
  无人引用,LTO 链接后全部裁掉,固件体积不受影响。
- **引擎侧常量与 v1 完全一致**(IR 逐项核对):arena CHECK 上限 49152、TFLite 束堆分配
  50336、`keyword_tflite_*` C ABI 原样 → v1 时代踩坑标定的 5 项配置**全部继续有效**。
- **唯一推理入口是 `keyword_tflite_forward_pcm()`**(一站式:AcceptWaveform → 5×80=400 滑窗堆叠
  → Forward → 归一化 → 解码搜索 → 命中写 result)。**绝不调 `detect()`**——它把 80 宽单帧直接喂
  Forward,撞 CHECK(80≠400) → `exit(-1)` 杀任务,永久弃用。
- 5 个配置是 v1 踩坑后的定案(v2 不变):

| 配置 | 值 | 原因 |
|---|---|---|
| `tensor_arena` | 49152 | 默认 98304 必触发库 CHECK 崩溃(构造器上限 49152) |
| `search_mode` | 1 | 仅 greedy:单关键词场景内存最省(静态 30144B + 堆 50336B) |
| `normalize_skip` | 0 | 默认 1 时 greedy 跳过后验归一化,FSMN 模型输出 log_softmax(≤0)→ 正数阈值永不满足(v2/v3 全天零命中根因)。0=自动探测输出域并归一化 |
| `frame_threshold` | 0 | 默认 3 的时长门把单窗(30ms)token 段拦掉;误触约束交给距离+阈值 |
| `gap_threshold` | 100 | 跨句拼装防护(默认 250 会把不同语句的 token 拼成唤醒词);8 token 词跨度 ~24-40 帧,100 帧余量足 |

- 引擎创建只试一次,失败不重试;堆余量用同尺寸 `malloc(50336)` 预探,避免构造器拿空指针直接崩。

---

## 3. 唤醒词与 token(v7 官方,取代 v6.1 自标定)

声学团队随 v2 包官方提供 token 序列与阈值(2026-09-07),**不再需要探针标定**:

| 注册名 | 唤醒词 | token(CTC id) | 阈值 |
|---|---|---|---|
| `nihaotuya`(主) | 你好涂鸦 | `{23,4,27,9,22,5,38,1}`(8) | 0.70 |
| `heytuya`(兼容) | 嘿涂鸦 | `{27,8,22,5,38,1}`(6) | 0.70 |

- 注册名用 ASCII(`result.name` 走 strncpy/strcmp,串口打印也稳);唤醒词本身是中文发音,
  与 TuyaOpen T5AI 平台的"你好涂鸦"一致(那边是 tutuClear 引擎,词相同库不同)。
- greedy 扫描仍是"前缀窗+得分不足即跳过"规则(v6 逆向结论,对引擎恒成立):实际触发路径是
  "词长-容差"前缀窗,得分被 `(1-1/词长)` 缩水——len=8 ⇒ 前缀窗得分 ≈ 0.875×段均值。
  官方阈值 0.7 已按此机制真机验证有效(2026-09-07);若后续出现系统性漏唤醒,先看探针日志
  再考虑下调(安全下限见 §8)。
- **同句双命中拦截**(Δstart<50 帧)保留:两词共享"涂鸦"尾段 `{...,22,5,38,1}` 且 greedy
  容差下 9/8 可互替,说"你好涂鸦"可能两词相继各中一次;同一词的滑窗也可能重复返回。只认第一次。
- v6.1 的自标定序列(`{27,8,22}`/`{27,8,22,5}`、阈值 0.50)已被官方序列取代删除;标定过程
  与"前缀窗"规则的完整分析见 git 历史(v6.1 版 `tuya_kws.c` 注释)。

---

## 4. 参数速查

| 参数 | 值 | 说明 |
|---|---|---|
| `TUYA_KWS_THRESHOLD` | 0.70 | 命中阈值(v2 官方值,两词同值;前缀窗缩水后实测有效) |
| `TUYA_KWS_AWAKE_MS` | 15000 | 唤醒窗:窗内允许 VAD 起轮;命中 / 起轮 / 答毕三点续期(`tuya_kws_window_kick`) |
| `TUYA_KWS_DUP_START_FRAMES` | 50 | 同句双命中拦截(\|Δstart\|<50 帧=同句):两词共享尾段,同句可能双中,只认第一次 |
| `cfg.gap_threshold` | 100 | 跨句拼装防护;词内 token 间隔 ~3 帧、整词跨度 ≤40 帧,100 帧(1s)余量足 |
| `TUYA_KWS_CLASSES` | 40 | 探针覆盖类别数;v2 模型实际 vocab 以开机日志 `TFLite model: ... vocab=%d` 为准 |

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
| `TUYA_KWS_TOKENS_READY=0`(tuya_kws.c) | 标定模式:只注册探针不注册正式词,awake 恒假,话音全走 idle 排空喂引擎(留给以后换词标定用) |

---

## 7. 探针([KWS-PROBE])

- k00~k39 每类一个单 token 关键词(阈值 0.0001),命中**只打印不唤醒**;
- 注册在正式关键词**之后**,库按注册序返回每窗首个命中 → 正式词优先消费窗口,探针只兜未被
  正式命中的窗。**所以探针日志不完整**(盲区),判真假唤醒以正式 `WAKE` 行 + 帧距为准;
- `[KWS-PROBE] alive fed=N hits=M err=K`:每 ~10s 心跳(err>0 = 引擎返回 -1,ABI 不匹配);
- 用途:漏唤醒时看引擎实际输出的 token 序列与分数(调参依据)。v7 已有官方 token,探针仅作
  观测,不再承担标定职责。

---

## 8. 调参指南

- **漏唤醒**:先看探针里"你好涂鸦"的头四个 token(k23→k04→k27→k09)是否按序出现。
  按序出现但不 WAKE → 阈值偏高,0.70 → 0.65 逐档试;不按序 → 发音/麦克风/前端问题,不是阈值;
- **误唤醒**:核对 WAKE 行的 `f=start..end` 跨度与词内 token 帧距——词内间隔 ~3 帧、整词
  ≤40 帧为真;跨度/帧距大 = 跨句拼装(gap_threshold 已修,再出现考虑升阈值至 0.75);
- 阈值安全范围 **0.65 ~ 0.75**;gap_threshold 不要超过 150(再大跨句拼装风险回来)。

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
| `[TUYA-KWS] ready: gating ON nihaotuya={23,4,27,9,22,5,38,1} heytuya={27,8,22,5,38,1} (greedy thr=0.700 window=15000ms)` | 引擎就绪,正式门控开 |
| `[TUYA-KWS] WAKE 'nihaotuya' score=.. f=..` | 正式命中(说"你好涂鸦");`'heytuya'`=说"嘿涂鸦" |
| `WAKE dup 'heytuya' f=.. (last start ..), suppressed` | 同句第二次命中被拦(两词共享尾段,正常现象) |
| `[KWS-PROBE] hit=k27 score=.. f=..` | 探针命中(不唤醒,观测 token 用) |
| `[KWS-PROBE] alive fed=N hits=M err=K` | 链路心跳(err 恒 0 为正常) |
| `TFLite model: feature=.. vocab=N ..` | 引擎加载模型的自述行(v2 起 vocab 以此为准) |
| `[TUYA] wake alert: WakeHeyTuya.mp3` | 应答提示音起播 |
| `[TUYA] wake tail drained (vad closed)` | 词尾吞咽窗结束 |

---

## 11. 已知问题

- **探针盲区**(§7):被正式词消费的窗口探针看不到,probe 序列不完整;
- **48k 设备率下 AEC 劣化**(音乐交接后 opus 播放器重开把设备锁 48k,根因未修):故事/歌曲
  回声尖峰可能触发 VAD,由能量门 + 静音验证兜底(见 `CHANGES.md` 故事回声治理节);
  **唤醒词本身不受影响**——劣化态下真唤醒仍正常触发(2026-09-06 实测 4 次全真);
- v2 包内其余模型(nhty_hty epoch17 等)未被引用:若声学团队后续想切换默认模型,需改库内
  引用重出包,应用侧无法选择。
