# STM OPEN SDK(UDP 传输)接入说明

涂鸦团队用杰理工具链预编译的 STM OPEN SDK(`libstm.a`,LTO bitcode 归档,
target=pi32v2 / clang 4.0.1),为语音通道提供 **UDP(DTLS) 与 TCP 竞速、UDP 优先**
的传输层:UDP 连通即销毁 TCP 通道,UDP 不通自动回落 TCP。上层无法也无需指定
传输方式——因此本仓库的"TCP/UDP 开关"切换的是**后端 SDK 本身**。

## 1. 开关用法

`apps/wifi_story_machine/include/app_config.h`:

```c
#define TUYA_TRANSPORT_STM_ENABLE     0   // 0=TCP(rtc-tcp-client 源码,默认,行为不变)
                                          // 1=UDP 优先(本目录 STM OPEN SDK)
```

改完重新编译即可,无需改其他文件。两个后端可同时编译共存:demo 里的
`tai_*` 调用由 `tuya_ai_select.h` 在 STM 模式下重定向到 `tstm_*`;TCP 模式下
原样使用 `agentic-kit/modules/rtc-tcp-client`。

## 2. 目录内容

| 文件 | 作用 |
| --- | --- |
| `libstm.a` | 原厂库(725,474B,77 个 bitcode 成员),勿直接链接 |
| `libstm_tuya.a` | 补丁版，三处修正：①引擎线程 `pthread_create` 重命名 `stm_ac79_pthread_create`（栈 12KB）；②`lwip_fcntl` 重命名 `stm_ac79_fcntl`（O_NONBLOCK 常量翻译，见下）；链接这个 |
| `patch_libstm.sh` | 重打补丁脚本。**仅当厂商提供新版 libstm.a 时**在 ASCII 路径下重跑 |
| `include/stm_open.h` / `stm_typedef.h` / `stm_errno.h` | 原厂公开头文件 |
| `stm_port_ac79_shim.c` | 补齐层：①引擎线程栈 12KB（默认 1KB 会握手爆栈）；②`stm_ac79_fcntl` 做 O_NONBLOCK 常量翻译——libstm 按 Linux 头编译 O_NONBLOCK=0x4000，本平台 lwip_2_2_0 是 1（sockets.h:456），不翻译 lwip 按"不支持的标志位"返回 ENOSYS，libstm 报 **-12088**、UDP/TCP 建连全挂（2026-08-31 实测定位）。`fflush/fputs/fprintf` 不用补——`apps/common/c++/cxx_runtime.cpp` 已提供 |
| `tuya_stm_ai.h/.c` | 适配层:以 `tstm_*` 实现 `tuya_ai.h`(tai_*)的整套会话 API |
| `tuya_ai_select.h` | 传输层选择宏(STM 模式把 `tai_*` 重定向为 `tstm_*`) |
| `OPEN-SDK接入文档.md` | 原厂接入文档 |

## 3. 集成点(已全部接好,备忘)

- `app_config.h`:`TUYA_TRANSPORT_STM_ENABLE` 开关(默认 0)。
- `board/wl82/Makefile` + `AC791N_WIFI_STORY_MACHINE.cbp`:
  - include:`stm/`、`stm/include/`
  - 源文件:`stm/tuya_stm_ai.c`、`stm/stm_port_ac79_shim.c`(两者都带开关守卫,关闭时空翻译单元)
  - 库:`stm/libstm_tuya.a`(归档按需拉取:开关关闭时无符号引用,零影响)
- `tuya_agentic_demo.c`:include `tuya_ai_select.h`;两处 `iot_client_get_session_token`
  成功后调用 `tai_bind_session_token(token)`(TCP 模式空操作)。

## 4. 与 TCP 版(tai_*)的行为差异

- **连接参数来源**:完全取自云端下发的原始 session token(base64 串原样传给库,
  库自行解析 connect_conf/hosts/udpport 等)。`tai_config_t` 的 host/port/tls_sni/
  sign_level/ping 间隔等不再使用;`local_key`(加密密钥)、`client_type`、回调、
  `session_attrs_json`/`event_user_data_json`、`user_data` 仍用。
- **建连阻塞**:`stm_open_session_create` 内部 UDP/TCP 竞速,超时 30s,坏网络下
  该调用可能阻塞较久(tai 是 5s 级)。
