/*
 * stm_port_ac79_shim.c — libstm_tuya.a(涂鸦 STM OPEN SDK)在 AC79/WL82 上的补齐层。
 *
 * libstm.a 是涂鸦用杰理工具链编出来的 LTO bitcode 归档,target=pi32v2、clang 4.0.1,
 * mbedtls_ssl_context / pthread_mutex_internal / pthread_cond_internal / xSTATIC_QUEUE
 * 的结构布局与本 SDK 宿主库逐字节一致(已反汇编核对),140 个外部符号全部由本 SDK
 * 现有库/源码满足。本文件配合 patch_libstm.sh 做两件事:
 *   ① 引擎线程栈 1KB → 12KB;② O_NONBLOCK 常量翻译(libstm 按 Linux 头编译的
 *   0x4000,本平台 lwip 是 1,不翻译 UDP/TCP 建连必挂,详见 patch 脚本注释)。
 *
 * 日志不在这里桥接:stm_engine_init 自己会注册内部 handler,并转发到
 * stm_open_init(config) 里传的 config.on_log,用那个即可(见 tuya_stm_ai.c)。
 */
#include "app_config.h"

#if defined(CONFIG_TUYA_AGENTIC_ENABLE) && defined(TUYA_TRANSPORT_STM_ENABLE) && TUYA_TRANSPORT_STM_ENABLE

#include "os/pthread.h"
#include "lwip/sockets.h"    /* lwip_fcntl 原型 + 本平台 O_NONBLOCK(=1) */

/* ------------------------------------------------------------------------- */
/* ① 引擎线程栈                                                              */
/* ------------------------------------------------------------------------- */
/* 12KB:引擎线程要同时承载 stm_net_loop 轮询和 DTLS/TLS 握手。mbedtls 的
 * ssl ctx 单个就 1916(TLS)/1964(DTLS)字节,握手期间还有证书链解析的临时量。
 * 参照物:pal_ac791n.c 的 tuya_pal 线程 6KB(只跑 TLS,不跑 net loop)。
 * 内存紧张可降到 8KB,但不要更低 —— 握手爆栈表现为随机 hardfault,
 * 不会给出"stack overflow"这类明确线索,极难定位。*/
#define STM_ENGINE_STACK_BYTES  (12 * 1024)

int stm_ac79_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                            void *(*startroutine)(void *), void *arg)
{
    pthread_attr_t a;

    if (attr) {
        /* 库当前只用 attr=NULL 调;真给了 attr 就尊重调用方,不擅自改栈 */
        return pthread_create(thread, attr, startroutine, arg);
    }

    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, STM_ENGINE_STACK_BYTES);
    return pthread_create(thread, &a, startroutine, arg);
}

/* ------------------------------------------------------------------------- */
/* ② O_NONBLOCK 常量翻译(见 patch_libstm.sh ②)                              */
/* ------------------------------------------------------------------------- */
#define TSTM_LIB_O_NONBLOCK 0x4000   /* libstm 按 Linux 头编译的值 */

int stm_ac79_fcntl(int fd, int cmd, int val)
{
    if (cmd == 3) {                  /* F_GETFL,两侧命令号相同 */
        int f = lwip_fcntl(fd, cmd, 0);
        return (f == -1) ? -1 : ((f & O_NONBLOCK) ? TSTM_LIB_O_NONBLOCK : 0);
    }
    if (cmd == 4) {                  /* F_SETFL */
        return lwip_fcntl(fd, cmd,
                          (val & TSTM_LIB_O_NONBLOCK) ? O_NONBLOCK : 0);
    }
    return lwip_fcntl(fd, cmd, val); /* 其余命令原样转发 */
}

/* fflush / fputs / fprintf 不补桩:apps/common/c++/cxx_runtime.cpp 已提供
 * (printf 提示 + 返回 0),libstm 对三者的唯一引用在 stm_log.c 的 FILE*
 * stdout 分支,而 stdout_enabled 初值 0,是死代码路径,够用。*/

#endif /* CONFIG_TUYA_AGENTIC_ENABLE && TUYA_TRANSPORT_STM_ENABLE */
