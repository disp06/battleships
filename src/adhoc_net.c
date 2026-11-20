/*
 * adhoc_net.c - PS Vita native Ad-Hoc layer.
 *
 * 1. Link     : the system NetCheck dialog in ADHOC_CONN mode creates/joins
 *               the Ad-Hoc network for this title (this is how retail Vita
 *               games do it; without it every Ad-Hoc call fails with
 *               SCE_NET_CTL_ERROR_NOT_CONNECTED 0x80412108).
 * 2. Discovery: SceNetAdhocMatching (PARENT = host, CHILD = joiner).  The
 *               host's hello-opt advertises {magic, version, nickname}; the
 *               joiner answers with the same block in SelectTarget().
 * 3. Traffic  : reliable SCE_NET_SOCK_STREAM_P2P socket between the two
 *               consoles' Ad-Hoc IP addresses (port SCE_NET_ADHOC_PORT +
 *               virtual port GAME_VPORT). Fixed 8-byte packets.
 *
 * Threading: matching callbacks -> lock-free ring -> net_update() (main);
 * a worker thread owns the socket (accept/connect, tx flush, rx).
 */
#include <string.h>
#include <stdio.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/adhoc_matching.h>
#include <psp2/netcheck_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include "adhoc_net.h"

/* Matching event codes (same numbering as the PSP matching library). */
#define MEV_HELLO        1
#define MEV_REQUEST      2
#define MEV_LEAVE        3
#define MEV_DENY         4
#define MEV_CANCEL       5
#define MEV_ACCEPT       6
#define MEV_ESTABLISHED  7
#define MEV_TIMEOUT      8
#define MEV_ERROR        9
#define MEV_BYE          10

#define MATCH_PORT       1
#define GAME_VPORT       3000
#define PEER_TIMEOUT_US  8000000ULL
#define PING_EVERY_US    1000000ULL
#define LINK_TIMEOUT_US  15000000ULL

#define MAGIC "BSHP"
#define NETCTL_ERROR_NOT_TERMINATED 0x80412102   /* sceNetCtlInit already done */

typedef struct {
    char    magic[4];
    uint8_t version;
    uint8_t pad[11];
    char    nick[NET_NICK_LEN];
} HelloOpt;                      /* 48 bytes */

/* ----------------------------------------------------------- SPSC rings */
#define RING 64
typedef struct { volatile unsigned w, r; Packet q[RING]; } PktRing;

static int ring_push(PktRing *rg, const Packet *p)
{
    if (rg->w - rg->r >= RING) return -1;
    rg->q[rg->w % RING] = *p;
    __sync_synchronize();
    rg->w++;
    return 0;
}
static int ring_pop(PktRing *rg, Packet *p)
{
    if (rg->r == rg->w) return 0;
    *p = rg->q[rg->r % RING];
    __sync_synchronize();
    rg->r++;
    return 1;
}

typedef struct {
    int type;
    SceNetInAddr addr;
    int opt_len;
    HelloOpt opt;
} MatchEvent;
static volatile unsigned g_ev_w, g_ev_r;
static MatchEvent g_ev[RING];

/* ---------------------------------------------------------------- state */
static int  g_inited = 0;
static volatile NetState g_state = NET_OFF;
static int  g_is_host = 0;
static int  g_match_id = -1;
static volatile int g_listen_sock = -1;
static volatile int g_sock = -1;
static SceNetInAddr g_peer_addr;
static char g_peer_nick[NET_NICK_LEN];
static char g_err[96];
static HelloOpt g_my_opt;
static char g_my_nick[NET_NICK_LEN];

/* NetCheck dialog */
static int g_dialog_active = 0;
static int g_pending_role = 0;          /* 1 host, 2 join (after dialog) */
static SceNetAdhocctlGroupName g_group;

typedef struct { NetHost pub; SceNetInAddr addr; SceUInt64 seen; } HostEntry;
static HostEntry g_hosts[NET_MAX_HOSTS];

static PktRing g_tx, g_rx;
static SceUID  g_worker = -1;
static volatile int g_worker_stop = 0;
static volatile int g_worker_link_req = 0;
static volatile SceUInt64 g_last_rx = 0;
static SceUInt64 g_last_ping = 0;
static SceUInt64 g_link_start = 0;

static uint8_t g_net_mem[1 * 1024 * 1024] __attribute__((aligned(64)));
static uint8_t g_match_pool[128 * 1024] __attribute__((aligned(64)));