- **连接生命周期**:libstm 引擎线程 + 全局连接在整个进程存活(`stm_open_init`
  一次,不随会话销毁);`tstm_disconnect` 只关会话,连接复用,下一轮建会话很快。
- **事件模型**:上行按"事件"组织(首轮带 event_id/参数/app_data,末包 fin=1),
  下行 `on_data_recv` + fin 合成 `TAI_STREAM_*` 流标记,fin 后补发 `TAI_EVT_END`。
- **云端 VAD**:`TAI_EVT_SERVER_VAD` 在 stm 下行通道中无法表达(指令类型号在
  open 层被吞掉,只有 break 可识别)→ 云端 VAD 模式实际退化为 demo 的本地静音
  兜底(约 2s)。建议 STM 模式下把 `TUYA_SERVER_VAD_ENABLE` 视同关闭,或让云端
  在 token/配置上直接控制 VAD。
- **打断与 MCP 回应**:走库**未公开**的底层 `stm_session_send()`(符号在
  libstm_tuya.a 中为全局导出),指令 type 4=break、1000=MCP(JSON-RPC 作载荷),
  与 tai 协议编号对齐但**格式未经云端确认**。发送前用 sid.id 与本侧 session_id
  的一致性校验兜底:若厂商库结构变化只会报 `TAI_ERR_PROTO` 并放弃发送,不会写坏
  内存。`tuya_stm_ai.c` 顶部 `TUYA_STM_PRIVATE_CMD_ENABLE` 置 0 可整体禁用。

## 5. 待云端/实测确认项(按优先级)

1. **token 必须带 `udpport`/`udpport_backup`**:libstm 的 token 解析对这两个字段
   是硬性要求(缺了直接报错)。当前 PID 的 token 是否携带需抓 log 确认;
   若没有需云端在签发 token 时补。
2. **上行 OPUS 的 codec_type 取值(已定论:3)**:文档音频表只列 101-PCM/108-SPEEX/
   109-MP3,但示例用 codec_type=3 标注 OPUS;tai TCP 协议里 OPUS=111。轮换实验
   (`TUYA_STM_CODEC_ALTERNATE=1`,奇数轮 3/偶数轮 111,2026-08-31 第六轮日志
   8 轮对话)结果:**codec=3 的轮次云端 ASR 全部正确**(如"天气怎么样?"lang=zh
   → 正确天气回答+TTS);**codec=111 的轮次 ASR 乱码/截断/空**("Un d."
   lang=fr、"今天。"、空文本)。→ 正确值定死 3(`TUYA_STM_OPUS_CODEC_TYPE 3`,
   `TUYA_STM_CODEC_ALTERNATE=0` 关轮换,开关留作复验)。
3. **打断指令格式**:上行 break 是否需要 SESSION_ID/EVENT_ID 属性、载荷是否
   要 2 字节事件号(tai TCP 版带),当前发的是裸 type=4 空载荷。
4. **MCP 回应的上行通道**:下行 MCP 命令以 CMD 数据类型到达但 open 层把指令
   类型号吞成 0(`[TSTM] mcp downlink cmd_type=0`),拿不到对称回应的权威
   类型号,只能通道实验:
   - 轮 A(2026-08-31,已完成):私有指令 type=1000(`TUYA_STM_MCP_INSTR_TYPE`,
     推测值)载 JSON-RPC → 云端**全程零反应**(连 initialized 通知/tools/list
     都没有,说明回应没进云端应用层)。
   - 轮 B(2026-08-31,已验证打通):公开 TEXT 数据事件载 JSON-RPC
     (`TUYA_STM_MCP_VIA_TEXT=1`)。**管道双向确实通**(回应 rc=0 后云端有下行),
     但第六轮日志实证:云端把 TEXT 当【用户聊天输入】——initialize 回应发出
     ~1.3s 后云端 NLG 答"你还是没有告诉我具体内容…"(把 JSON-RPC 串当用户
     说的话来回答),且该回复期间设备的语音上行云端不做 ASR(同期整句被吞,
     该轮无 ASR 事件)。
   - 轮 C(2026-09-02,反证实验,结论反转):VIA_TEXT 改回 0(私有 1000)后,
     云端对语音**全程零响应**——连 ASR 都不出(设备侧一切健康:codec=3 每轮
     确认、真实话音能量 sum 1.1M~1.8M、MCP 延迟发送 rc=0、biz_code/tag 与
     轮6完全一致,仅集群节点不同)。与第 2/3 轮私有指令时代"云端零反应"
     同现象,累计三次独立开机。**结论:initialize 的 TEXT 回应是云端语音
     管线激活的必要条件**——尽管它同时被当聊天输入污染一句。
     → 现默认 `TUYA_STM_MCP_VIA_TEXT=1`(回到已验证可用配置):代价是每会话
     开机后云端把 JSON 当用户输入答非所问一句,且该句播放期间说话无 ASR。
     **MCP 回应的官方通道/类型号仍需云端 FAE 确认**(届时改
     `TUYA_STM_MCP_INSTR_TYPE` 一处;另一候选实验:TEXT 载规范形状的
     initialize result(protocolVersion/capabilities/serverInfo),看云端是否
     走 MCP 解析而不落聊天路径)。
   demo 侧已修"回应 id 硬编码 1 不回带请求 id"的问题(云端按 id 匹配会丢弃)。
   回应发送线程约束见第 6 节(必须走 mcp_resp_defer/pump 延迟发送)。
