#include "app_config.h"
#include "audio_input.h"
#include "system/includes.h"
#include "device/device.h"
#include "event/device_event.h"
#include "usb/host/usb_host.h"
#include "usb/host/audio.h"
#include "server/audio_server.h"
#include "server/server_core.h"
#include "generic/circular_buf.h"

#define MSG_START_NET_AUDIO_PLAY   1
#define MSG_START_LOCAL_AUDIO_PLAY   2
#define MSG_STOP_NET_AUDIO_PLAY   3
#define MSG_STOP_LOCAL_AUDIO_PLAY   4
#define MSG_START_AUDIO_RECORDER   5
#define MSG_STOP_AUDIO_RECORDER   6
#define MSG_START_AUDIO_TEST   7
#define MSG_STOP_AUDIO_TEST  8
#define MSG_AUDIO_RECORDER_FIRST_PLAY_NEXT_TEST   9
#define MSG_STOP_AUDIO_RECORDER_AND_START_PLAY_TEST   10
#define MSG_AUDIO_RECORDER_FIRST_PLAY_NEXT_STOP_PLAY_TEST   11


#define AUDIO_PLAY_VOICE_VOLUME   50    /* 下行 TTS 播放音量(0-100)。原 80 太大,改 50。要运行时调见 AUDIO_DEC_SET_VOLUME */
#define AUDIO_RECORD_VOICE_VOLUME 100

#if defined CONFIG_VOLC_LLM_ENABLE
#define AUDIO_RECORD_VOICE_UPLORD_LEN (320)
#elif defined CONFIG_ONESDK_LLM_ENABLE
#define AUDIO_RECORD_VOICE_UPLORD_LEN (3200)
#elif defined CONFIG_IFLY_AIUI_ENABLE
#define AUDIO_RECORD_VOICE_UPLORD_LEN (640)
#elif defined CONFIG_TWETALK_ENABLE
#define AUDIO_RECORD_VOICE_UPLORD_LEN (180)
#elif defined CONFIG_TUYA_AGENTIC_ENABLE
#define AUDIO_RECORD_VOICE_UPLORD_LEN (1280)  /* PCM 16k/16bit/mono 40ms=1280B/帧;opus format_mode=0 是百度无头,涂鸦解不了 */
#else
#define AUDIO_RECORD_VOICE_UPLORD_LEN (1280)
#endif

#define _AUDIO_TASK_NAME    "audio_task"
#define _AUDIO_WAIT_TIMEOUT    500
#define _AUDIO_UAC_TRY_TIMES    10

#define MSG_AUDIO_RECORDER_FIRST_PLAY_NEXT_TEST_STOP   0
#define MSG_AUDIO_RECORDER_FIRST_PLAY_NEXT_TEST_START   1
#define MSG_AUDIO_RECORDER_FIRST_PLAY_NEXT_TEST_RECORDER_START   2
#define MSG_AUDIO_RECORDER_FIRST_PLAY_NEXT_TEST_PLAY_START   3

int recoder_state = 0;

typedef struct {
    char *play_src;
    int   play_src_len;
    int   play_len;
} _AUDIO_LOCAL_MSG;

typedef unsigned short WORD;

typedef struct {
    cbuffer_t pcm_cbuff_w;
    cbuffer_t pcm_cbuff_r;
    struct server *enc_server;
    struct server *dec_server;
    u8   status;
    bool is_audio_play_open;
    bool is_audio_record_open;
} _AUDIO_HANDLE;

typedef struct {
    OS_SEM    r_sem;
    OS_QUEUE msg_que;
    bool pcm_wait_sem;
    void *task_handle;
} _AUDIO_CTRL;

typedef struct {
    unsigned int        cmd;
    u8                  *data;
    unsigned int       	data_len;
} _AUDIO_CTRL_MSG;

static _AUDIO_CTRL g_audio_ctrl = {0};
static _AUDIO_HANDLE g_audio_hdl = {0};
static _AUDIO_LOCAL_MSG  g_local_file = {0};
static unsigned int SAMPLE_RATE = 8000;//48000;//8000;
static unsigned int CHANNEL     = 1;
static unsigned int BIT_DEP     = 16;
static char audio_data[AUDIO_RECORD_VOICE_UPLORD_LEN] = {0};
static unsigned int voice_buf_size = AUDIO_RECORD_VOICE_UPLORD_LEN;
static int audio_power_off = 0;
static int audio_uac_retry_times = 0;
static unsigned int audio_test_recorder_first_play_next = 0;
static unsigned int audio_test_recorder_first_play_times = 0;