static SceUInt64 now_us(void) { return sceKernelGetProcessTimeWide(); }

/* Time since `since`, safe if `since` was stamped after the caller's own
 * snapshot of "now" (unsigned subtraction would wrap). */
static SceUInt64 elapsed_us(SceUInt64 since)
{
    SceUInt64 n = now_us();
    return n > since ? n - since : 0;
}

static void set_error(const char *msg, int code)
{
    if (code) snprintf(g_err, sizeof(g_err), "%s (0x%08X)", msg, (unsigned)code);
    else      snprintf(g_err, sizeof(g_err), "%s", msg);
}

static void fail(const char *msg, int code)
{
    set_error(msg, code);
    g_state = NET_ERROR;
}

/* --------------------------------------------------------- init / term */
int net_init(void)
{
    int ret;
    SceNetInitParam p;

    if (g_inited) return 0;
    g_err[0] = 0;

    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (ret < 0) { fail("Cannot load SceNet", ret); return ret; }
    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET_ADHOC_MATCHING);
    if (ret < 0) { fail("Cannot load Ad-Hoc matching", ret); return ret; }

    p.memory = g_net_mem;
    p.size = sizeof(g_net_mem);
    p.flags = 0;
    ret = sceNetInit(&p);
    if (ret < 0 && ret != (int)SCE_NET_ERROR_EBUSY) { fail("sceNetInit failed", ret); return ret; }

    ret = sceNetCtlInit();
    if (ret < 0 && ret != (int)NETCTL_ERROR_NOT_TERMINATED) { fail("sceNetCtlInit failed", ret); return ret; }

    ret = sceNetAdhocMatchingInit(sizeof(g_match_pool), g_match_pool);
    if (ret < 0 && ret != (int)SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_INITIALIZED) {
        fail("sceNetAdhocMatchingInit failed", ret); return ret;
    }

    g_inited = 1;
    g_state = NET_IDLE;
    return 0;
}

void net_shutdown(void)
{
    if (!g_inited) return;
    net_leave();
    sceNetAdhocMatchingTerm();
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET_ADHOC_MATCHING);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    g_inited = 0;
    g_state = NET_OFF;
}

/* ---------------------------------------------------- matching callback */
static void match_cb(int id, int type, SceNetInAddr *addr, SceSize opt_len, void *opt)
{
    MatchEvent *e;
    (void)id;
    if (g_ev_w - g_ev_r >= RING) return;
    e = &g_ev[g_ev_w % RING];
    e->type = type;
    if (addr) e->addr = *addr; else memset(&e->addr, 0, sizeof(e->addr));
    e->opt_len = 0;
    if (opt && opt_len >= sizeof(HelloOpt)) {
        memcpy(&e->opt, opt, sizeof(HelloOpt));
        e->opt_len = sizeof(HelloOpt);
    }
    __sync_synchronize();
    g_ev_w++;
}

static int valid_opt(const MatchEvent *e)
{
    return e->opt_len == (int)sizeof(HelloOpt) &&
           memcmp(e->opt.magic, MAGIC, 4) == 0 &&
           e->opt.version == NET_PROTO_VERSION;
}

static int start_matching(int mode)
{
    int ret, parent = (mode == SCE_NET_ADHOC_MATCHING_MODE_PARENT);
    memset(&g_my_opt, 0, sizeof(g_my_opt));
    memcpy(g_my_opt.magic, MAGIC, 4);
    g_my_opt.version = NET_PROTO_VERSION;
    strncpy(g_my_opt.nick, g_my_nick, NET_NICK_LEN - 1);

    g_match_id = sceNetAdhocMatchingCreate((SceNetAdhocMatchingMode)mode, 2, MATCH_PORT, 2048,
                                           200 * 1000, 200 * 1000, 30, 200 * 1000, match_cb);
    if (g_match_id < 0) {
        fail("Matching create failed", g_match_id);
        g_match_id = -1; return -1;
    }
    ret = sceNetAdhocMatchingStart(g_match_id, 0x10000100, 0x4000, 0,
                                   parent ? (int)sizeof(g_my_opt) : 0, parent ? &g_my_opt : NULL);
    if (ret < 0) {
        fail("Matching start failed", ret);
        sceNetAdhocMatchingDelete(g_match_id);
        g_match_id = -1; return -1;
    }
    return 0;
}

