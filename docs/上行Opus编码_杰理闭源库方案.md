# 上行 Opus 编码 —— 杰理闭源库 audio_server 方案(2026-09-20 定稿)

上行(设备→云端 ASR)音频编码的现行方案:不再本地移植 libopus,改用杰理闭源
opus 编码库(`cpu/wl82/liba/lib_opus_enc.a` / `lib_opus_stenc.a`,固件链接清单
本就含,无需新增链接),经 SDK audio_server 的 **"virtual" 源通道**驱动。实现全
部在 `apps/common/LLM/tuya_agentic/tuya_opus_enc.c/.h`,对 `tuya_agentic_demo.c`
只暴露 `tuya_uplink_send_frame()` 一类薄接口。

## 1. 背景与决策

| | 旧方案(已弃用) | 现方案 |
| --- | --- | --- |
| 编码器 | 本地 libopus 1.4 定点移植(`topus_` 前缀符号 + 预编译 `libopus_tuya.a`) | 杰理闭源库,针对 pi32v2 优化 |
| 运行位置 | demo 任务线程内联编码 | audio_server 编码任务线程 |
| 高压声学测试 CPU | `opus_encoder` 任务 ~95%,有整机降载风险 | 2-3% |
| 构建耦合 | Makefile/.cbp 需链 libopus 目录与 `libopus_tuya.a` | 零新增链接,摘除旧链接即可 |

`libopus/` 目录自 2026-09-20 起不再参与构建,仅在仓内保留作历史/回退参考
(回退方法见 §7)。

## 2. 架构与数据流

```
demo 任务线程                          audio_server 编码任务线程
─────────────                         ─────────────────────
tuya_uplink_send_frame(pcm 1280B)
  │ cbuf_write → s_inj_cbuf ──────────► read_input 回调拉 1280B
  │ os_sem_post(s_inj_sem)               (数据不足按 20ms 切片等待)
  │                                      杰理闭源库编码(80B CBR)
  │ os_sem_pend(s_out_sem, ≤120ms)     vfs_ops.fwrite
  │◄──────────────────── s_out_cbuf ◄── (写回输出 cbuf + post 信号量)
  ▼
取包返回(预期恰 80B)→ tai 上行
```

- 两条无锁 cbuf(各配一个信号量)做双向交接:输入侧 3 帧余量,输出侧 4 包余量。
- 编码发生在 audio_server 任务线程,**不占 demo 任务**;空闲时零编码、零忙转。
- mic PCM 管线(AEC/VAD/能量门/KWS/barge-in)完全不受影响——demo 仍是 mic
  cbuf 的唯一消费者,拿到 PCM 后只是把"发出去的字节"从 raw PCM 换成 opus 包。

## 3. 关键配置(`union audio_req`,AUDIO_REQ_OPEN)

| 字段 | 值 | 说明 |
| --- | --- | --- |
| `channel` | 1 | 单声道 |
| `sample_rate` | 16000 | 与 mic 一致 |
| `format` | `"opus"` | |
| `format_mode` | 0 | 裸 CBR 包(百度无头格式),涂鸦要的就是无封装 opus |
| `frame_ms` | 40 | 640 采样/帧,与 mic 帧等长 |
| `bitrate` | 16000 | 16kbps |
| `complexity` | 0 | 最快档,语音够用 |
| `frame_size` | 1280 | virtual 源下即 `read_input` 的 len |
| `sample_source` | `"virtual"` | 不接硬件源,数据由 `read_input` 提供 |
| `read_input` / `vfs_ops` | 本文件回调 | 输入拉取 / 编码结果回写 |
| `vir_data_wait` | 1 | 等数据齐再编码,不丢帧 |

**包长推导**:CBR 下 `bitrate × frame_ms / 8000 = 16000 × 40 / 8000 = 80` 字节/包。
`tuya_opus_enc_frame()` 前 10 包打长度核对日志,此后偶发非 80B 限频打点——
**实测返回值为准,勿盲发**(twetalk 场景 24k×60ms=180B 定长读帧已验证该公式)。

云端契约与旧版一致:上行 opus=111 且必带帧参数(tai 协议层从帧长推导
80B/帧→40ms/16000bps)。