/***************************************audio*********************************************/

static int _send_audio_msg(IN const unsigned int msgid, IN const void *data, IN const unsigned int len)
{
    int op_ret = 0;
    int msg_num = 0;

    if (!&g_audio_ctrl.msg_que) {
        return -1;
    }

    _AUDIO_CTRL_MSG *msg_data;
    msg_data = malloc(sizeof(_AUDIO_CTRL_MSG) + 1);

    if (!msg_data) {
        return -1;
    }

    memset(msg_data, 0, sizeof(_AUDIO_CTRL_MSG) + 1);
    msg_data->cmd = msgid;

    if (data && len) {
        msg_data->data = malloc(len + 1);

        if (!msg_data->data) {
            if (msg_data) {
                free(msg_data);
            }

            return -1;
        }

        memset(msg_data->data, 0, len + 1);
        memcpy(msg_data->data, data, len);
        msg_data->data_len = len;
    } else {
        msg_data->data = NULL;
    }

    op_ret = os_q_post(&g_audio_ctrl.msg_que, msg_data);

    if (0 != op_ret) {
        if (msg_data->data) {
            free(msg_data->data);
        }

        if (msg_data) {
            free(msg_data);
        }

        return op_ret;
    }

    return 0;
}

int _device_get_voice_data(void *data, unsigned int max_len)
{
    cbuffer_t *cbuf = (cbuffer_t *)&g_audio_hdl.pcm_cbuff_w;
    unsigned int rlen;
    static flag = 1;
#if defined CONFIG_ONESDK_LLM_ENABLE || defined  CONFIG_VOLC_LLM_ENABLE
    mdelay(20);
#elif defined CONFIG_IFLY_AIUI_ENABLE
    mdelay(1);
#elif defined CONFIG_TWETALK_ENABLE
    mdelay(60);
#elif defined CONFIG_TUYA_AGENTIC_ENABLE
    mdelay(40);                /* PCM 40ms/帧(1280B),按帧节拍取数 */
#else
    mdelay(30);
#endif
    rlen = cbuf_get_data_size(cbuf);

    if (rlen == 0) {
        //    audio_debug("device_get_voice_data no data");
        return 0;
    } else {
        // audio_debug("device_get_voice_data %d",rlen);
    }

    if (voice_buf_size <= cbuf_get_data_size(cbuf)) {

        rlen = rlen > max_len ? max_len : rlen;
        cbuf_read(cbuf, data, rlen);
        // printf("get audio_data: %d\n", rlen);
        return rlen;
    }

    return 0;
}

void _device_rbuf_clear()
{
    cbuf_clear(&g_audio_hdl.pcm_cbuff_r);
}

void _device_wbuf_clear()
{
    cbuf_clear(&g_audio_hdl.pcm_cbuff_w);
}

unsigned int _device_get_voice_level(void)
{
    return cbuf_get_data_size(&g_audio_hdl.pcm_cbuff_w);
}

unsigned int _device_get_play_level(void)
{
    return cbuf_get_data_size(&g_audio_hdl.pcm_cbuff_r);   /* 下行 TTS 播放 cbuf 水位:排空≈DAC 播完 */
}

int _device_write_voice_data(void *data, unsigned int len)
{
    cbuffer_t *cbuf = (cbuffer_t *)&g_audio_hdl.pcm_cbuff_r;
    if (len > 0) {
#if defined(CONFIG_TUYA_AGENTIC_ENABLE) && defined(TUYA_DOWNLINK_OPUS_ENABLE)
        /* Opus 下行:云端帧长可变(400B/640B 实测),整包直写 cbuf,不做帧重组。
         * 杰理 opus 解码器内部按标准 Opus 帧边界自行切分。*/
        unsigned int write_len = cbuf_write(cbuf, data, len);
        if (write_len != len) {
            if (write_len == 0) {
                static unsigned int dl_full_cnt = 0;
                if (++dl_full_cnt % 50 == 1) {
                    printf("[AUDIO] dl cbuf full drop #%u\r\n", dl_full_cnt);
                }
            }
        }
#else
        unsigned int write_len = cbuf_write(cbuf, data, len);
        if (write_len != len) {
            /* 写不满(缓冲满)时【不】再 cbuf_clear——那会瞬间丢掉整缓冲→明显截断。
             * 改丢本次新数据(丢最新一小段,缓冲里已存的继续平滑播放)。统计满溢次数便于诊断:
             * 计数涨得快=云端突发/解码跟不上;不涨=是网络停顿 underrun(靠大缓冲扛)。*/
            if (write_len == 0) {
                static unsigned int dl_full_cnt = 0;
                if (++dl_full_cnt % 50 == 1) {
                    printf("[AUDIO] dl cbuf full drop #%u (level=%u)\r\n",
                           dl_full_cnt, cbuf_get_data_size(cbuf));
                }
            }
        }
#endif
    }

    os_sem_set(&g_audio_ctrl.r_sem, 0);
    os_sem_post(&g_audio_ctrl.r_sem);
    //此回调返回0录音就会自动停止
    return len;
}

