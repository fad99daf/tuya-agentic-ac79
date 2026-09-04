/*
 * tuya_stm_ai.h -- tuya_ai.h(tai_* API)的 STM OPEN SDK(libstm,UDP 优先)后端。
 *
 * 与 rtc-tcp-client(tai_*,TCP)等价的一套会话 API,函数签名逐一对齐
 * tuya_ai.h,但使用独立的 tstm_ 命名空间,两者可同时编译链接不冲突;
 * 由 tuya_ai_select.h 把 demo 里的 tai_* 调用按开关重定向到本实现。
 *
 * 差异(相对 tai TCP 版):
 *  - 连接参数不取自 tai_config_t.host/port,而是取自云端下发的原始
 *    session token(iot_client_get_session_token 的 base64 串),库内部
 *    自行解析 connect_conf/host 列表并 UDP(DTLS)与 TCP 竞速,UDP 优先。
 *    因此每次会话前必须先调用 tstm_set_session_token() 绑定原始 token。
 *  - 服务器 VAD(TAI_EVT_SERVER_VAD)在 stm 下行通道中无法区分表达,
 *    云端 VAD 模式请依赖本地静音兜底(见 app_config.h 注释)。
 */

#ifndef TUYA_STM_AI_H
#define TUYA_STM_AI_H

#include "app_config.h"

#if defined(CONFIG_TUYA_AGENTIC_ENABLE) && defined(TUYA_TRANSPORT_STM_ENABLE) && TUYA_TRANSPORT_STM_ENABLE

#include <stddef.h>
#include <stdint.h>
#include "tuya_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 绑定原始 session token(iot_client_get_session_token 输出的 base64 串,
 * 原样传入,库内部自行 base64 解码 + JSON 解析)。
 * 在 tstm_connect 之前调用;token 有效期约 24h,每次申请新 token 后重新绑定。
 */
void tstm_set_session_token(const char *raw_b64_token);

/* ---- 与 tai_* 逐一对齐的 API(签名/语义见 tuya_ai.h) ---- */
size_t     tstm_ctx_size(void);
tai_ctx_t *tstm_ctx_init(void *mem, const tai_config_t *cfg);
void       tstm_ctx_deinit(tai_ctx_t *ctx);

int        tstm_connect(tai_ctx_t *ctx);
void       tstm_disconnect(tai_ctx_t *ctx);
void       tstm_request_disconnect(tai_ctx_t *ctx);

int        tstm_send_text(tai_ctx_t *ctx, const char *text, size_t len);

int        tstm_send_audio_start(tai_ctx_t *ctx,
                                 uint8_t codec, uint8_t channels,
                                 uint8_t bit_depth, uint32_t sample_rate);
int        tstm_send_audio_chunk(tai_ctx_t *ctx, const uint8_t *data, size_t len);
int        tstm_send_audio_end(tai_ctx_t *ctx);

int        tstm_send_image(tai_ctx_t *ctx,
                           const uint8_t *data, size_t len,
                           uint8_t format, uint16_t width, uint16_t height);
int        tstm_send_image_with_text(tai_ctx_t *ctx,
                                     const char *text, size_t text_len,
                                     const uint8_t *img_data, size_t img_len,
                                     uint8_t format,
                                     uint16_t width, uint16_t height);

int        tstm_chat_break(tai_ctx_t *ctx);
int        tstm_send_mcp_response(tai_ctx_t *ctx, const char *json_rpc_response);

/* 刷出 TUYA_STM_LOG_DEFER=1 时缓冲的库日志(见 tuya_stm_ai.c:tstm_on_log)。
 * 由 demo 任务在 20ms 级循环节拍处周期调用;它是库日志唯一的 printf 出口,
 * 其它线程不要并发调(那会重新引入跨线程 printf 撞车)。TCP 后端无此需要,
 * tuya_ai_select.h 已把 tai_log_flush 映射为空操作。 */
void       tstm_log_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_TUYA_AGENTIC_ENABLE && TUYA_TRANSPORT_STM_ENABLE */

#endif /* TUYA_STM_AI_H */