/* ------------------------------------------------------- P2P socket utils */
static void p2p_addr(SceNetSockaddrIn *a, unsigned int ip)
{
    memset(a, 0, sizeof(*a));
    a->sin_len = sizeof(*a);
    a->sin_family = SCE_NET_AF_INET;
    a->sin_port = sceNetHtons(SCE_NET_ADHOC_PORT);
    a->sin_vport = sceNetHtons(GAME_VPORT);
    a->sin_addr.s_addr = ip;
}

static void set_timeouts(int s, int rcv_us, int snd_us)
{
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &rcv_us, sizeof(rcv_us));
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &snd_us, sizeof(snd_us));
}

static int is_timeout(int r)
{
    return r == (int)SCE_NET_ERROR_EAGAIN || r == (int)SCE_NET_ERROR_ETIMEDOUT ||
           r == (int)SCE_NET_ERROR_EINTR;
}

static int open_listen(void)
{
    SceNetSockaddrIn a;
    int one = 1, s, r;
    s = sceNetSocket("bs_listen", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM_P2P, 0);
    if (s < 0) { fail("P2P socket failed", s); return -1; }
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
    p2p_addr(&a, SCE_NET_INADDR_ANY);
    r = sceNetBind(s, (SceNetSockaddr *)&a, sizeof(a));
    if (r < 0) { sceNetSocketClose(s); fail("P2P bind failed", r); return -1; }
    r = sceNetListen(s, 1);
    if (r < 0) { sceNetSocketClose(s); fail("P2P listen failed", r); return -1; }
    set_timeouts(s, 300 * 1000, 300 * 1000);   /* accept() wakes up to check stop */
    g_listen_sock = s;
    return 0;
}

/* ------------------------------------------------------------ worker */
static void frame_rx(uint8_t *acc, int *acc_len)
{
    while (*acc_len >= NET_PKT_SIZE) {
        Packet p;
        memcpy(&p, acc, NET_PKT_SIZE);
        if (p.type != PKT_PING) ring_push(&g_rx, &p);
        memmove(acc, acc + NET_PKT_SIZE, (size_t)(*acc_len - NET_PKT_SIZE));
        *acc_len -= NET_PKT_SIZE;
    }
}

static int worker(SceSize args, void *argp)
{
    uint8_t acc[256];
    int acc_len = 0;
    (void)args; (void)argp;

    while (!g_worker_stop && !g_worker_link_req) sceKernelDelayThread(10 * 1000);
    if (g_worker_stop) return 0;

    if (g_is_host) {
        while (!g_worker_stop) {
            SceNetSockaddrIn from;
            unsigned int flen = sizeof(from);
            int s = sceNetAccept(g_listen_sock, (SceNetSockaddr *)&from, &flen);
            if (s >= 0) { g_sock = s; break; }
            if (!is_timeout(s)) { fail("P2P accept failed", s); return 0; }
        }
        if (g_worker_stop) return 0;
    } else {
        int tries, r = -1;
        for (tries = 0; tries < 25 && !g_worker_stop; tries++) {
            SceNetSockaddrIn a;
            int s = sceNetSocket("bs_conn", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM_P2P, 0);
            if (s < 0) { fail("P2P socket failed", s); return 0; }
            set_timeouts(s, 500 * 1000, 500 * 1000);
            p2p_addr(&a, g_peer_addr.s_addr);
            g_sock = s;
            r = sceNetConnect(s, (SceNetSockaddr *)&a, sizeof(a));
            if (r >= 0) break;
            g_sock = -1;
            sceNetSocketClose(s);
            sceKernelDelayThread(200 * 1000);     /* host may still be opening */
        }
        if (g_worker_stop) return 0;
        if (r < 0) { fail("P2P connect failed", r); return 0; }
    }

    set_timeouts(g_sock, 50 * 1000, 500 * 1000);
    g_last_rx = now_us();
    g_state = NET_CONNECTED;
    {
        Packet h; memset(&h, 0, sizeof(h));
        h.type = PKT_HELLO; h.a = NET_PROTO_VERSION;
        ring_push(&g_tx, &h);
    }

    for (;;) {
        Packet p;
        int r;
        while (ring_pop(&g_tx, &p)) {
            int sent = 0;
            while (sent < NET_PKT_SIZE) {
                r = sceNetSend(g_sock, (uint8_t *)&p + sent, (unsigned)(NET_PKT_SIZE - sent), 0);
                if (r > 0) { sent += r; continue; }
                if (is_timeout(r) && !g_worker_stop) continue;
                if (g_state == NET_CONNECTED && !g_worker_stop) { set_error("Connection lost (send)", r); g_state = NET_DISCONNECTED; }
                return 0;
            }
        }
        if (g_worker_stop) return 0;

        r = sceNetRecv(g_sock, acc + acc_len, (unsigned)(sizeof(acc) - (unsigned)acc_len > 64 ? 64 : sizeof(acc) - (unsigned)acc_len), 0);
        if (r > 0) {
            acc_len += r;
            g_last_rx = now_us();
            frame_rx(acc, &acc_len);
        } else if (r == 0 || !is_timeout(r)) {
            if (g_state == NET_CONNECTED && !g_worker_stop) { set_error("Opponent disconnected", r); g_state = NET_DISCONNECTED; }
            return 0;
        }
        if (elapsed_us(g_last_rx) > PEER_TIMEOUT_US) {
            if (g_state == NET_CONNECTED) { set_error("Connection timed out", 0); g_state = NET_DISCONNECTED; }
            return 0;
        }
    }
}

