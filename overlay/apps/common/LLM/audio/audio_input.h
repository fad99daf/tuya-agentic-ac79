#ifndef AUDIO_INPUT_H_INCLUDED
#define AUDIO_INPUT_H_INCLUDED

#include "system/includes.h"
#include "server/audio_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_TYPE_G711A
// #define AUDIO_TYPE_AACLC
// #define AUDIO_TYPE_OPUS

typedef unsigned char UCHAR_T;
typedef char CHAR_T;
typedef signed int INT_T;
typedef int BOOL_T;

#ifndef IN
#define IN
#endif

#ifndef OUT
#define OUT
#endif

#ifndef INOUT
#define INOUT
#endif

#ifndef VOID
#define VOID void
#endif

#ifndef VOID_T
#define VOID_T void
#endif


#ifndef CONST
#define CONST const
#endif

#define AUDIO_DEBUG_ENABLE

#ifdef AUDIO_DEBUG_ENABLE
#define audio_debug(format, ...)      printf(format, ## __VA_ARGS__)
#else
#define audio_debug(...)
#endif


typedef struct {
    INT_T channel_num;
    INT_T bit_dept;
    INT_T sample_rate;
    INT_T audio_power_off;
} _AUDIO_PARAM;


int _device_write_voice_data(void *data, unsigned int len);
int _device_get_voice_data(void *data, unsigned int max_len);
void _device_wbuf_clear(void);   /* 清录音 cbuf:每轮对话开始前清掉轮间积压的旧音频 */
unsigned int _device_get_voice_level(void);  /* 录音 cbuf 当前数据量(字节),诊断用 */
void _device_rbuf_clear(void);   /* 清播放 cbuf:barge-in 打断 TTS 时清掉残留音频 */
void tts_prebuffer_arm(void);    /* TTS 本轮预蓄水:on_audio 收到 START 帧时调用 */
void audio_stream_init(int sample_rate, int bit_dept, int channel_num);
void start_audio_stream(void);
void stop_audio_stream(void);
int get_recoder_state();

#ifdef __cplusplus
}
#endif

#endif // AUDIO_INPUT_H_INCLUDED