#if defined(CONFIG_TUYA_AGENTIC_ENABLE)
/* ===== TTS 首字抗卡顿:预蓄水 =====
 * 问题:云端智能体回复开头常带语气词(嗯/好的/…),TTS 生成时首帧音频很短(80B=40ms),
 *       解码器拿到就播,40ms 后缓冲空 → underrun 卡顿。
 * 解法:本轮 TTS 首次 fread 时,先等 cbuf 攒够 TTS_PREBUFFER_BYTES 再返回给解码器。
 *       后续 fread 不再蓄水(只要消费≤生产就持续,蓄水只在开头做一次)。
 * g_tts_prebuffer_done:0=本轮还没蓄够,1=已蓄够(后续直接走原逻辑)。
 *       新一帧 stream_flag=START 时 on_audio 清零,触发本轮重新蓄水。*/
static volatile int g_tts_prebuffer_done = 1;   /* 默认1:开机/纯文本demo不蓄水 */
#define TTS_PREBUFFER_BYTES   (80 * 4)          /* 4 帧 opus ≈ 160ms,够 DAC 平滑启动不卡顿 */

/* on_audio 收到本轮 START 帧时调:标记需要预蓄水(cbuf 清空 + 标志清零)。
 * 由 tuya_agentic_demo.c 的 on_audio 在 stream_flag==START 时调用。*/
void tts_prebuffer_arm(void)
{
    g_tts_prebuffer_done = 0;
}
#endif

//编码器输出PCM数据
static int recorder_vfs_fwrite(void *file, void *data, unsigned int len)
{
    static int cnt;
    int ret;
    cbuffer_t *cbuf = (cbuffer_t *)file;

    if (0 == cbuf_write(cbuf, data, len)) {
        // 上层buf写不进去时清空一下，避免出现声音滞后的情况
        cbuf_clear(cbuf);
    } else {
        // audio_debug("cbuf write %d data", len);
    }

    return len;
}

static const struct audio_vfs_ops recorder_vfs_ops = {
    .fwrite = recorder_vfs_fwrite,
    .fopen = 0,
    .fread = 0,
    .fseek = 0,
    .ftell = 0,
    .flen = 0,
    .fclose = 0,
};


/**
 * @berief: 初始化录音功能,录取PCM的数据,存储到PCM buff中
 * @param   int
 * @return: none
 * @retval: none
 */

