/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qemu/sockets.h"	// struct in_addr needed for libslirp.h
#include "sysemu/qtest.h"
#include "slirp/libslirp.h"
#include "qemu/main-loop.h"
#include "block/aio.h"

#ifndef _WIN32

#include "qemu/compatfd.h"

/* If we have signalfd, we mask out the signals we want to handle and then
 * use signalfd to listen for them.  We rely on whatever the current signal
 * handler is to dispatch the signals when we receive them.
 */
static void sigfd_handler(void *opaque)
{
    int fd = (intptr_t)opaque;
    struct qemu_signalfd_siginfo info;
    struct sigaction action;
    ssize_t len;

    while (1) {
        do {
            len = read(fd, &info, sizeof(info));
        } while (len == -1 && errno == EINTR);

        if (len == -1 && errno == EAGAIN) {
            break;
        }

        if (len != sizeof(info)) {
            printf("read from sigfd returned %zd: %m\n", len);
            return;
        }

        sigaction(info.ssi_signo, NULL, &action);
        if ((action.sa_flags & SA_SIGINFO) && action.sa_sigaction) {
            action.sa_sigaction(info.ssi_signo,
                                (siginfo_t *)&info, NULL);
        } else if (action.sa_handler) {
            action.sa_handler(info.ssi_signo);
        }
    }
}

static int qemu_signal_init(void)
{
    int sigfd;
    sigset_t set;

    /*
     * SIG_IPI must be blocked in the main thread and must not be caught
     * by sigwait() in the signal thread. Otherwise, the cpu thread will
     * not catch it reliably.
     */
    sigemptyset(&set);
    sigaddset(&set, SIG_IPI);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGBUS);
    /* SIGINT cannot be handled via signalfd, so that ^C can be used
     * to interrupt QEMU when it is being run under gdb.  SIGHUP and
     * SIGTERM are also handled asynchronously, even though it is not
     * strictly necessary, because they use the same handler as SIGINT.
     */
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    sigdelset(&set, SIG_IPI);
    sigfd = qemu_signalfd(&set);
    if (sigfd == -1) {
        fprintf(stderr, "failed to create signalfd\n");
        return -errno;
    }

    fcntl_setfl(sigfd, O_NONBLOCK);

    qemu_set_fd_handler(sigfd, sigfd_handler, NULL, (void *)(intptr_t)sigfd);

    return 0;
}

#else /* _WIN32 */

static int qemu_signal_init(void)
{
    return 0;
}
#endif

static AioContext *qemu_aio_context;
static QEMUBH *qemu_notify_bh;

static void notify_event_cb(void *opaque)
{
    /* No need to do anything; this bottom half is only used to
     * kick the kernel out of ppoll/poll/WaitForMultipleObjects.
     */
}

AioContext *qemu_get_aio_context(void)
{
    return qemu_aio_context;
}

void qemu_notify_event(void)
{
    if (!qemu_aio_context) {
        return;
    }
    qemu_bh_schedule(qemu_notify_bh);
}

// 全局变量，需要监听的fd数组
// 底层实现相当于C++的vector可以动态增长
static GArray *gpollfds;

int qemu_init_main_loop(Error **errp)
{
    int ret;
    /*
    struct pollfd {
        // 文件描述符
        int fd;
        // 等待的事件
        short events;
        // 实际发生的事件
        short revents;
    }
    */

    // 保存pollfd结构体指针(被监听的文件描述符)。实际上以链表形式存在
    GSource *src;
    Error *local_error = NULL;
    // 依次创建rt_clock, vm_clock, host_lock三个始终
    init_clocks();

    ret = qemu_signal_init();
    if (ret) {
        return ret;
    }
    // 创建qemu定制的事件源qemu_aio_context
    qemu_aio_context = aio_context_new(&local_error);
    if (!qemu_aio_context) {
        error_propagate(errp, local_error);
        return -EMFILE;
    }
    qemu_notify_bh = qemu_bh_new(notify_event_cb, NULL);
    gpollfds = g_array_new(FALSE, FALSE, sizeof(GPollFD));
    // 从定制事件源中获取Glib原始的事件源
    src = aio_get_g_source(qemu_aio_context);
    // 设置事件源的名称aio-context
    g_source_set_name(src, "aio-context");
    // 将事件源添加到Glib默认事件循环上下文
    g_source_attach(src, NULL);
    g_source_unref(src);
    // 获取另外一个定制的事件源iohandler_ctx
    src = iohandler_get_g_source();
    // 设置事件源的名称
    g_source_set_name(src, "io-handler");
    // 将事件源添加到Glib默认事件循环上下文
    g_source_attach(src, NULL);
    g_source_unref(src);

    // 上面这些代码，qemu事件初始化通过两个事件源封装了QEMU所谓的上下文
    // qemu_aio_context和iohandler_ctx
    // 两个事件源被加到默认的事件循环中，default GMainContext
    // 这两个事件源同属于一个上下文，可以运行在同一个线程中
    return 0;
}