## 4. 资源与故障模型

- **单例常驻**:`init` 成功后编码通道跨会话/重连复用,不随一轮对话重建。
- **堆积保护**:同步协议下注入/输出队列常态为空;检测到残留即丢旧保新并计数
  (`inject backlog` / 丢大包),不炸通道。
- **超时语义**:`frame()` 等包上限 120ms(3 帧时间,正常 <5ms);超时丢本帧
  保链路,send 侧按成功处理,不留空洞。
- **异常路径**:audio_server `ERR`/非预期 `END` 置 `s_dead`,后续 `frame()` 拒绝
  服务,demo 会话内自动回退 PCM 上行;`deinit` 主动收尾(`s_closing`)触发的
  `END` 属预期。
- **include 约束**:本文件不得 include newlib `<stdio.h>`(audio_server.h 经
  fs/fs.h 引入 SDK 的 FILE,与 agentic-kit 侧冲突),printf 用 SDK 头自带声明。

## 5. demo.c 侧集成要点(`tuya_agentic_demo.c`)

- `use_opus_uplink` 分支覆盖全部三处上行:barge **预填**(历史帧回灌)、
  **实时**帧、**尾部冲刷**(`tail flush: N frames` 日志)。
- ★ **历史教训(2026-09-20 修复的致命 bug)**:opus 会话里**绝不能**用
  `tai_send_audio_chunk` 直发 raw PCM——旧尾部冲刷就这么干,1280B 裸 PCM 混进
  opus 流,云端解码器流被打断,下一轮 gRPC `INVALID_ARGUMENT`,该轮 ASR/TTS
  全无(音乐打断场景必现)。任何新增的上行路径都必须走 `tuya_uplink_send_frame()`。
- settle(静音判定)排水循环带 `&& !g_barge_in`:barge-in 进行中不排水,
  避免吞掉预填队列头部(话音开头丢失)。

## 6. 验证(2026-09-20,真机 + 云端双侧)

- 10 轮对话(含 2 轮音乐播放中打断)全部正常出 ASR/LLM/TTS,云端零解码错误。
- **逐包完整性核对**:云端 OSS 全轮音频 vs 设备侧日志,包数精确匹配——
  50 包 = 13 预填 + 35 实时 + 2 冲刷;23 包 = 14 + 1 + 8;
  全部 80B 包两两 Hamming 距离分析(最小 116bit,随机基线 ~320bit)证明
  **零丢失、零重复**;每包 TOC 单字节 = 干净 CBR 流。
- **CPU**:`opus_encoder` 2-3%(旧本地库 ~95%),aec 68%,agentic ~2%。
- 已知开放项(非链路问题):音乐播放中打断说话的轮次,话音头部混音乐/AEC
  残留,云端 ASR 偶发语种漂移(zh→fr),待声学侧继续优化。

## 7. 回退与排障

| 手段 | 做法 |
| --- | --- |
| 回 PCM 上行 | 注释 `app_config.h` 的 `TUYA_UPLINK_OPUS_ENABLE`(编码器文件整体空编译,固件不留编码通道) |
| 会话级回退 | 编码器 init 失败/通道异常时 demo 自动回退 PCM,不废会话 |
| 回退到本地 libopus | 恢复 Makefile/.cbp 的 libopus 链接条目(见 README"构建"节),改过源码才需重跑 `build_tuya_libopus.sh` |

常用日志:`[OPUS-ENC] inited` / `pkt #N len=80` / `pkt len != 80` /
`encode timeout` / `inject backlog` / `read_input len=...` / `audio_server ERR`。

## 8. 历史注记

- **2026-08-28**:clang+LTO 对本地 libopus *浮点解码* 产生 axi_wr_inv 崩溃,
  当时弃用解码、仅留定点编码。
- **2026-09-17**:声学测试期云端 ASR 偶发语种误判/乱码,上行切 PCM(09-03 已
  全链路验证 codec=101)。
- **2026-09-20**:换杰理闭源库 + 修复尾部冲刷 raw PCM bug 后复测定稿 opus
  (带宽 1/16,CPU 2-3%);本地 libopus 摘除链接、目录保留。