static void audio_recoder_init()
{
    audio_debug("audio recoder init ");
    int err;
    static bool flag = FALSE;
    union audio_req req = {0};

    flag = TRUE;

#if defined CONFIG_ONESDK_LLM_ENABLE || defined  CONFIG_VOLC_LLM_ENABLE || defined CONFIG_IFLY_AIUI_ENABLE
    req.enc.frame_size = SAMPLE_RATE / 100 * 4 * CHANNEL;        //收集够多少字节PCM数据就回调一次fwrite
#else
    req.enc.frame_size = SAMPLE_RATE / 100 * 4 * CHANNEL * 2;        //收集够多少字节PCM数据就回调一次fwrite
#endif
    req.enc.output_buf_len = req.enc.frame_size * 3; //底层缓冲buf至少设成3倍frame_size
    req.enc.cmd = AUDIO_ENC_OPEN;
    req.enc.channel = CHANNEL;
    req.enc.volume = AUDIO_RECORD_VOICE_VOLUME;
    req.enc.sample_rate = SAMPLE_RATE;

#ifdef AUDIO_TYPE_G711A
    req.enc.format = "pcm";
#elif defined(AUDIO_TYPE_AACLC)
    req.enc.format = "aac";
    req.enc.bitrate = SAMPLE_RATE * 4;
    req.enc.no_header = 0;
#elif defined(AUDIO_TYPE_OPUS)
    req.enc.format = "opus";
#endif

#ifdef CONFIG_TWETALK_ENABLE
    req.enc.format      = "opus";
    req.enc.bitrate     = 24000;
    req.enc.format_mode = 0;
    req.enc.frame_ms    = 60;
#endif
#ifdef CONFIG_TUYA_AGENTIC_ENABLE
    /* 涂鸦 AI 上行用 PCM:杰理 opus format_mode=0 是"百度无头"格式(非标准 opus),
     * 涂鸦 ASR 解不了→秒回空。云端下行也用 PCM(codec=101=TAI_AUDIO_PCM),改 PCM 最匹配。 */
    req.enc.format      = "pcm";
    req.enc.bitrate     = 24000;
    req.enc.format_mode = 0;
    req.enc.frame_ms    = 60;
#endif
    req.enc.sample_source = "mic";
    req.enc.vfs_ops = &recorder_vfs_ops;
    req.enc.file = (FILE *)&g_audio_hdl.pcm_cbuff_w;
    req.enc.channel_bit_map = BIT(1);

    if (CHANNEL == 1 && !strcmp(req.enc.sample_source, "mic") && (SAMPLE_RATE == 8000 || SAMPLE_RATE == 16000)) {
        req.enc.use_vad = 1;                    //打开VAD断句功能
        req.enc.vad_auto_refresh = 1;   //VAD自动刷新
    }

    if (req.enc.use_vad == 1) {
        req.enc.vad_start_threshold = 300;    //ms
        req.enc.vad_stop_threshold  = 0;    //ms
    }
#ifdef CONFIG_TUYA_AGENTIC_ENABLE
    /* 本地 VAD 阈值:起检调低(少丢话头、压低 barge-in/起轮延迟);收尾给真实值(原 0 不触发 SPEAK_STOP)。
     * 起检从 150→100:VAD 更快开口,但多出的假触发由 turn-start/barge-in 两道能量门(BARGE_MIN_ENERGY)
     * 兜底过滤,不会回流成自言自语。实测 barge-in 从发声到切 TTS ~345ms→~295ms。*/
    if (req.enc.use_vad == 1) {
        req.enc.vad_start_threshold = 100;    //ms
#ifdef TUYA_BARGE_IN_ENABLE
        req.enc.vad_stop_threshold  = 600;    //ms — 800→600 压低"说完→回话"等待(省 ~200ms);代价:长停顿可能被截断
#else
        req.enc.vad_stop_threshold  = 600;    //ms
#endif
    }
#endif

#ifdef CONFIG_AEC_ENC_ENABLE
    struct aec_s_attr aec_param = {0};
    req.enc.aec_attr = &aec_param;
    req.enc.aec_enable = 1;

    extern void get_cfg_file_aec_config(struct aec_s_attr * aec_param);
    get_cfg_file_aec_config(&aec_param);

    if (aec_param.EnableBit == 0) {
        req.enc.aec_enable = 0;
        req.enc.aec_attr = NULL;
    }

#if defined CONFIG_ALL_ADC_CHANNEL_OPEN_ENABLE && defined CONFIG_AISP_LINEIN_ADC_CHANNEL && defined CONFIG_AEC_LINEIN_CHANNEL_ENABLE

    if (req.enc.aec_enable) {
        aec_param.output_way = 1;               //1:使用硬件回采 0:使用软件回采

        if (aec_param.output_way) {
            req.enc.channel_bit_map |= BIT(CONFIG_AISP_LINEIN_ADC_CHANNEL);             //配置回采硬件通道

            if (CONFIG_AISP_LINEIN_ADC_CHANNEL < CONFIG_PHONE_CALL_ADC_CHANNEL) {
                req.enc.ch_data_exchange = 1;     //如果回采通道使用的硬件channel比MIC通道使用的硬件channel靠前的话处理数据时需要交换一下顺序
            }
        }
    }

#endif

    if (req.enc.sample_rate == 16000) {
        aec_param.wideband = 1;
        aec_param.hw_delay_offset = 50;
    } else {
        aec_param.wideband = 0;
        aec_param.hw_delay_offset = 75;
    }

#endif

    err = server_request(g_audio_hdl.enc_server, AUDIO_REQ_ENC, &req);
    printf("err:%d", err);
}