// 所有就绪事件中最高优先级等级，作为prepare函数的传入传出参数（执行该函数后，会获取最高优先级给该变量）
static int max_priority;

#ifndef _WIN32
static int glib_pollfds_idx;
static int glib_n_poll_fds;

// 获取所有需要进行监听的fd，并且计算一个最小的超时时间
// 执行了Glib库中的两个接口，为监听做准备并获取需要poll监听的fd
// g_main_context_prepare  g_main_context_query
static void glib_pollfds_fill(int64_t *cur_timeout)
{
    // GMainContext是Gsource的容器，Gsource可以添加到GMainContext里面
    // Gsource结构体保存了pollfd，包含文件fd和
    GMainContext *context = g_main_context_default();
    int timeout = 0;
    int64_t timeout_ns;
    int n;
    // 为主循环的监听做准备
    // 通过该函数会调用相应的prepare回调函数 时间准备好监听以后会返回true
    // prepare主要做三件事
    // 1. 查看是否有准备就绪的事件源，如果有返回true 如果没有返回false
    // 2. 找到最高优先级的准备就绪的事件源，把该优先级赋值给传出参数max_priority
    // 3. 设置GmainContext.timeout值，表示最近一次要到超时的事件源的时间
    // 当函数返回TRUE表示事件源已经准备好不需要poll的监听，进入下一步是否对事件源进行处理
    // 当函数返回FALSE表示事件源没 有准备好，需要poll进行监听等待事件源的相应，并设置一个超时时间
    g_main_context_prepare(context, &max_priority);

    glib_pollfds_idx = gpollfds->len;
    n = glib_n_poll_fds;
    // 进入循环 在循环中调用query
    do { 
        GPollFD *pfds;
        glib_n_poll_fds = n;
        // 参数gpollfds 是一个全局变量保存了需要监听的fd
        g_array_set_size(gpollfds, glib_pollfds_idx + glib_n_poll_fds);
        pfds = &g_array_index(gpollfds, GPollFD, glib_pollfds_idx);
        // 获取实际需要调用的poll 的文件fd
        // 循环获取
        n = g_main_context_query(context, max_priority, &timeout, pfds,
                                 glib_n_poll_fds);
    } while (n != glib_n_poll_fds);

    if (timeout < 0) {
        timeout_ns = -1;
    } else {
        timeout_ns = (int64_t)timeout * (int64_t)SCALE_MS;
    }

    *cur_timeout = qemu_soonest_timeout(timeout_ns, *cur_timeout);
}
// 调用 g_main_context_check g_main_context_dispatch
// 实现事件的是否相应，并将相应的事件进行分发
static void glib_pollfds_poll(void)
{
    GMainContext *context = g_main_context_default();
    GPollFD *pfds = &g_array_index(gpollfds, GPollFD, glib_pollfds_idx);
    // 执行完poll之后调用check对事件进行检查，如果事件相应了的就返回的TRUE
    if (g_main_context_check(context, max_priority, pfds, glib_n_poll_fds)) {
        // 事件相应则调用dispatch将事件进行分发（执行callback）
        g_main_context_dispatch(context);
    }
}

#define MAX_MAIN_LOOP_SPIN (1000)

// 对事件进行监听
// 三步：glib_pollfds_fill    qemu_poll_ns    glib_pollfds_fill 
static int os_host_main_loop_wait(int64_t timeout)
{
    int ret;
    static int spin_counter;
    // 第一步：准备工作。获取实际需要调用poll的fd
    // g_main_context_prepare  g_main_context_query
    glib_pollfds_fill(&timeout);

    /* If the I/O thread is very busy or we are incorrectly busy waiting in
     * the I/O thread, this can lead to starvation of the BQL such that the
     * VCPU threads never run.  To make sure we can detect the later case,
     * print a message to the screen.  If we run into this condition, create
     * a fake timeout in order to give the VCPU threads a chance to run.
     */
    if (!timeout && (spin_counter > MAX_MAIN_LOOP_SPIN)) {
        static bool notified;

        if (!notified && !qtest_enabled() && !qtest_driver()) {
            fprintf(stderr,
                    "main-loop: WARNING: I/O thread spun for %d iterations\n",
                    MAX_MAIN_LOOP_SPIN);
            notified = true;
        }

        timeout = SCALE_MS;
    }

    if (timeout) {
        spin_counter = 0;
        qemu_mutex_unlock_iothread();
    } else {
        spin_counter++;
    }
    // 第二步：
    // 会阻塞主线程，要么返回发生了事件，要么返回一个超时
    ret = qemu_poll_ns((GPollFD *)gpollfds->data, gpollfds->len, timeout);

    if (timeout) {
        qemu_mutex_lock_iothread();
    }
    // 第三步：进行事件的检查和分发处理
    glib_pollfds_poll();
    return ret;
}
#else
/***********************************************************/
/* Polling handling */