static int start_worker(void)
{
    g_worker_stop = 0;
    g_worker_link_req = 0;
    g_tx.r = g_tx.w = 0;
    g_rx.r = g_rx.w = 0;
    g_worker = sceKernelCreateThread("bs_net", worker, 0x10000100, 0x8000, 0, 0, NULL);
    if (g_worker < 0) { fail("Cannot create net thread", g_worker); g_worker = -1; return -1; }
    sceKernelStartThread(g_worker, 0, NULL);
    return 0;
}

/* -------------------------------------------------- NetCheck dialog */
static int open_netcheck_dialog(void)
{
    /* Some firmware/param combinations insist on a group name or timeout,
     * others reject them - try the plain form first, then fall back. */
    static const struct { int group; unsigned timeout; } tries[] = {
        { 0, 0 }, { 1, 0 }, { 0, 30 * 1000 * 1000 }, { 1, 30 * 1000 * 1000 }
    };
    int i, ret = -1;
    memset(&g_group, 0, sizeof(g_group));
    memcpy(g_group.data, "BTLSHP01", SCE_NET_ADHOCCTL_GROUPNAME_LEN);
    for (i = 0; i < (int)(sizeof(tries) / sizeof(tries[0])); i++) {
        SceNetCheckDialogParam p;
        sceNetCheckDialogParamInit(&p);
        p.mode = SCE_NETCHECK_DIALOG_MODE_ADHOC_CONN;
        p.groupName = tries[i].group ? &g_group : NULL;
        p.timeoutUs = tries[i].timeout;
        ret = sceNetCheckDialogInit(&p);
        if (ret >= 0) return 0;
        if (ret != (int)SCE_NETCHECK_DIALOG_ERROR_PARAM &&
            ret != (int)SCE_NETCHECK_DIALOG_ERROR_INVALID_PSPADHOC_PARAM &&
            ret != (int)SCE_NETCHECK_DIALOG_ERROR_INVALID_TIMEOUT_PARAM) break;
    }
    fail("Cannot open the Ad-Hoc dialog", ret);
    return -1;
}

static int adhoc_link_up(void)
{
    int st = 0;
    return sceNetCtlAdhocGetState(&st) >= 0 && st == SCE_NETCTL_STATE_CONNECTED;
}

/* Called once the Vita is on the Ad-Hoc network. */
static void start_role(void)
{
    g_ev_r = g_ev_w = 0;
    memset(g_hosts, 0, sizeof(g_hosts));
    if (g_is_host) {
        if (open_listen() < 0) return;
        if (start_matching(SCE_NET_ADHOC_MATCHING_MODE_PARENT) < 0) return;
        if (start_worker() < 0) return;
        g_state = NET_HOSTING;
    } else {
        if (start_matching(SCE_NET_ADHOC_MATCHING_MODE_CHILD) < 0) return;
        if (start_worker() < 0) return;
        g_state = NET_SCANNING;
    }
}