5. **下行 MCP 命令识别**:open 层丢失指令类型号,当前靠"载荷是 { 且含
   \"jsonrpc\"" heuristic 识别,cmd_type 值已随识别打印(见第 4 条)。

## 6. 排查提示

- 库日志有**两道闸门**,都要开才能看到 DEBUG:
  ① 库内部阈值 `stm_open_set_log_level()`(曾钉死 WARN 导致应用层怎么设都
  看不到 DEBUG,2026-08-31 排查发现;`tuya_stm_ai.c` 的
  `TUYA_STM_LIB_LOG_LEVEL` 控制,**默认 WARN=3**——DEBUG 只在专项排查时
  短开,原因见下一条 printf 撞车);
  ② 应用侧过滤 `TUYA_STM_LOG_PRINT_LEVEL`(tuya_stm_ai.c 默认 3,
  app_config.h 排查期临时设 1;设 0=连 VERBOSE 全放,串口量大,仅排查时开)。
- **JL printf 无锁、不可重入,跨线程并发 printf 会崩**:`tstm_on_log` 是库
  引擎线程回调,2026-08-31 轮 B 开库 DEBUG 后回调高频 printf,与 demo 任务
  printf 撞车 → 音频首包发送时必崩 `axi_rd_inv`(两次开机寄存器逐位一致;
  日志中存在两条整行字符级交错的实锤;18:12 同代码仅库阈值 WARN 的构建从未
  崩,排除库自身/入参数据问题)。**已修**:`TUYA_STM_LOG_DEFER=1` 下回调只把
  日志行 memcpy 进环形缓冲(OS_ENTER_CRITICAL 保护,丢最旧),由 demo 任务
  周期调 `tai_log_flush()`(STM 下即 `tstm_log_flush()`,TCP 下空操作)统一
  printf——库日志唯一的打印出口在 demo 任务。今后任何"非 demo 线程要打日志"
  的需求都走这个环形缓冲模式,不要直接 printf。
- **不能在 on_data_recv 回调(引擎线程)里再进库发包——会与 demo 任务的
  音频上行在库内会话锁上互等卡死**(2026-08-31 第五轮实测:MCP initialize
  恰在音频上行中到达,回调里同步 `tai_send_mcp_response` 后 demo 任务从第
  3 帧起永久静默,无 exception、VAD 线程日志照常;轮 4 两次 MCP 都在空闲期
  到达所以没踩到)。**已修**:demo 侧 MCP 回应改延迟发送——回调只
  `mcp_resp_defer()` 存 pending,demo 任务在六处 20ms 级循环节拍
  `mcp_resp_pump()` 真正发送(与 `tai_log_flush` 同批调用点)。规律:
  **引擎线程回调里只许写内存/置标志,任何库调用(tai_send_*/tstm_send_*)
  都必须挪到 demo 任务做**。
- 连不上先看有没有 `[STM]` 日志:无 → `stm_open_init` 失败;有 `token` 相关
  错误 → 第 5.1 条;握手错误(-29 SSL_HANDSHAKE)→ 网络时间/证书问题。
- 引擎线程栈 12KB 在 `stm_port_ac79_shim.c`,别低于 8KB:爆栈表现为随机
  hardfault,不会给明确线索。