typedef struct PollingEntry {
    PollingFunc *func;
    void *opaque;
    struct PollingEntry *next;
} PollingEntry;

static PollingEntry *first_polling_entry;

int qemu_add_polling_cb(PollingFunc *func, void *opaque)
{
    PollingEntry **ppe, *pe;
    pe = g_malloc0(sizeof(PollingEntry));
    pe->func = func;
    pe->opaque = opaque;
    for(ppe = &first_polling_entry; *ppe != NULL; ppe = &(*ppe)->next);
    *ppe = pe;
    return 0;
}

void qemu_del_polling_cb(PollingFunc *func, void *opaque)
{
    PollingEntry **ppe, *pe;
    for(ppe = &first_polling_entry; *ppe != NULL; ppe = &(*ppe)->next) {
        pe = *ppe;
        if (pe->func == func && pe->opaque == opaque) {
            *ppe = pe->next;
            g_free(pe);
            break;
        }
    }
}

/***********************************************************/
/* Wait objects support */
typedef struct WaitObjects {
    int num;
    int revents[MAXIMUM_WAIT_OBJECTS + 1];
    HANDLE events[MAXIMUM_WAIT_OBJECTS + 1];
    WaitObjectFunc *func[MAXIMUM_WAIT_OBJECTS + 1];
    void *opaque[MAXIMUM_WAIT_OBJECTS + 1];
} WaitObjects;

static WaitObjects wait_objects = {0};

int qemu_add_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    WaitObjects *w = &wait_objects;
    if (w->num >= MAXIMUM_WAIT_OBJECTS) {
        return -1;
    }
    w->events[w->num] = handle;
    w->func[w->num] = func;
    w->opaque[w->num] = opaque;
    w->revents[w->num] = 0;
    w->num++;
    return 0;
}

void qemu_del_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    int i, found;
    WaitObjects *w = &wait_objects;

    found = 0;
    for (i = 0; i < w->num; i++) {
        if (w->events[i] == handle) {
            found = 1;
        }
        if (found) {
            w->events[i] = w->events[i + 1];
            w->func[i] = w->func[i + 1];
            w->opaque[i] = w->opaque[i + 1];
            w->revents[i] = w->revents[i + 1];
        }
    }
    if (found) {
        w->num--;
    }
}

void qemu_fd_register(int fd)
{
    WSAEventSelect(fd, event_notifier_get_handle(&qemu_aio_context->notifier),
                   FD_READ | FD_ACCEPT | FD_CLOSE |
                   FD_CONNECT | FD_WRITE | FD_OOB);
}

static int pollfds_fill(GArray *pollfds, fd_set *rfds, fd_set *wfds,
                        fd_set *xfds)
{
    int nfds = -1;
    int i;

    for (i = 0; i < pollfds->len; i++) {
        GPollFD *pfd = &g_array_index(pollfds, GPollFD, i);
        int fd = pfd->fd;
        int events = pfd->events;
        if (events & G_IO_IN) {
            FD_SET(fd, rfds);
            nfds = MAX(nfds, fd);
        }
        if (events & G_IO_OUT) {
            FD_SET(fd, wfds);
            nfds = MAX(nfds, fd);
        }
        if (events & G_IO_PRI) {
            FD_SET(fd, xfds);
            nfds = MAX(nfds, fd);
        }
    }
    return nfds;
}

static void pollfds_poll(GArray *pollfds, int nfds, fd_set *rfds,
                         fd_set *wfds, fd_set *xfds)
{
    int i;

    for (i = 0; i < pollfds->len; i++) {
        GPollFD *pfd = &g_array_index(pollfds, GPollFD, i);
        int fd = pfd->fd;
        int revents = 0;

        if (FD_ISSET(fd, rfds)) {
            revents |= G_IO_IN;
        }
        if (FD_ISSET(fd, wfds)) {
            revents |= G_IO_OUT;
        }
        if (FD_ISSET(fd, xfds)) {
            revents |= G_IO_PRI;
        }
        pfd->revents = revents & pfd->events;
    }
}

