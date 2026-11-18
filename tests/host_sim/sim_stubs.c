/*
 * Host stand-ins for the Vita APIs used by Battleship, so the real
 * main.c / render.c / audio.c / adhoc_net.c can run on Linux.
 *
 *  - Time runs SPEED x faster than wall clock (keeps tests short).
 *  - vita2d calls are no-ops (layout is not checked here).
 *  - Ad-Hoc matching is emulated over UNIX datagram sockets, PTP over a
 *    UNIX stream socket, so two processes (host + join) can play a real
 *    networked match through the game's own protocol code.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/audioout.h>
#include <psp2/apputil.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/adhoc_matching.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <vita2d.h>

#define SPEED 10

/* ------------------------------------------------------------- time */
uint64_t sim_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL) * SPEED;
}
static void vsleep(unsigned us) { usleep(us / SPEED + 1); }

SceUInt64 sceKernelGetProcessTimeWide(void) { return sim_now_us(); }
int sceKernelDelayThread(SceUInt delay) { vsleep(delay); return 0; }
int sceKernelExitProcess(int res) { (void)res; return 0; }

/* ----------------------------------------------------------- threads */
typedef struct { SceKernelThreadEntry fn; pthread_t th; SceSize argl; void *argp; } SimThread;
static SimThread g_threads[16];
static int g_nthreads;

static void *thread_tramp(void *p) { SimThread *t = (SimThread *)p; t->fn(t->argl, t->argp); return NULL; }

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority,
                             SceSize stackSize, SceUInt attr, int cpuAffinityMask, const SceKernelThreadOptParam *option)
{
    (void)name; (void)initPriority; (void)stackSize; (void)attr; (void)cpuAffinityMask; (void)option;
    if (g_nthreads >= 16) return -1;
    g_threads[g_nthreads].fn = entry;
    return g_nthreads++;
}
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp)
{
    g_threads[thid].argl = arglen; g_threads[thid].argp = argp;
    return pthread_create(&g_threads[thid].th, NULL, thread_tramp, &g_threads[thid]);
}
int sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout)
{
    (void)stat; (void)timeout;
    return pthread_join(g_threads[thid].th, NULL);
}
int sceKernelDeleteThread(SceUID thid) { (void)thid; return 0; }

/* ------------------------------------------------------------- input */
void (*sim_input_hook)(SceCtrlData *pad, int *touch, int *tx, int *ty);
static int g_touch, g_tx, g_ty;

int sceCtrlSetSamplingMode(SceCtrlPadInputMode mode) { (void)mode; return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count)
{
    (void)port; (void)count;
    memset(pad, 0, sizeof(*pad));
    pad->lx = pad->ly = 128;
    g_touch = 0;
    if (sim_input_hook) sim_input_hook(pad, &g_touch, &g_tx, &g_ty);
    return 1;
}
int sceTouchSetSamplingState(SceUInt32 port, SceTouchSamplingState state) { (void)port; (void)state; return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData *d, SceUInt32 n)
{
    (void)port; (void)n;
    memset(d, 0, sizeof(*d));
    if (g_touch) { d->reportNum = 1; d->report[0].x = (SceInt16)(g_tx * 2); d->report[0].y = (SceInt16)(g_ty * 2); }
    return 1;
}

/* ------------------------------------------------------------- audio */
int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode)
{ (void)type; (void)len; (void)freq; (void)mode; return 1; }
int sceAudioOutReleasePort(int port) { (void)port; return 0; }
int sceAudioOutOutput(int port, const void *buf) { (void)port; (void)buf; vsleep(5333); return 0; }
int sceAudioOutSetVolume(int port, SceAudioOutChannelFlag ch, int *vol) { (void)port; (void)ch; (void)vol; return 0; }

