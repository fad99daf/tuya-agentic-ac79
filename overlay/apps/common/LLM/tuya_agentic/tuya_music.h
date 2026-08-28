/*
 * tuya_music.h -- 涂鸦音乐技能(Music Skill)下行解析。
 *
 * 云端音乐技能通过 on_text 文本通道回 SKILL JSON(整体一份,可能跨分片):
 *   {"bizType":"SKILL","data":{"code":"music","general":{
 *       "action":"play","data":{"audios":[{
 *           "name":"开不了口","artist":"周杰伦","album":"范特西",
 *           "format":"mp3","url":"https://...mp3","audioId":"...",...}]}}}}
 * 本模块负责:文本流重组(START/MIDDLE/END)→ 判音乐响应 → 取第一首的
 * url/歌名/歌手暂存。实际起播/等播完由 tuya_agentic_demo.c 的语音循环
 * 调 app_music 的网络解码器完成(见 app_music_tuya_play_url)。
 *
 * 线程模型:全部接口只在涂鸦 worker(tuya_pal)线程调用(on_text/on_event),
 * pending 结果由语音循环线程在"本轮回答结束"之后读取——轮次边界即同步点。
 */
#ifndef TUYA_MUSIC_H
#define TUYA_MUSIC_H

/* 每次会话(语音循环)开始时调:丢弃上一会话残留的半截流/未消费的音乐结果 */
void tuya_music_reset(void);

/* on_text 喂入一个文本分片(内容按 msg->len 拷贝,回调切片无 '\0' 结尾)。
 * 返回 1=一条完整流重组完毕(已就地解析,音乐响应已暂存);
 *      0=继续收;-1=该流被丢(超缓冲/无结束帧等,已打印原因)。*/
int tuya_music_text_accum(int stream_flag, const char *text, unsigned int len);

/* on_event 收 TAI_EVT_END 时兜底调:SDK 会丢空文本帧,半截流可能等不到
 * 显式 END。有未交付内容就强行交付解析,返回 1=交付了,0=没有。*/
int tuya_music_text_flush(void);

/* 本轮是否解析出了可播放的音乐(URL http/https 已校验) */
int  tuya_music_pending(void);
const char *tuya_music_get_url(void);     /* 试听 mp3 地址(NUL 结尾) */
const char *tuya_music_get_name(void);    /* 歌名(仅打印) */
const char *tuya_music_get_artist(void);  /* 歌手(仅打印) */
void tuya_music_clear_pending(void);      /* 交接消费后清掉,防下轮误重播 */

#endif /* TUYA_MUSIC_H */
