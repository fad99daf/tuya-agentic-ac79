/*
 * tuya_ai_select.h -- 语音通道传输层选择(TCP / UDP)开关。
 *
 * 用法:在 #include "tuya_ai.h" 之后紧接着 #include "tuya_ai_select.h"。
 *
 *  - TUYA_TRANSPORT_STM_ENABLE=1(app_config.h):
 *      tai_* 调用被重定向到 stm/tuya_stm_ai.c 的 tstm_*(STM OPEN SDK,
 *      UDP(DTLS) 与 TCP 竞速、UDP 优先);tai_bind_session_token() 绑定
 *      原始 session token。原 rtc-tcp-client(tai_*.c)仍参与编译,但其
 *      符号不再被引用,不参与最终链接。
 *  - 未定义/0(默认):
 *      使用原 rtc-tcp-client(TCP)实现;tai_bind_session_token() 为空操作。
 *
 * 注意:重定向表只覆盖函数名;tai_config_t/TAI_* 常量等类型与常量两种
 * 后端完全共用,业务代码无需感知。
 */

#ifndef TUYA_AI_SELECT_H
#define TUYA_AI_SELECT_H

#if defined(CONFIG_TUYA_AGENTIC_ENABLE) && defined(TUYA_TRANSPORT_STM_ENABLE) && TUYA_TRANSPORT_STM_ENABLE

#include "tuya_stm_ai.h"

#define tai_ctx_size             tstm_ctx_size
#define tai_ctx_init             tstm_ctx_init
#define tai_ctx_deinit           tstm_ctx_deinit
#define tai_connect              tstm_connect
#define tai_disconnect           tstm_disconnect
#define tai_request_disconnect   tstm_request_disconnect
#define tai_send_text            tstm_send_text
#define tai_send_audio_start     tstm_send_audio_start
#define tai_send_audio_chunk     tstm_send_audio_chunk
#define tai_send_audio_end       tstm_send_audio_end
#define tai_send_image           tstm_send_image
#define tai_send_image_with_text tstm_send_image_with_text
#define tai_chat_break           tstm_chat_break
#define tai_send_mcp_response    tstm_send_mcp_response
#define tai_log_flush            tstm_log_flush   /* demo 任务周期调用:刷出缓冲的库日志 */

/* 绑定原始 session token(stm 后端需要;tcp 后端不需要,空操作) */
#define tai_bind_session_token(p) tstm_set_session_token(p)

#else /* TCP(rtc-tcp-client) */

#define tai_bind_session_token(p) ((void)(p))
#define tai_log_flush()           ((void)0)   /* TCP 后端日志无延迟缓冲,无事可刷 */

#endif /* TUYA_TRANSPORT_STM_ENABLE */

#endif /* TUYA_AI_SELECT_H */