static int os_host_main_loop_wait(int64_t timeout)
{
    GMainContext *context = g_main_context_default();
    GPollFD poll_fds[1024 * 2]; /* this is probably overkill */
    int select_ret = 0;
    int g_poll_ret, ret, i, n_poll_fds;
    PollingEntry *pe;
    WaitObjects *w = &wait_objects;
    gint poll_timeout;
    int64_t poll_timeout_ns;
    static struct timeval tv0;
    fd_set rfds, wfds, xfds;
    int nfds;

    /* XXX: need to suppress polling by better using win32 events */
    ret = 0;
    for (pe = first_polling_entry; pe != NULL; pe = pe->next) {
        ret |= pe->func(pe->opaque);
    }
    if (ret != 0) {
        return ret;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&xfds);
    nfds = pollfds_fill(gpollfds, &rfds, &wfds, &xfds);
    if (nfds >= 0) {
        select_ret = select(nfds + 1, &rfds, &wfds, &xfds, &tv0);
        if (select_ret != 0) {
            timeout = 0;
        }
        if (select_ret > 0) {
            pollfds_poll(gpollfds, nfds, &rfds, &wfds, &xfds);
        }
    }

    g_main_context_prepare(context, &max_priority);
    n_poll_fds = g_main_context_query(context, max_priority, &poll_timeout,
                                      poll_fds, ARRAY_SIZE(poll_fds));
    g_assert(n_poll_fds <= ARRAY_SIZE(poll_fds));

    for (i = 0; i < w->num; i++) {
        poll_fds[n_poll_fds + i].fd = (DWORD_PTR)w->events[i];
        poll_fds[n_poll_fds + i].events = G_IO_IN;
    }

    if (poll_timeout < 0) {
        poll_timeout_ns = -1;
    } else {
        poll_timeout_ns = (int64_t)poll_timeout * (int64_t)SCALE_MS;
    }

    poll_timeout_ns = qemu_soonest_timeout(poll_timeout_ns, timeout);

    qemu_mutex_unlock_iothread();
    g_poll_ret = qemu_poll_ns(poll_fds, n_poll_fds + w->num, poll_timeout_ns);

    qemu_mutex_lock_iothread();
    if (g_poll_ret > 0) {
        for (i = 0; i < w->num; i++) {
            w->revents[i] = poll_fds[n_poll_fds + i].revents;
        }
        for (i = 0; i < w->num; i++) {
            if (w->revents[i] && w->func[i]) {
                w->func[i](w->opaque[i]);
            }
        }
    }

    if (g_main_context_check(context, max_priority, poll_fds, n_poll_fds)) {
        g_main_context_dispatch(context);
    }

    return select_ret || g_poll_ret;
}
#endif

int main_loop_wait(int nonblocking)
{
    int ret;
    uint32_t timeout = UINT32_MAX;
    int64_t timeout_ns;

    if (nonblocking) {
        timeout = 0;
    }

    /* poll any events */
    g_array_set_size(gpollfds, 0); /* reset for new iteration */
    /* XXX: separate device handlers from system ones */
#ifdef CONFIG_SLIRP
    slirp_pollfds_fill(gpollfds, &timeout);
#endif

    if (timeout == UINT32_MAX) {
        timeout_ns = -1;
    } else {
        timeout_ns = (uint64_t)timeout * (int64_t)(SCALE_MS);
    }
    // 计算最小的time_out值，可以使QEMU及时处理定时器到期事件
    timeout_ns = qemu_soonest_timeout(timeout_ns,
                                      timerlistgroup_deadline_ns(
                                          &main_loop_tlg));
    // os_host_main_loop_wait函数主要涉及下面三个函数  
    // glib_pollfds_fill    qemu_poll_ns    glib_pollfds_fill 
    ret = os_host_main_loop_wait(timeout_ns);
#ifdef CONFIG_SLIRP
    slirp_pollfds_poll(gpollfds, (ret < 0));
#endif

    /* CPU thread can infinitely wait for event after
       missing the warp */
    qemu_start_warp_timer();
    qemu_clock_run_all_timers();

    return ret;
}

/* Functions to operate on the main QEMU AioContext.  */

QEMUBH *qemu_bh_new(QEMUBHFunc *cb, void *opaque)
{
    return aio_bh_new(qemu_aio_context, cb, opaque);
}
