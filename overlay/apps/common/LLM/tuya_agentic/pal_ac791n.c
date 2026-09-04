/*
 * pal_ac791n.c -- AgenticKit PAL for JieLi AC791N (FreeRTOS + lwIP + mbedTLS).
 *
 * 把 agentic-kit 的 pal/pal.h 契约(14 个回调)桥接到 AC79 宿主 API。
 * TCP 部分基本照搬 pal/pal_freertos.c(同为 lwIP);mutex 用 FreeRTOS 递归信号量
 * (configUSE_RECURSIVE_MUTEXES=1,见 include_lib/system/os/FreeRTOS/FreeRTOSConfig.h);
 * thread 用 thread_fork + thread_kill(KILL_WAIT);time 用 sys_timer_get_ms()。
 *
 * 递归锁是硬要求:iot-client 的 DP schema 更新路径会重入锁(pal.h 注释、iot_dp.c),
 * 普通互斥锁会死锁。
 */
#include "pal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* AC79 宿主头 */
#include "system/timer.h"          /* sys_timer_get_ms            */
#include "system/os/os_api.h"      /* thread_fork / thread_kill   */
#include "FreeRTOS/FreeRTOS.h"     /* 递归信号量(与 qcloud HAL_OS_free_rtos.c 同款 include)*/
#include "FreeRTOS/semphr.h"
#include "lwip/sockets.h"          /* socket/connect/select/...   */
#include "lwip/netdb.h"            /* getaddrinfo                 */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif

/* AC79 newlib 缺 flockfile/funlockfile(agentic-kit common/log.c 的
 * log_default_handler 用到),提供空实现。日志线程安全在这里不强求。*/
void flockfile(FILE *fp)   { (void)fp; }
void funlockfile(FILE *fp) { (void)fp; }

/* ------------------------------------------------------------------------- */
/* TCP socket(lwIP,与 pal_freertos.c 一致)                                 */
/* ------------------------------------------------------------------------- */
typedef struct {
    int fd;
} ac_tcp_t;

static void *ac_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return NULL;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return NULL; }

    /* 非阻塞 connect,用 select 限时 */
    int fl = lwip_fcntl(fd, F_GETFL, 0);
    lwip_fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd); freeaddrinfo(res); return NULL;
    }
    if (rc != 0) {
        if (timeout_ms == 0) { close(fd); freeaddrinfo(res); return NULL; }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel;
        do {
            sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        } while (sel < 0 && errno == EINTR);
        if (sel <= 0) { close(fd); freeaddrinfo(res); return NULL; }

        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
            close(fd); freeaddrinfo(res); return NULL;
        }
    }
    /* 回到阻塞模式,靠 SO_SNDTIMEO/SO_RCVTIMEO 控超时 */
    lwip_fcntl(fd, F_SETFL, fl);
    freeaddrinfo(res);

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));   /* 关 Nagle,低延迟 */

    ac_tcp_t *h = (ac_tcp_t *)malloc(sizeof(ac_tcp_t));
    if (!h) { close(fd); return NULL; }
    h->fd = fd;
    return h;
}