static void _audio_recoder_stop()
{
    int err;
    union audio_req req = {0};
    req.enc.cmd             = AUDIO_ENC_STOP;
    err = server_request(g_audio_hdl.enc_server, AUDIO_REQ_ENC, &req);
    audio_debug("_audio_recoder_stop err=%d", err);

}


//解码器读取PCM数据
static int audio_play_net_vfs_fread(void *file, void *data, unsigned int len)
{

    cbuffer_t *cbuf = NULL;
    unsigned int cbuf_len = 0;
    unsigned int rlen = 0;
    unsigned int c_rlen = 0;
    cbuf = (cbuffer_t *)file;

#if defined(CONFIG_TUYA_AGENTIC_ENABLE)
    /* TTS 首字预蓄水:本轮首次 fread 时,cbuf 水位 < 阈值则等 on_audio 继续写,
     * 攒够 TTS_PREBUFFER_BYTES 再返回。避免首帧 80B 直接播→40ms 后 underrun 卡顿。
     * g_tts_prebuffer_done 蓄够后置 1,本轮后续 fread 走原逻辑(不再蓄水)。
     * 超时保护:最长等 _AUDIO_WAIT_TIMEOUT×若干次(≈2s),防云端半路断流卡死解码器。*/
    if (!g_tts_prebuffer_done) {
        unsigned int pb_wait = 0;
        unsigned int pb_lvl0 = cbuf_get_data_size(cbuf);
        while (cbuf_get_data_size(cbuf) < TTS_PREBUFFER_BYTES &&
               g_audio_hdl.is_audio_play_open && pb_wait < 2000) {
            g_audio_ctrl.pcm_wait_sem = 1;
            os_sem_pend(&g_audio_ctrl.r_sem, _AUDIO_WAIT_TIMEOUT);
            g_audio_ctrl.pcm_wait_sem = 0;
            pb_wait += _AUDIO_WAIT_TIMEOUT;
        }
        g_tts_prebuffer_done = 1;   /* 无论蓄够还是超时,都放行(超时则有多少播多少) */
        printf("[TTS-PB] armed lvl0=%u→%u wait=%ums %s\r\n",
               pb_lvl0, cbuf_get_data_size(cbuf), pb_wait,
               cbuf_get_data_size(cbuf) >= TTS_PREBUFFER_BYTES ? "OK" : "TIMEOUT");
    }
#endif

    do {
        cbuf_len = cbuf_get_data_size(cbuf);
        rlen = cbuf_len > len ? len : cbuf_len;
        c_rlen = cbuf_read(cbuf, data, rlen);

        // printf("cbuf_len=%d , clen=%d, len=%d", cbuf_len, c_rlen, len);
        if (c_rlen > 0) {
            //audio_debug("c_rlen=%d rlen=%d cbuf_len=%d len=%d",c_rlen,rlen,cbuf_len,len);
            break;
        }

        //此处等待信号量是为了防止解码器因为读不到数而一直空转
        if (FALSE == g_audio_hdl.is_audio_play_open) {
            rlen = 0;
            break;
        }

        g_audio_ctrl.pcm_wait_sem = 1;
        os_sem_pend(&g_audio_ctrl.r_sem, _AUDIO_WAIT_TIMEOUT);
        g_audio_ctrl.pcm_wait_sem = 0;

    } while (1);

    //返回成功读取的字节数
    return rlen;
}

static int audio_vfs_fseek(void *file, u32 offset, int orig)
{
    /* printf("audio_vfs_fseek %d", offset); */
    return 0;
}

static const struct audio_vfs_ops audio_play_net_vfs_ops = {
    .fread  = audio_play_net_vfs_fread,
    .fwrite = 0,
    .fopen = 0,
    .fseek = audio_vfs_fseek,
    .ftell = 0,
    .flen = 0,
    .fclose = 0,
};

/**
 * @berief: 初始化播放功能,播放PCM的数据,从pcm_buff中读取数据播放
 * @param   int
 * @return: none
 * @retval: none
 */