static int begin(int host, const char *nickname)
{
    if (net_init() < 0) return -1;
    net_leave();
    g_err[0] = 0;
    g_is_host = host;
    g_peer_nick[0] = 0;
    memset(g_my_nick, 0, sizeof(g_my_nick));
    strncpy(g_my_nick, nickname ? nickname : "Captain", NET_NICK_LEN - 1);

    if (adhoc_link_up()) {            /* already on the Ad-Hoc network */
        start_role();
        return g_state == NET_ERROR ? -1 : 0;
    }
    if (open_netcheck_dialog() < 0) return -1;
    g_dialog_active = 1;
    g_pending_role = host ? 1 : 2;
    g_state = NET_STARTING;
    return 0;
}

/* ------------------------------------------------------------ public */
int net_host(const char *nickname) { return begin(1, nickname); }
int net_scan(const char *nickname) { return begin(0, nickname); }
int net_dialog_active(void)        { return g_dialog_active; }

int net_host_count(void)
{
    int i, n = 0;
    for (i = 0; i < NET_MAX_HOSTS; i++) if (g_hosts[i].pub.valid) n++;
    return n;
}

const NetHost *net_host_get(int idx)
{
    int i;
    for (i = 0; i < NET_MAX_HOSTS; i++)
        if (g_hosts[i].pub.valid && idx-- == 0) return &g_hosts[i].pub;
    return NULL;
}

int net_join(int idx)
{
    int i, ret;
    if (g_state != NET_SCANNING) return -1;
    for (i = 0; i < NET_MAX_HOSTS; i++)
        if (g_hosts[i].pub.valid && idx-- == 0) break;
    if (i >= NET_MAX_HOSTS) return -1;
    g_peer_addr = g_hosts[i].addr;
    strncpy(g_peer_nick, g_hosts[i].pub.nick, NET_NICK_LEN - 1);
    ret = sceNetAdhocMatchingSelectTarget(g_match_id, &g_peer_addr, (int)sizeof(g_my_opt), &g_my_opt);
    if (ret < 0) { fail("Join request failed", ret); return -1; }
    g_state = NET_JOINING;
    g_link_start = now_us();
    return 0;
}

void net_leave(void)
{
    if (!g_inited) return;
    if (g_dialog_active) {
        sceNetCheckDialogAbort();
        /* the dialog finishes asynchronously; net_update() terms it */
    }
    g_pending_role = 0;
    if (g_worker >= 0) {
        if (g_state == NET_CONNECTED) {
            Packet q; memset(&q, 0, sizeof(q)); q.type = PKT_QUIT;
            ring_push(&g_tx, &q);
            sceKernelDelayThread(150 * 1000);   /* let the worker flush QUIT */
        }
        g_worker_stop = 1;
        if (g_sock >= 0) sceNetSocketAbort(g_sock, 0);
        if (g_listen_sock >= 0) sceNetSocketAbort(g_listen_sock, 0);
        sceKernelWaitThreadEnd(g_worker, NULL, NULL);
        sceKernelDeleteThread(g_worker);
        g_worker = -1;
    }
    if (g_sock >= 0)        { sceNetSocketClose(g_sock);        g_sock = -1; }
    if (g_listen_sock >= 0) { sceNetSocketClose(g_listen_sock); g_listen_sock = -1; }
    if (g_match_id >= 0) {
        sceNetAdhocMatchingStop(g_match_id);
        sceNetAdhocMatchingDelete(g_match_id);
        g_match_id = -1;
    }
    if (adhoc_link_up()) sceNetCtlAdhocDisconnect();   /* give Wi-Fi back */
    g_state = NET_IDLE;
}

NetState    net_state(void)      { return g_state; }
const char *net_peer_nick(void)  { return g_peer_nick[0] ? g_peer_nick : "Opponent"; }
const char *net_last_error(void) { return g_err; }
int         net_is_host(void)    { return g_is_host; }

int net_send(const Packet *p)
{
    if (g_state != NET_CONNECTED) return -1;
    return ring_push(&g_tx, p);
}

int net_recv(Packet *p) { return ring_pop(&g_rx, p); }

static void begin_link(void)
{
    g_state = NET_LINKING;
    g_link_start = now_us();
    __sync_synchronize();
    g_worker_link_req = 1;
}