- `tstm_connect` 返回 `TAI_ERR_ARGS` 且 log 显示 "no session token bound" →
  demo 的 `tai_bind_session_token` 没执行到(token 申请失败)。
- 说话即重启、异常打印 `misalign_err`、fault 在 `topus_silk_biquad_alt_stride1`
  (opus 编码器内部)→ 是【opus 输入缓冲奇地址】,不是 STM/网络问题(2026-08-31
  实测定位)。`unsigned char` 缓冲默认 align 1,转 `short*` 进 libopus 做 16bit
  load,pi32v2 上奇地址=硬件对齐异常。TCP 旧构建不崩纯属栈布局运气。已修:
  demo.c 的 `abuf`/`g_barge_prebuf` 加 `__attribute__((aligned(4)))`,
  `tuya_opus_enc_frame()` 加奇地址防御(丢帧+`[OPUS-ENC] misaligned` 日志,不再
  硬崩)。今后任何要转 `short*` 进 opus 的缓冲都必须 4 对齐。

## 7. 验证情况

- `tuya_stm_ai.c` / `stm_port_ac79_shim.c` / `tuya_agentic_demo.c` 在开关
  0/1 两种模式下均通过 `clang -fsyntax-only -target pi32v2`(Makefile 同款
  include/define)。整包编译由使用者在 CodeBlocks 完成,未在本仓执行。
