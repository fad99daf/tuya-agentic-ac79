#ifndef TUYA_AGENTIC_H
#define TUYA_AGENTIC_H

#include "pal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AC791N 平台适配:返回 PAL 函数表(供 iot_init / tai_config.pal 用)*/
const pal_t *tai_pal_ac791n(void);

/* 涂鸦 AI 文本对话入口(WiFi 联网成功后 thread_fork 调它)。
 * 阶段0/1/2:先用硬编码的 devid/secret/local_key 跑通文本对话。*/
void tuya_agentic_demo(void *arg);

/* 完整流程入口(开机自动跑):
 *   有三元组(已配网激活过)→ 连 AI;
 *   无三元组 → BLE 配网 → 连 WiFi → 激活拿三元组并存 syscfg → 连 AI。*/
void tuya_agentic_main(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* TUYA_AGENTIC_H */