/* ------------------------------------------------------------ system */
int sceAppUtilInit(SceAppUtilInitParam *i, SceAppUtilBootParam *b) { (void)i; (void)b; return 0; }
int sceAppUtilSystemParamGetString(unsigned int id, SceChar8 *buf, SceSize size)
{
    const char *r = getenv("BS_ROLE");
    (void)id;
    snprintf((char *)buf, size, "%s", r && !strcmp(r, "join") ? "Joiner" : "Hoster");
    return 0;
}
int scePowerSetArmClockFrequency(int f) { (void)f; return 0; }
int scePowerSetGpuClockFrequency(int f) { (void)f; return 0; }
int sceSysmoduleLoadModule(SceSysmoduleModuleId id) { (void)id; return 0; }
int sceSysmoduleUnloadModule(SceSysmoduleModuleId id) { (void)id; return 0; }

/* ------------------------------------------------------------ vita2d */
static unsigned char g_pool[1 << 20];
static size_t g_pool_off;
static int g_frames;
int vita2d_init(void) { return 0; }
int vita2d_fini(void) { return 0; }
void vita2d_set_clear_color(unsigned int c) { (void)c; }
void vita2d_set_vblank_wait(int e) { (void)e; }
void vita2d_start_drawing(void) { g_pool_off = 0; }
void vita2d_end_drawing(void) {}
void vita2d_clear_screen(void) {}
void vita2d_swap_buffers(void) { g_frames++; vsleep(16667); }
void vita2d_wait_rendering_done(void) {}
void *vita2d_pool_memalign(unsigned int size, unsigned int align)
{
    size_t o = (g_pool_off + align - 1) / align * align;
    if (o + size > sizeof(g_pool)) { fprintf(stderr, "vita2d pool exhausted in one frame!\n"); exit(4); }
    g_pool_off = o + size;
    return g_pool + o;
}
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void vita2d_draw_line(float x0, float y0, float x1, float y1, unsigned int c) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }
void vita2d_draw_fill_circle(float x, float y, float r, unsigned int c) { (void)x; (void)y; (void)r; (void)c; }
void vita2d_draw_array(SceGxmPrimitiveType m, const vita2d_color_vertex *v, size_t n) { (void)m; (void)v; (void)n; }
void vita2d_enable_clipping(void) {}
void vita2d_disable_clipping(void) {}
void vita2d_set_clip_rectangle(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
static vita2d_pgf *g_fake_font = (vita2d_pgf *)1;
vita2d_pgf *vita2d_load_default_pgf(void) { return g_fake_font; }
void vita2d_free_pgf(vita2d_pgf *f) { (void)f; }
int vita2d_pgf_draw_text(vita2d_pgf *f, int x, int y, unsigned int c, float s, const char *t) { (void)f; (void)x; (void)y; (void)c; (void)s; return (int)strlen(t); }
int vita2d_pgf_text_width(vita2d_pgf *f, float s, const char *t) { (void)f; return (int)(strlen(t) * 10 * s); }

/* ======================================================= net emulation */
static const char *role(void) { const char *r = getenv("BS_ROLE"); return r ? r : "single"; }
static int is_host(void) { return !strcmp(role(), "host"); }

int sceNetInit(SceNetInitParam *p) { (void)p; return 0; }
int sceNetTerm(void) { return 0; }
int sceNetCtlInit(void) { return 0; }
void sceNetCtlTerm(void) {}
int sceNetAdhocInit(void) { return 0; }
int sceNetAdhocTerm(void) { return 0; }
int sceNetAdhocctlInit(const SceNetAdhocctlAdhocId *id)
{
    if (memcmp(id->data, "BTLSHP001", 9) != 0) return -1;
    return 0;
}
int sceNetAdhocctlTerm(void) { return 0; }
int sceNetAdhocctlGetEtherAddr(SceNetEtherAddr *a)
{
    memset(a, 0, sizeof(*a));
    a->data[5] = is_host() ? 0x01 : 0x02;
    return 0;
}

/* ---- matching over UNIX datagrams ---- */
#define SOCK_PARENT "/tmp/bssim_parent.sock"
#define SOCK_CHILD  "/tmp/bssim_child.sock"
#define SOCK_PTP    "/tmp/bssim_ptp.sock"
enum { M_HELLO = 1, M_REQ, M_ACC, M_BYE };
typedef struct { int type; int opt_len; unsigned char opt[64]; } MMsg;

static int m_fd = -1, m_mode, m_run, m_established, m_pending_est, m_opt_len;
static unsigned char m_opt[64];
static SceNetAdhocMatchingCallback m_cb;
static pthread_t m_th;

static void m_send(const char *to, int type, const void *opt, int len)
{
    struct sockaddr_un a; MMsg m;
    memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX; strcpy(a.sun_path, to);
    memset(&m, 0, sizeof(m)); m.type = type; m.opt_len = len;
    if (opt && len) memcpy(m.opt, opt, (size_t)len);
    sendto(m_fd, &m, sizeof(m), 0, (struct sockaddr *)&a, sizeof(a));
}

#define DBG(...) do { if (getenv("BS_DEBUG")) { fprintf(stderr, "[%s %8.3f] ", role(), sim_now_us() / 1e6); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static void *m_thread(void *p)
{
    SceNetInAddr peer;
    (void)p;
    memset(&peer, 0, sizeof(peer));
    peer.s_addr = m_mode == SCE_NET_ADHOC_MATCHING_MODE_PARENT ? 2 : 1;
    while (m_run) {
        struct pollfd pf = { m_fd, POLLIN, 0 };
        if (m_pending_est) { m_pending_est = 0; m_established = 1; m_cb(0, 7, &peer, 0, NULL); }
        if (m_mode == SCE_NET_ADHOC_MATCHING_MODE_PARENT && !m_established)
            m_send(SOCK_CHILD, M_HELLO, m_opt, m_opt_len);
        if (poll(&pf, 1, 20) > 0) {
            MMsg m;
            if (recv(m_fd, &m, sizeof(m), 0) == (ssize_t)sizeof(m)) {
                if (m.type != M_HELLO) DBG("matching rx type %d (established=%d)", m.type, m_established);
                switch (m.type) {
                case M_HELLO: if (!m_established) m_cb(0, 1, &peer, (SceSize)m.opt_len, m.opt); break;
                case M_REQ:   m_cb(0, 2, &peer, (SceSize)m.opt_len, m.opt); break;
                case M_ACC:   m_established = 1; m_cb(0, 7, &peer, 0, NULL); break;
                case M_BYE:   m_cb(0, 10, &peer, 0, NULL); break;
                }
            }
        }
    }
    return NULL;
}

int sceNetAdhocMatchingInit(unsigned int s, void *p) { (void)s; (void)p; return 0; }
int sceNetAdhocMatchingTerm(void) { return 0; }
int sceNetAdhocMatchingCreate(SceNetAdhocMatchingMode mode, int max, SceUShort16 port, int rx, unsigned int hello,
                              unsigned int ka, int retry, unsigned int rexmt, SceNetAdhocMatchingCallback cb)
{
    struct sockaddr_un a;
    const char *me = mode == SCE_NET_ADHOC_MATCHING_MODE_PARENT ? SOCK_PARENT : SOCK_CHILD;
    (void)max; (void)port; (void)rx; (void)hello; (void)ka; (void)retry; (void)rexmt;
    m_mode = mode; m_cb = cb; m_established = 0; m_pending_est = 0;
    m_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX; strcpy(a.sun_path, me);
    unlink(me);
    if (bind(m_fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return -1; }
    return 1;
}
int sceNetAdhocMatchingStart(int id, int prio, int stack, int aff, int opt_len, void *opt)
{
    (void)id; (void)prio; (void)stack; (void)aff;
    m_opt_len = opt_len;
    if (opt && opt_len) memcpy(m_opt, opt, (size_t)opt_len);
    m_run = 1;
    pthread_create(&m_th, NULL, m_thread, NULL);
    return 0;
}
int sceNetAdhocMatchingSelectTarget(int id, SceNetInAddr *t, int opt_len, void *opt)
{
    DBG("select target (mode %d)", m_mode);
    (void)id; (void)t;
    if (m_mode == SCE_NET_ADHOC_MATCHING_MODE_PARENT) {
        m_send(SOCK_CHILD, M_ACC, NULL, 0);
        m_pending_est = 1;                          /* delivered on matching thread */
    } else {
        m_send(SOCK_PARENT, M_REQ, opt, opt_len);
    }
    return 0;
}
int sceNetAdhocMatchingCancelTarget(int id, SceNetInAddr *t) { (void)id; (void)t; return 0; }
int sceNetAdhocMatchingStop(int id)
{
    DBG("matching stop");
    (void)id;
    if (m_run) {
        m_send(m_mode == SCE_NET_ADHOC_MATCHING_MODE_PARENT ? SOCK_CHILD : SOCK_PARENT, M_BYE, NULL, 0);
        m_run = 0;
        pthread_join(m_th, NULL);
    }
    return 0;
}
int sceNetAdhocMatchingDelete(int id) { (void)id; if (m_fd >= 0) close(m_fd); m_fd = -1; return 0; }

/* ---- PTP over a UNIX stream socket ---- */
int sceNetAdhocPtpListen(const SceNetEtherAddr *s, SceUShort16 p, unsigned int b, unsigned int ri, int rc, int bl, int f)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    (void)s; (void)p; (void)b; (void)ri; (void)rc; (void)f;
    memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX; strcpy(a.sun_path, SOCK_PTP);
    unlink(SOCK_PTP);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, bl) < 0) return -1;
    return fd;
}
int sceNetAdhocPtpAccept(int id, SceNetEtherAddr *addr, SceUShort16 *port, unsigned int timeout, int flag)
{
    struct pollfd pf = { id, POLLIN, 0 };
    (void)addr; (void)port; (void)flag;
    if (poll(&pf, 1, (int)(timeout / 1000 / SPEED) + 1) <= 0) return (int)SCE_ERROR_NET_ADHOC_TIMEOUT;
    return accept(id, NULL, NULL);
}
int sceNetAdhocPtpOpen(const SceNetEtherAddr *s, SceUShort16 sp, const SceNetEtherAddr *d, SceUShort16 dp,
                       unsigned int b, unsigned int ri, int rc, int f)
{
    (void)s; (void)sp; (void)d; (void)dp; (void)b; (void)ri; (void)rc; (void)f;
    return socket(AF_UNIX, SOCK_STREAM, 0);
}
int sceNetAdhocPtpConnect(int id, unsigned int timeout, int flag)
{
    struct sockaddr_un a;
    (void)flag;
    memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX; strcpy(a.sun_path, SOCK_PTP);
    if (connect(id, (struct sockaddr *)&a, sizeof(a)) == 0) { DBG("ptp connected"); return 0; }
    DBG("ptp connect failed errno %d", errno);
    vsleep(timeout);
    return (int)SCE_ERROR_NET_ADHOC_TIMEOUT;
}
int sceNetAdhocPtpSend(int id, const void *data, int *len, unsigned int timeout, int flag)
{
    ssize_t r;
    (void)timeout; (void)flag;
    r = send(id, data, (size_t)*len, MSG_NOSIGNAL);
    if (r < 0) return (int)SCE_ERROR_NET_ADHOC_DISCONNECTED;
    *len = (int)r;
    return 0;
}
int sceNetAdhocPtpRecv(int id, void *buf, int *len, unsigned int timeout, int flag)
{
    struct pollfd pf = { id, POLLIN, 0 };
    ssize_t r;
    (void)flag;
    if (poll(&pf, 1, (int)(timeout / 1000 / SPEED) + 1) <= 0) return (int)SCE_ERROR_NET_ADHOC_TIMEOUT;
    r = recv(id, buf, (size_t)*len, 0);
    if (r <= 0) return (int)SCE_ERROR_NET_ADHOC_DISCONNECTED;
    *len = (int)r;
    return 0;
}
int sceNetAdhocPtpFlush(int id, unsigned int t, int f) { (void)id; (void)t; (void)f; return 0; }
int sceNetAdhocPtpClose(int id, int flag) { (void)flag; return close(id); }