static void audio_player_net_init()
{
    int err;
    union audio_req req = {0};

    req.dec.cmd             = AUDIO_DEC_OPEN;
    req.dec.volume          = AUDIO_PLAY_VOICE_VOLUME;
    req.dec.output_buf_len  = 8 * 1024;
    req.dec.priority        = 1;
    req.dec.channel         = CHANNEL;  /*dac 差分输出 单路*/
    req.dec.sample_rate     = SAMPLE_RATE;
    req.dec.vfs_ops         = &audio_play_net_vfs_ops;
#ifdef AUDIO_TYPE_G711A
    req.dec.dec_type 		= "pcm";
#elif defined(AUDIO_TYPE_AACLC)
    req.dec.dec_type 		= "aac";
#elif defined(AUDIO_TYPE_OPUS)
    req.dec.dec_type 		= "opus";
#endif

#ifdef CONFIG_TWETALK_ENABLE
    req.dec.dec_type        = "opus";
    req.dec.channel         = 0;
    req.dec.sample_rate     = 0;
    req.dec.attr |= AUDIO_ATTR_OPUS_CBR_PKTLEN_TYPE;
    req.dec.opus_cbr_pktlen = 180;
#endif
#if defined(CONFIG_TUYA_AGENTIC_ENABLE) && defined(TUYA_DOWNLINK_OPUS_ENABLE)
    /* === opus 下行 ===
     * ★ sample_rate=0:让解码器自动输出(内部 48k),audio_server 自动重采样到 DAC。
     *   之前 sample_rate=16000 导致 48k PCM 按 16k 播 → 3倍慢速低沉音。
     * ★ CBR 模式必须保留:杰理 opus 解码器靠 CBR pktlen 确定帧边界,去掉会卡死。
     * ★ channel=0:让解码器自动(参考 twetalk)。
     * ★ opus_cbr_pktlen=80:16kbps×40ms/8。*/
    req.dec.dec_type        = "opus";
    req.dec.channel         = 0;
    req.dec.sample_rate     = 0;     /* ★ 自动:让解码器输出 48k,重采样到 DAC 16k */
    req.dec.attr           |= AUDIO_ATTR_OPUS_CBR_PKTLEN_TYPE;
    req.dec.opus_cbr_pktlen = 80;
#elif defined(CONFIG_TUYA_AGENTIC_ENABLE)
    /* === PCM 下行(默认)=== 稳定能播,但 16k=32KB/s 拥挤测试网易卡顿。channel/sample_rate 沿用上面
     * 已设的 CHANNEL(1)/SAMPLE_RATE(16k)。云端实测回 codec=101(PCM)。开 TUYA_DOWNLINK_OPUS_ENABLE 切 opus。*/
    req.dec.dec_type        = "pcm";
#endif
    req.dec.sample_source   = "dac";
    req.dec.file            = (FILE *)&g_audio_hdl.pcm_cbuff_r;
    audio_debug("audio_player_net_init ");
    err = server_request(g_audio_hdl.dec_server, AUDIO_REQ_DEC, &req);

    if (err) {
        audio_debug("server_request dec_server AUDIO_DEC_OPEN err %d", err);
        goto __err;
    }

    req.dec.cmd = AUDIO_DEC_START;
    server_request(g_audio_hdl.dec_server, AUDIO_REQ_DEC, &req);
    return;
__err :

    return;

}

static void _audio_player_stop()
{
    int err;
    union audio_req req = {0};
    req.dec.cmd             = AUDIO_DEC_STOP;
    err = server_request(g_audio_hdl.dec_server, AUDIO_REQ_DEC, &req);
    audio_debug("_audio_player_stop err=%d", err);

}

static void enc_server_event_handler(void *priv, int argc, int *argv)
{

    switch (argv[0]) {
    case AUDIO_SERVER_EVENT_ERR:
    case AUDIO_SERVER_EVENT_END:
        break;
    case AUDIO_SERVER_EVENT_SPEAK_START:
        /* printf("speak start\n"); */
        recoder_state = 1;
        break;
    case AUDIO_SERVER_EVENT_SPEAK_STOP:
        /* printf("speak stop\n"); */
        recoder_state = 0;
        break;
    default:
        break;
    }
}

//*****************************收音和播放************************
void _device_net_audio(bool flag)
{
    audio_debug("ty_device_net_audio %d", flag);

    if (flag) {
        //音频播放功放
        _send_audio_msg(MSG_START_NET_AUDIO_PLAY, 0, NULL);
        _send_audio_msg(MSG_START_AUDIO_RECORDER, 0, NULL);
    } else {
        _send_audio_msg(MSG_STOP_NET_AUDIO_PLAY, 0, NULL);
        _send_audio_msg(MSG_STOP_AUDIO_RECORDER, 0, NULL);
    }

}