static int ac_tcp_send(void *handle, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    ac_tcp_t *h = (ac_tcp_t *)handle;
    int flags = MSG_NOSIGNAL;
    if (timeout_ms == 0) {
        flags |= MSG_DONTWAIT;
    } else {
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(h->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    /* 海外数据中心慢链路诊断:测 send() 系统调用本身耗时(不含上层 TLS/锁开销)。
     * 正常 <5ms;若普遍 >50ms 说明是 TCP/网络层慢(拥塞/RTT 大/VPN),非 SDK 问题。*/
    unsigned int _t0 = sys_timer_get_ms();
    int n;
    do { n = send(h->fd, buf, len, flags); } while (n < 0 && errno == EINTR);
    unsigned int _dt = sys_timer_get_ms() - _t0;
    if (_dt >= 50) {
        printf("[NET-SND] tcp send=%ums len=%u to=%u n=%d errno=%d\r\n",
               _dt, (unsigned)len, timeout_ms, n, errno);
    }
    if (n > 0) return n;
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return PAL_ERR_AGAIN;
    return PAL_ERR_NET;
}

static int ac_tcp_recv(void *handle, uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    ac_tcp_t *h = (ac_tcp_t *)handle;
    int flags = 0;
    if (timeout_ms == 0) {
        flags |= MSG_DONTWAIT;
    } else {
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(h->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    int n;
    do { n = recv(h->fd, buf, buf_len, flags); } while (n < 0 && errno == EINTR);
    if (n > 0) return n;
    if (n == 0) return 0;                       /* EOF */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return PAL_ERR_AGAIN;
    return PAL_ERR_NET;
}

static int ac_tcp_poll(void *handle, int events, uint32_t timeout_ms)
{
    ac_tcp_t *h = (ac_tcp_t *)handle;
    fd_set rfds, wfds;
    FD_ZERO(&rfds); FD_ZERO(&wfds);
    if (events & 1) FD_SET(h->fd, &rfds);
    if (events & 2) FD_SET(h->fd, &wfds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int sel = select(h->fd + 1,
                     (events & 1) ? &rfds : NULL,
                     (events & 2) ? &wfds : NULL,
                     NULL, &tv);
    if (sel < 0) return -3;
    if (sel == 0) return 0;
    int result = 0;
    if ((events & 1) && FD_ISSET(h->fd, &rfds)) result |= 1;
    if ((events & 2) && FD_ISSET(h->fd, &wfds)) result |= 2;
    return result;
}

static void ac_tcp_close(void *handle)
{
    if (!handle) return;
    ac_tcp_t *h = (ac_tcp_t *)handle;
    close(h->fd);
    free(h);
}

/* ------------------------------------------------------------------------- */
/* 时间 / 内存                                                               */
/* ------------------------------------------------------------------------- */
static uint64_t ac_time_ms(void)  { return (uint64_t)sys_timer_get_ms(); }
static void    *ac_malloc(size_t n){ return malloc(n); }
static void     ac_free(void *p)   { free(p); }

/* ------------------------------------------------------------------------- */
/* 互斥锁 —— 必须递归(FreeRTOS 递归信号量)                                  */
/* ------------------------------------------------------------------------- */
static void *ac_mutex_create(void)
{
    return (void *)xSemaphoreCreateRecursiveMutex();
}
static void ac_mutex_lock(void *m)
{
    xSemaphoreTakeRecursive((SemaphoreHandle_t)m, portMAX_DELAY);
}
static void ac_mutex_unlock(void *m)
{
    xSemaphoreGiveRecursive((SemaphoreHandle_t)m);
}
static void ac_mutex_destroy(void *m)
{
    vSemaphoreDelete((SemaphoreHandle_t)m);
}

/* ------------------------------------------------------------------------- */
/* 线程 —— thread_fork(void fn(void*)) + trampoline 桥接 PAL 的 void* fn(void*),
 * join 用 thread_kill(KILL_WAIT)。                                          */
/* ------------------------------------------------------------------------- */
typedef struct {
    void *(*fn)(void *);
    void  *arg;
} ac_thr_arg;

typedef struct {
    volatile int pid;
    ac_thr_arg   a;
} ac_thr_handle_t;

static void ac_thr_entry(void *p)
{
    ac_thr_handle_t *th = (ac_thr_handle_t *)p;
    th->a.fn(th->a.arg);
    /* 入口返回后,thread_fork 创建的任务会自动结束 */
}

static int ac_thread_create(void **handle, void *(*func)(void *), void *arg)
{
    ac_thr_handle_t *th = (ac_thr_handle_t *)malloc(sizeof(ac_thr_handle_t));
    if (!th) return -1;
    th->a.fn  = func;
    th->a.arg = arg;
    th->pid   = 0;

    /* stk_size 单位是 4 字节;6*1024 = 24KB(跑 mbedTLS 握手,不够再加)*/
    int rc = thread_fork("tuya_pal", 4, 6 * 1024, 0, (int *)&th->pid, ac_thr_entry, th);
    if (rc != 0) {
        free(th);
        return -1;
    }
    *handle = th;
    return 0;
}

static int ac_thread_join(void *handle)
{
    if (!handle) return -1;
    ac_thr_handle_t *th = (ac_thr_handle_t *)handle;
    thread_kill(&th->pid, KILL_WAIT);
    free(th);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 导出                                                                      */
/* ------------------------------------------------------------------------- */
static const pal_t g_ac791n_pal = {
    .tcp_connect  = ac_tcp_connect,
    .tcp_send     = ac_tcp_send,
    .tcp_recv     = ac_tcp_recv,
    .tcp_close    = ac_tcp_close,
    .tcp_poll     = ac_tcp_poll,
    .time_ms      = ac_time_ms,
    .malloc       = ac_malloc,
    .free         = ac_free,
    .mutex_create = ac_mutex_create,
    .mutex_lock   = ac_mutex_lock,
    .mutex_unlock = ac_mutex_unlock,
    .mutex_destroy= ac_mutex_destroy,
    .thread_create= ac_thread_create,
    .thread_join  = ac_thread_join,
};

const pal_t *tai_pal_ac791n(void)
{
    return &g_ac791n_pal;
}