- 2026-08-31 实测(STM=1):UDP 建连/会话 ready/MCP 下行均通,opus 奇地址崩溃
  已定位修复(见第 6 节末条)。**第二轮实测**:说话不再重启,首轮连续上行
  3.7s/65 帧正常;但云端全程无响应(唯一下行是 MCP initialize,回应后即沉默),
  且等待回复的 60s 内设备对后续说话无反应(已加 10s 无 TTS 回听音兜底)。
  **第三轮实测**( instrumentation 生效):`mcp downlink cmd_type=0`(open 层
  吞类型号,无权威值可对);MCP 回应 id 回带正确、rc=0,云端依旧零反应;
  10s 兜底生效,第二轮 barge-in 90 帧上行完整,设备不再假死;libstm DEBUG
  日志仍全无 → 定位到库阈值钉在 WARN 的双闸门问题(见第 6 节,已修)。
  日志开头的 LOW POWER 复位时间点与烧录动作吻合,是烧录器复位,非运行故障。
  **轮 B 实测(2026-08-31 第四轮,双闸门全开 + MCP 走 TEXT + codec 轮换)**:
  ① 库 DEBUG/INFO 全量流出,UDP(DTLS)与 TCP 竞速建连、TCP 被销毁、会话
  create/ready、loss_rate 0.000000 全链路证据齐;② **MCP 走 TEXT 通道打通**——
  TEXT 回应 rc=0 后 ~1.5s 云端 NLG 文本流持续到达(bizType=NLG 多段 append,
  bizId=vcd-m-*,见待确认 #4);③ 但音频首包发送时必崩 `axi_rd_inv`(两次
  开机寄存器逐位一致),根因是库 DEBUG 洪流下引擎线程回调 printf 与 demo 任务
  printf 无锁撞车(证据与修复见第 6 节 printf 条),轮换实验因此未拿到干净
  数据;④ 异常打印里的 PC/CPU0 栈帧不可信(exception_analyze 自身地址,
  已用 sdk.elf nm 符号化核实),分析只认 exception reason/current_task/寄存器。
  **第五轮实测(printf 撞车修复后,19:33 构建)**:音频首包不再崩(全程无
  exception,printf 撞车修复确认生效);但暴露下一个问题——MCP initialize
  恰在音频上行中到达,回调(引擎线程)里同步发回应与 demo 任务的音频包在
  库内会话锁上互等,demo 任务永久卡死(后续三轮 VAD 起停正常、tuya_agentic
  任务零输出),云端也因此收不到 initialize 回应、整轮无 NLG/TTS。已改 MCP
  回应延迟发送(见第 6 节末条),等下一轮干净验证。
  **下一轮(printf 撞车修复后)看三个信号**:① 说话两轮以上,`[TSTM] audio
  start codec= turn=` 3/111 轮换下云端是否只对某一 codec 出 ASR/TTS(定死
  codec 值);② NLG 之后是否跟到 TTS 音频真正出声(此前崩在首包没验证到);
  ③ `[STM]` 前缀现在只应出现 WARN+(经环形缓冲延迟打印,时间戳略有滞后属
  正常)。若 ASR 仍空,转云端 FAE(token udpport/agent 挂 UDP 集群)。
  **第六轮实测(2026-08-31 19:43 构建,延迟日志+MCP 延迟发送,8 轮对话)**:
  ① 三个修复全部生效——全程无 exception、无卡死,MCP initialize 到达于空闲期
  `recv→deferred→resp(84B) rc=0` 30ms 内完成;② **UDP 语音链路首次全通**:
  云端 ASR/NLG 多段流/TTS 音频帧下行(on_audio len=80~640B)/打断回执
  (MQTT asrInterrupt)/云端 VAD 判停(chat_break→speak-stop: server-vad)
  全部工作,连续 barge-in 正常,tuya_agentic 任务健康(栈余 6KB/CPU 10%,
  堆余 4.8MB);③ 轮换实验定论:3→ASR 正确、111→乱码/空(见第 5.2 条);
  ④ MCP TEXT 通道证伪:云端把回应当用户聊天输入回答并吞同期语音(第 5.4 条);
  ⑤ **新根因:barge-in 排空吞音**——确认(120ms)+排空(~240ms,读帧按
  40ms/帧节拍)共 ~0.5s 恰好吞掉抢答整句,起上行只剩静音→ASR 空→"抢答了
  却不回复"(第 5/7 轮 codec=3 仍空 ASR 的铁证)。已修:排空帧改存
  g_barge_prebuf(3→12 帧=480ms)随 onset 一起补发(demo.c speak-start 前
  drain-stale 处)。已知遗留:ASR 空文本轮云端不发 chat_break,靠本地 2s
  静音兜底收尾(设计内,稍拖节奏);TTS 帧参数 codec/sr 打印为 0(库未回填
  音频参数,不影响播放)。
  **第七轮实测(2026-09-02 15:12 构建,VIA_TEXT=0 + codec 定死 3)**:设备侧
  全部健康——codec=3 每轮确认(`[TSTM] audio start codec=3` 新格式生效)、
  MCP initialize 在上行中到达且延迟发送正常(recv→deferred→resp 84B rc=0,
  死锁修复继续生效)、pre-TTS barge-in 起轮成功、抢答语音能量真实
  (confirm sums 1.1M~1.8M,f#1 sum=224291);但**云端对两轮语音全程零响应**
  (无 ASR/NLG/TTS/chat_break,唯一下行就是 initialize)。根因对照(第 5.4
  条轮 C):唯一功能差异是 initialize 回应走了私有指令 1000 而非 TEXT——
  TEXT 回应是云端语音管线激活的必要条件。已恢复 VIA_TEXT=1。另:等待期
  (④ 无 TTS)的第二段说话 14.5→16.0s 被忽略属已知设计(④ 的 barge-in
  检测以 g_tts_playing 为前提,10s 沉默兜底到期后恢复可打断)。
  **第八轮实测(2026-09-02 15:57 构建,TEXT 通道恢复)**:① 通道结论再次确认:
  MCP `resp(84 B) rc=0` 后约 1s 即恢复 NLG/TTS(on_audio #1~#5),说明上一轮
  “云端全程零反应”确为私有 type=1000 未激活管线;② 用户语音轮仍失败但原因已转到
  本地音频调度:第一次在启动污染 TTS 尚未结束时触发,只发 prebuf 1 帧、实时帧 0;
  第二次抢答确认成功后上行 123 帧,但 f#1/#26/#51/#76/#101 的 sum 仅
  203/581/984/243/347(全为静音),同时发送每帧约10ms又固定先 `mdelay(40)`,消费
  周期约50ms慢于录音40ms,水位从3.8KB涨到14KB后周期性跳回2.5KB——实锤录音
  cbuf 满时 `recorder_vfs_fwrite` 的旧逻辑整仓 `cbuf_clear`,反复丢掉用户话音窗口。
  已修:涂鸦录音写侧满仓时只丢最旧一帧保最新帧;读侧仅缺帧时短等、不再每次固定
  40ms;barge-in 起轮不再二次排 backlog;上行至少发送3个实时帧才允许被 TTS 状态
  截断。两文件 clang 语法检查通过,待下一轮验证。