void _device_net_audio_play(bool flag)
{
    audio_debug("_device_net_audio_play %d", flag);

    if (flag) {
        //音频播放功放
        _send_audio_msg(MSG_START_NET_AUDIO_PLAY, 0, NULL);
    } else {
        _send_audio_msg(MSG_STOP_NET_AUDIO_PLAY, 0, NULL);
    }

}

void _device_net_audio_recorder(bool flag)
{
    audio_debug("_device_net_audio_recorder %d", flag);

    if (flag) {
        //音频播放功放
        _send_audio_msg(MSG_START_AUDIO_RECORDER, 0, NULL);
    } else {
        _send_audio_msg(MSG_STOP_AUDIO_RECORDER, 0, NULL);
    }

}
//***************************************************************
bool is_audio_play_open(void)
{
    return g_audio_hdl.is_audio_play_open;
}

static void __audio_task(void *pArg)
{
    int op_ret = 0;
    _AUDIO_CTRL_MSG *msg_data;
    int msg[16] = {0,};

    while (1) {
        //阻塞等待消息
        op_ret = os_q_pend(&g_audio_ctrl.msg_que, 0, msg);

        if (op_ret != 0) {
            if (op_ret != -1) {
                audio_debug("tal_queue_fetch op_ret:%d", op_ret);
            }

            continue;
        }

        msg_data = (_AUDIO_CTRL_MSG *)msg[0];

        switch (msg_data->cmd) {
        case MSG_START_NET_AUDIO_PLAY: {
            cbuf_clear(&g_audio_hdl.pcm_cbuff_r);

            if (g_audio_hdl.is_audio_play_open) {
                _audio_player_stop();
            }

            g_audio_hdl.is_audio_play_open = TRUE;
            audio_player_net_init();
        }
        break;

        case MSG_STOP_NET_AUDIO_PLAY: {
            cbuf_clear(&g_audio_hdl.pcm_cbuff_r);
            g_audio_hdl.is_audio_play_open = FALSE;
            _audio_player_stop();
        }
        break;

        case MSG_START_AUDIO_RECORDER: {
            cbuf_clear(&g_audio_hdl.pcm_cbuff_w);

            if (g_audio_hdl.is_audio_record_open) {
                _audio_recoder_stop();
            }

            g_audio_hdl.is_audio_record_open = true;
            audio_recoder_init();
        }
        break;

        case MSG_STOP_AUDIO_RECORDER: {
            cbuf_clear(&g_audio_hdl.pcm_cbuff_w);
            g_audio_hdl.is_audio_record_open = FALSE;
            _audio_recoder_stop();
        }
        break;

        }

        if (msg_data) {
            if (msg_data->data) {
                free(msg_data->data);
            }

            free(msg_data);
            msg_data = NULL;
        }
    }
}

int audio_cfg_init(_AUDIO_PARAM *audio_param)
{
    audio_debug("into audio cfg init");
    int op_ret = -1;

    if (audio_param->sample_rate > 8000) {
        voice_buf_size = AUDIO_RECORD_VOICE_UPLORD_LEN;
    }

    audio_debug("audio param:%d %d %d %d", audio_param->sample_rate, audio_param->bit_dept, audio_param->channel_num, audio_param->audio_power_off);

    SAMPLE_RATE = audio_param->sample_rate;
    CHANNEL = audio_param->channel_num;
    BIT_DEP = audio_param->bit_dept;
    audio_power_off = audio_param->audio_power_off;
    return 0;
}