static void update_dialog(void)
{
    SceNetCheckDialogResult res;
    if (!g_dialog_active) return;
    if (sceNetCheckDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;

    memset(&res, 0, sizeof(res));
    sceNetCheckDialogGetResult(&res);
    sceNetCheckDialogTerm();
    g_dialog_active = 0;

    if (!g_pending_role) return;                 /* aborted by net_leave() */
    g_pending_role = 0;
    if (res.result == SCE_COMMON_DIALOG_RESULT_USER_CANCELED) {
        set_error("Ad-Hoc connection cancelled", 0);
        g_state = NET_CANCELLED;
        return;
    }
    if (res.result != SCE_COMMON_DIALOG_RESULT_OK || !adhoc_link_up()) {
        int st = -1;
        sceNetCtlAdhocGetState(&st);
        snprintf(g_err, sizeof(g_err), "Ad-Hoc network unavailable (result %d, state %d)", (int)res.result, st);
        g_state = NET_ERROR;
        return;
    }
    start_role();
}

void net_update(void)
{
    update_dialog();

    /* ---- matching events (from callback thread) ---- */
    while (g_ev_r != g_ev_w) {
        MatchEvent e = g_ev[g_ev_r % RING];
        __sync_synchronize();
        g_ev_r++;

        switch (e.type) {
        case MEV_HELLO:
            if (!g_is_host && g_state == NET_SCANNING && valid_opt(&e)) {
                int i, slot = -1;
                for (i = 0; i < NET_MAX_HOSTS; i++)
                    if (g_hosts[i].pub.valid && g_hosts[i].addr.s_addr == e.addr.s_addr) { slot = i; break; }
                if (slot < 0)
                    for (i = 0; i < NET_MAX_HOSTS; i++) if (!g_hosts[i].pub.valid) { slot = i; break; }
                if (slot >= 0) {
                    g_hosts[slot].pub.valid = 1;
                    g_hosts[slot].addr = e.addr;
                    g_hosts[slot].seen = now_us();
                    memcpy(g_hosts[slot].pub.nick, e.opt.nick, NET_NICK_LEN);
                    g_hosts[slot].pub.nick[NET_NICK_LEN - 1] = 0;
                }
            }
            break;
        case MEV_REQUEST:
            if (g_is_host && g_state == NET_HOSTING && valid_opt(&e)) {
                g_peer_addr = e.addr;
                memcpy(g_peer_nick, e.opt.nick, NET_NICK_LEN);
                g_peer_nick[NET_NICK_LEN - 1] = 0;
                sceNetAdhocMatchingSelectTarget(g_match_id, &e.addr, 0, NULL);
            } else if (g_is_host) {
                sceNetAdhocMatchingCancelTarget(g_match_id, &e.addr);
            }
            break;
        case MEV_ESTABLISHED:
            if ((g_is_host && g_state == NET_HOSTING) || (!g_is_host && g_state == NET_JOINING))
                begin_link();
            break;
        case MEV_DENY:
            if (g_state == NET_JOINING) fail("Host declined (game full?)", 0);
            break;
        case MEV_TIMEOUT:
        case MEV_ERROR:
            if (g_state == NET_JOINING || g_state == NET_LINKING) fail("Connection attempt timed out", 0);
            break;
        case MEV_LEAVE:
        case MEV_BYE:
        case MEV_CANCEL:
            if (!g_is_host && g_state == NET_SCANNING) {
                int i;
                for (i = 0; i < NET_MAX_HOSTS; i++)
                    if (g_hosts[i].addr.s_addr == e.addr.s_addr) g_hosts[i].pub.valid = 0;
            } else if ((g_state == NET_CONNECTED || g_state == NET_LINKING) &&
                       g_peer_addr.s_addr == e.addr.s_addr) {
                set_error("Opponent left the game", 0);
                g_state = NET_DISCONNECTED;
            }
            break;
        default: break;
        }
    }

    if (g_state == NET_SCANNING) {
        int i;
        for (i = 0; i < NET_MAX_HOSTS; i++)
            if (g_hosts[i].pub.valid && elapsed_us(g_hosts[i].seen) > 5000000ULL) g_hosts[i].pub.valid = 0;
    }

    if ((g_state == NET_JOINING || g_state == NET_LINKING) && elapsed_us(g_link_start) > LINK_TIMEOUT_US)
        fail("Could not reach the other Vita", 0);

    if (g_state == NET_CONNECTED && elapsed_us(g_last_ping) > PING_EVERY_US) {
        Packet p; memset(&p, 0, sizeof(p)); p.type = PKT_PING;
        ring_push(&g_tx, &p);
        g_last_ping = now_us();
    }
}
