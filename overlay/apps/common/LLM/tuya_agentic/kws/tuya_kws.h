#ifndef __TUYA_KWS_H__
#define __TUYA_KWS_H__
/* ============================================================================
 * tuya_kws.h — AC79 唤醒词("你好涂鸦"主 + "嘿涂鸦"兼容)薄封装
 *
 * 对齐 TuyaOpen 的 tkl_kws 流程：初始化 → 喂帧 → 唤醒事件。
 * 引擎 = apps/common/LLM/tuya_agentic/kws/audio_subsys.a(涂鸦闭源)。
 *
 * 行为设计(与 TuyaOpen ai_mode_wakeup 一致的"唤醒窗"模型):
 *  - 引擎初始化失败 / 关键词 token 未配置(见 tuya_kws.c 的 TODO) →
 *    tuya_kws_ready()==0，上层不启用唤醒门控，保持原有"常听"行为。
 *  - 命中唤醒词 → 打开 TUYA_KWS_AWAKE_MS 唤醒窗，窗内现有 VAD+能量门起轮
 *    照常工作；窗过期回睡眠，睡眠中起轮请求被吞掉、帧照常排空喂引擎。
 *  - tuya_kws_window_kick(): 每次成功起轮续窗，让追问不用反复喊唤醒词。
 * ==========================================================================*/

/* 初始化(幂等)。malloc 77KB 工作内存 + 建引擎 + 注册唤醒词。
 * 返回 0=引擎可用；非 0=初始化失败(上层回退常听模式)。 */
int  tuya_kws_init(void);

/* 引擎是否可用(= init 成功 且 关键词已注册)。 */
int  tuya_kws_ready(void);

/* 喂一帧 PCM(16k/16bit/单声, 40ms=1280 字节)。内部调用 forward_pcm,
 * 命中唤醒词时打印并打开唤醒窗。非阻塞, 未 ready 时直接返回。 */
void tuya_kws_feed(const void *pcm_frame, int bytes);

/* 当前是否处于唤醒窗内。引擎不可用时恒返回 1(门控自动失效=常听回退)。 */
int  tuya_kws_awake(void);

/* 续唤醒窗(成功起轮时调, 追问期间保持"醒着")。未 ready 时无操作。 */
void tuya_kws_window_kick(void);

#endif /* __TUYA_KWS_H__ */