static int _audio_soft_init(void)
{
    int op_ret;
    u8 *pcm_buff_w = NULL;
    u8 *pcm_buff_r = NULL;
    audio_debug("into volc audio soft init");
    pcm_buff_w = malloc(SAMPLE_RATE * CHANNEL * 1);
    cbuf_init(&g_audio_hdl.pcm_cbuff_w, pcm_buff_w, SAMPLE_RATE * CHANNEL * 1);
#if defined CONFIG_ONESDK_LLM_ENABLE || defined  CONFIG_VOLC_LLM_ENABLE
    pcm_buff_r = malloc(SAMPLE_RATE * CHANNEL * 1);
    cbuf_init(&g_audio_hdl.pcm_cbuff_r, pcm_buff_r, SAMPLE_RATE * CHANNEL * 1);
#elif defined CONFIG_IFLY_AIUI_ENABLE
    pcm_buff_r = malloc(SAMPLE_RATE * CHANNEL * 20);
    cbuf_init(&g_audio_hdl.pcm_cbuff_r, pcm_buff_r, SAMPLE_RATE * CHANNEL * 20);
#elif defined CONFIG_TWETALK_ENABLE
    pcm_buff_r = malloc(SAMPLE_RATE * CHANNEL * 1);
    cbuf_init(&g_audio_hdl.pcm_cbuff_r, pcm_buff_r, SAMPLE_RATE * CHANNEL * 1);
#elif defined CONFIG_TUYA_AGENTIC_ENABLE
    /* 下行 TTS 播放缓冲:PCM 16k/16bit/mono=32KB/s。云端长答案(如金价)是突发下发(比播放快),
     * 缓冲不够大会丢尾→"说到一半没了"。开到 64×=1MB(32s)吸收长突发,SDRAM 8MB 充足。
     * 配合 tuya_agentic_demo.c 的"等缓冲排空再听"(超时 35s > 32s 排空),确保云端发完=喇叭播完
     * 才开始听,不会把残留 TTS 当新问题(治自说自话)或和新 TTS 混音(治刺啦噪音)。*/
    pcm_buff_r = malloc(SAMPLE_RATE * CHANNEL * 64);
    cbuf_init(&g_audio_hdl.pcm_cbuff_r, pcm_buff_r, SAMPLE_RATE * CHANNEL * 64);
#else
    pcm_buff_r = malloc(SAMPLE_RATE * CHANNEL * 4);
    cbuf_init(&g_audio_hdl.pcm_cbuff_r, pcm_buff_r, SAMPLE_RATE * CHANNEL * 4);
#endif

    if (!g_audio_hdl.enc_server) {
        g_audio_hdl.enc_server = server_open("audio_server", "enc");

        if (!g_audio_hdl.enc_server) {
            op_ret = -1;
            audio_debug("server_open err:%d", op_ret);
            return op_ret;
        }

        audio_debug("enc server_open succ!");
        server_register_event_handler_to_task(g_audio_hdl.enc_server, NULL, enc_server_event_handler, "app_core");
    }

    if (!g_audio_hdl.dec_server) {
        g_audio_hdl.dec_server = server_open("audio_server", "dec");

        if (!g_audio_hdl.dec_server) {
            op_ret = -1;
            audio_debug("server_open err:%d", op_ret);
            return op_ret;
        }

        audio_debug("dec server_open succ!");
        audio_debug("enc_server 0x%x dec_server 0x%x", g_audio_hdl.enc_server, g_audio_hdl.dec_server);
    }

    op_ret = os_sem_create(&g_audio_ctrl.r_sem, 0);

    if (op_ret != 0) {
        audio_debug("_hal_semaphore_create_init create semphore err:%d", op_ret);
        return op_ret;
    }

    QS queue_size = (sizeof(_AUDIO_CTRL_MSG *) * 20 + sizeof(WORD) - 1) / sizeof(WORD);
    op_ret = os_q_create(&g_audio_ctrl.msg_que, queue_size);

    thread_fork(_AUDIO_TASK_NAME, 5, 1 * 1024, 0, g_audio_ctrl.task_handle, __audio_task, NULL);

    audio_debug("_audio_task create");
    return op_ret;
}

void _audio_init(_AUDIO_PARAM *audio_param)
{
    audio_debug("into volc audio inti!");
    static char init = 0;

    audio_cfg_init(audio_param);

    if (!init) {
        _audio_soft_init();
    }

    init = 1;
}

void audio_stream_init(int sample_rate, int bit_dept, int channel_num)
{
    _AUDIO_PARAM audio_param;
    audio_param.audio_power_off = false;
    audio_param.bit_dept = bit_dept;
    audio_param.channel_num = channel_num;
    audio_param.sample_rate = sample_rate;
    _audio_init(&audio_param);
}

void start_audio_stream(void)
{
    _device_net_audio(true);
}

void stop_audio_stream(void)
{
    _device_net_audio(false);
}

int get_recoder_state()
{
    return recoder_state;
}

