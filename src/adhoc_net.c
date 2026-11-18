/*
 * adhoc_net.c - VitaSDK Ad-Hoc matching & PTP socket layer.
 *
 * Discovery / handshake : SceNetAdhocMatching (PARENT = host, CHILD = joiner).
 *     The host's hello-opt advertises {magic, version, MAC, nickname}; the
 *     joiner replies with the same block in SelectTarget().  Exchanging the
 *     MAC addresses this way lets both sides open a PTP connection without
 *     any extra lookup.
 * Game traffic          : PTP (reliable, ordered) - host listens, joiner
 *     connects.  Fixed 8-byte packets, re-framed from the byte stream.
 *
 * Threading
 *   - the matching library calls match_cb() on its own thread; events go
 *     into a lock-free SPSC ring and are handled in net_update() (main).
 *   - a worker thread owns the PTP socket: accept/connect, tx flush, rx.
 *     Main <-> worker traffic uses two more SPSC rings, so nothing locks.
 */
#include <string.h>
#include <stdio.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/adhoc_matching.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
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
#define PTP_PORT         0x5342    /* 'SB' */
#define PTP_BUF          8192
#define PTP_REXMT_US     200000
#define PTP_REXMT_CNT    50
#define PEER_TIMEOUT_US  8000000ULL
#define PING_EVERY_US    1000000ULL
#define LINK_TIMEOUT_US  15000000ULL

#define MAGIC "BSHP"
#define NETCTL_ERROR_NOT_TERMINATED 0x80412102   /* sceNetCtlInit already done */

typedef struct {
    char    magic[4];
    uint8_t version;
    uint8_t pad[3];
    SceNetEtherAddr mac;
    uint8_t pad2[2];
    char    nick[NET_NICK_LEN];
} HelloOpt;                      /* 48 bytes, sent in hello / select opts */

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
static int  g_listen_id = -1;
static int  g_ptp_id = -1;
static SceNetEtherAddr g_my_mac;
static SceNetEtherAddr g_peer_mac;
static SceNetInAddr    g_peer_addr;
static char g_peer_nick[NET_NICK_LEN];
static char g_err[96];
static HelloOpt g_my_opt;

typedef struct { NetHost pub; SceNetInAddr addr; SceNetEtherAddr mac; SceUInt64 seen; } HostEntry;
static HostEntry g_hosts[NET_MAX_HOSTS];

static PktRing g_tx, g_rx;
static SceUID  g_worker = -1;
static volatile int g_worker_stop = 0;
static volatile int g_worker_link_req = 0;   /* main -> worker: open PTP now */
static volatile SceUInt64 g_last_rx = 0;
static SceUInt64 g_last_ping = 0;
static SceUInt64 g_link_start = 0;

static uint8_t g_net_mem[1 * 1024 * 1024] __attribute__((aligned(64)));
static uint8_t g_match_pool[128 * 1024] __attribute__((aligned(64)));

static SceUInt64 now_us(void) { return sceKernelGetProcessTimeWide(); }

/* Time since `since`, safe when `since` was stamped after the caller's
 * own snapshot of "now" (unsigned subtraction would wrap to a huge value). */
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

/* --------------------------------------------------------- init / term */
int net_init(void)
{
    int ret;
    SceNetInitParam p;
    SceNetAdhocctlAdhocId id;

    if (g_inited) return 0;
    g_err[0] = 0;

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_PSPNET_ADHOC);
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET_ADHOC_MATCHING);

    p.memory = g_net_mem;
    p.size = sizeof(g_net_mem);
    p.flags = 0;
    ret = sceNetInit(&p);
    if (ret < 0 && ret != (int)SCE_NET_ERROR_EBUSY /* already initialised */) {
        set_error("sceNetInit failed", ret); g_state = NET_ERROR; return ret;
    }
    ret = sceNetCtlInit();
    if (ret < 0 && ret != (int)NETCTL_ERROR_NOT_TERMINATED) {
        set_error("sceNetCtlInit failed", ret); g_state = NET_ERROR; return ret;
    }
    ret = sceNetAdhocInit();
    if (ret < 0 && ret != (int)SCE_ERROR_NET_ADHOC_ALREADY_INITIALIZED) {
        set_error("sceNetAdhocInit failed", ret); g_state = NET_ERROR; return ret;
    }

    memset(&id, 0, sizeof(id));
    id.type = SCE_NET_ADHOCCTL_ADHOCTYPE_PRODUCT_ID;
    memcpy(id.data, NET_ADHOC_ID, SCE_NET_ADHOCCTL_ADHOCID_LEN);
    ret = sceNetAdhocctlInit(&id);
    if (ret < 0 && ret != (int)SCE_ERROR_NET_ADHOCCTL_ALREADY_INITIALIZED) {
        set_error("sceNetAdhocctlInit failed", ret); g_state = NET_ERROR; return ret;
    }

    ret = sceNetAdhocMatchingInit(sizeof(g_match_pool), g_match_pool);
    if (ret < 0 && ret != (int)SCE_NET_ADHOC_MATCHING_ERROR_ALREADY_INITIALIZED) {
        set_error("sceNetAdhocMatchingInit failed", ret); g_state = NET_ERROR; return ret;
    }

    ret = sceNetAdhocctlGetEtherAddr(&g_my_mac);
    if (ret < 0) { set_error("Cannot read own MAC address", ret); g_state = NET_ERROR; return ret; }

    g_inited = 1;
    g_state = NET_IDLE;
    return 0;
}

void net_shutdown(void)
{
    if (!g_inited) return;
    net_leave();
    sceNetAdhocMatchingTerm();
    sceNetAdhocctlTerm();
    sceNetAdhocTerm();
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET_ADHOC_MATCHING);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_PSPNET_ADHOC);
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

static int start_matching(int mode, const char *nickname)
{
    int ret;
    memset(&g_my_opt, 0, sizeof(g_my_opt));
    memcpy(g_my_opt.magic, MAGIC, 4);
    g_my_opt.version = NET_PROTO_VERSION;
    g_my_opt.mac = g_my_mac;
    strncpy(g_my_opt.nick, nickname ? nickname : "Captain", NET_NICK_LEN - 1);

    g_match_id = sceNetAdhocMatchingCreate((SceNetAdhocMatchingMode)mode, 2, MATCH_PORT, 2048,
                                           200 * 1000, 200 * 1000, 30, 200 * 1000, match_cb);
    if (g_match_id < 0) {
        set_error("Matching create failed", g_match_id);
        g_match_id = -1; g_state = NET_ERROR; return -1;
    }
    ret = sceNetAdhocMatchingStart(g_match_id, 0x10000100, 0x4000, 0,
                                   mode == SCE_NET_ADHOC_MATCHING_MODE_PARENT ? (int)sizeof(g_my_opt) : 0,
                                   mode == SCE_NET_ADHOC_MATCHING_MODE_PARENT ? &g_my_opt : NULL);
    if (ret < 0) {
        set_error("Matching start failed", ret);
        sceNetAdhocMatchingDelete(g_match_id);
        g_match_id = -1; g_state = NET_ERROR; return -1;
    }
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

    /* ---- wait for main to request link, then accept / connect ---- */
    while (!g_worker_stop && !g_worker_link_req) sceKernelDelayThread(10 * 1000);
    if (g_worker_stop) return 0;

    if (g_is_host) {
        SceNetEtherAddr peer; SceUShort16 port;
        int sid = -1;
        while (!g_worker_stop) {
            sid = sceNetAdhocPtpAccept(g_listen_id, &peer, &port, 500 * 1000, 0);
            if (sid >= 0) break;
            if (sid != (int)SCE_ERROR_NET_ADHOC_TIMEOUT && sid != (int)SCE_ERROR_NET_ADHOC_WOULD_BLOCK) {
                set_error("PTP accept failed", sid); g_state = NET_ERROR; return 0;
            }
        }
        if (g_worker_stop) return 0;
        g_ptp_id = sid;
    } else {
        int sid, ret = -1, tries;
        sid = sceNetAdhocPtpOpen(&g_my_mac, 0, &g_peer_mac, PTP_PORT, PTP_BUF, PTP_REXMT_US, PTP_REXMT_CNT, 0);
        if (sid < 0) { set_error("PTP open failed", sid); g_state = NET_ERROR; return 0; }
        g_ptp_id = sid;
        for (tries = 0; tries < 20 && !g_worker_stop; tries++) {
            ret = sceNetAdhocPtpConnect(sid, 500 * 1000, 0);
            if (ret >= 0) break;
            sceKernelDelayThread(100 * 1000);
        }
        if (ret < 0) { set_error("PTP connect failed", ret); g_state = NET_ERROR; return 0; }
    }

    g_last_rx = now_us();
    g_state = NET_CONNECTED;
    {
        Packet h; memset(&h, 0, sizeof(h));
        h.type = PKT_HELLO; h.a = NET_PROTO_VERSION;
        ring_push(&g_tx, &h);
    }

    /* ---- connected loop: flush tx, then short blocking recv ---- */
    for (;;) {
        Packet p;
        int ret, len;
        while (ring_pop(&g_tx, &p)) {
            len = NET_PKT_SIZE;
            ret = sceNetAdhocPtpSend(g_ptp_id, &p, &len, 500 * 1000, 0);
            if (ret < 0 && ret != (int)SCE_ERROR_NET_ADHOC_TIMEOUT) {
                if (g_state == NET_CONNECTED) { set_error("Connection lost (send)", ret); g_state = NET_DISCONNECTED; }
                return 0;
            }
        }
        if (g_worker_stop) {
            sceNetAdhocPtpFlush(g_ptp_id, 200 * 1000, 0);
            return 0;
        }
        len = (int)sizeof(acc) - acc_len;
        if (len > 64) len = 64;
        ret = sceNetAdhocPtpRecv(g_ptp_id, acc + acc_len, &len, 50 * 1000, 0);
        if (ret >= 0 && len > 0) {
            acc_len += len;
            g_last_rx = now_us();
            frame_rx(acc, &acc_len);
        } else if (ret < 0 && ret != (int)SCE_ERROR_NET_ADHOC_TIMEOUT &&
                   ret != (int)SCE_ERROR_NET_ADHOC_WOULD_BLOCK) {
            if (g_state == NET_CONNECTED) { set_error("Opponent disconnected", ret); g_state = NET_DISCONNECTED; }
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
    if (g_worker < 0) { set_error("Cannot create net thread", g_worker); g_state = NET_ERROR; return -1; }
    sceKernelStartThread(g_worker, 0, NULL);
    return 0;
}

/* ------------------------------------------------------------ public */
int net_host(const char *nickname)
{
    if (net_init() < 0) return -1;
    net_leave();
    g_is_host = 1;
    g_peer_nick[0] = 0;
    g_ev_r = g_ev_w = 0;
    g_listen_id = sceNetAdhocPtpListen(&g_my_mac, PTP_PORT, PTP_BUF, PTP_REXMT_US, PTP_REXMT_CNT, 1, 0);
    if (g_listen_id < 0) {
        set_error("PTP listen failed", g_listen_id);
        g_listen_id = -1; g_state = NET_ERROR; return -1;
    }
    if (start_matching(SCE_NET_ADHOC_MATCHING_MODE_PARENT, nickname) < 0) return -1;
    if (start_worker() < 0) return -1;
    g_state = NET_HOSTING;
    return 0;
}

int net_scan(const char *nickname)
{
    if (net_init() < 0) return -1;
    net_leave();
    g_is_host = 0;
    g_peer_nick[0] = 0;
    g_ev_r = g_ev_w = 0;
    memset(g_hosts, 0, sizeof(g_hosts));
    if (start_matching(SCE_NET_ADHOC_MATCHING_MODE_CHILD, nickname) < 0) return -1;
    if (start_worker() < 0) return -1;
    g_state = NET_SCANNING;
    return 0;
}

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
    g_peer_mac = g_hosts[i].mac;
    strncpy(g_peer_nick, g_hosts[i].pub.nick, NET_NICK_LEN - 1);
    ret = sceNetAdhocMatchingSelectTarget(g_match_id, &g_peer_addr, (int)sizeof(g_my_opt), &g_my_opt);
    if (ret < 0) { set_error("Join request failed", ret); g_state = NET_ERROR; return -1; }
    g_state = NET_JOINING;
    g_link_start = now_us();
    return 0;
}

void net_leave(void)
{
    if (!g_inited) return;
    if (g_worker >= 0) {
        if (g_state == NET_CONNECTED) {
            Packet q; memset(&q, 0, sizeof(q)); q.type = PKT_QUIT;
            ring_push(&g_tx, &q);
        }
        g_worker_stop = 1;
        sceKernelWaitThreadEnd(g_worker, NULL, NULL);
        sceKernelDeleteThread(g_worker);
        g_worker = -1;
    }
    if (g_ptp_id >= 0)    { sceNetAdhocPtpClose(g_ptp_id, 0);    g_ptp_id = -1; }
    if (g_listen_id >= 0) { sceNetAdhocPtpClose(g_listen_id, 0); g_listen_id = -1; }
    if (g_match_id >= 0) {
        sceNetAdhocMatchingStop(g_match_id);
        sceNetAdhocMatchingDelete(g_match_id);
        g_match_id = -1;
    }
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

void net_update(void)
{
    SceUInt64 t = now_us();

    /* ---- matching events (from callback thread) ---- */
    while (g_ev_r != g_ev_w) {
        MatchEvent e = g_ev[g_ev_r % RING];
        __sync_synchronize();
        g_ev_r++;

        switch (e.type) {
        case MEV_HELLO:      /* joiner sees a host advertising */
            if (!g_is_host && g_state == NET_SCANNING && valid_opt(&e)) {
                int i, slot = -1;
                for (i = 0; i < NET_MAX_HOSTS; i++)
                    if (g_hosts[i].pub.valid && memcmp(&g_hosts[i].addr, &e.addr, sizeof(e.addr)) == 0) { slot = i; break; }
                if (slot < 0)
                    for (i = 0; i < NET_MAX_HOSTS; i++) if (!g_hosts[i].pub.valid) { slot = i; break; }
                if (slot >= 0) {
                    g_hosts[slot].pub.valid = 1;
                    g_hosts[slot].addr = e.addr;
                    g_hosts[slot].mac = e.opt.mac;
                    g_hosts[slot].seen = t;
                    memcpy(g_hosts[slot].pub.nick, e.opt.nick, NET_NICK_LEN);
                    g_hosts[slot].pub.nick[NET_NICK_LEN - 1] = 0;
                }
            }
            break;
        case MEV_REQUEST:    /* host receives a join request */
            if (g_is_host && g_state == NET_HOSTING && valid_opt(&e)) {
                g_peer_addr = e.addr;
                g_peer_mac = e.opt.mac;
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
            if (g_state == NET_JOINING) { set_error("Host declined (game full?)", 0); g_state = NET_ERROR; }
            break;
        case MEV_TIMEOUT:
        case MEV_ERROR:
            if (g_state == NET_JOINING || g_state == NET_LINKING) { set_error("Connection attempt timed out", 0); g_state = NET_ERROR; }
            break;
        case MEV_LEAVE:
        case MEV_BYE:
        case MEV_CANCEL:
            if (!g_is_host && g_state == NET_SCANNING) {
                int i;
                for (i = 0; i < NET_MAX_HOSTS; i++)
                    if (memcmp(&g_hosts[i].addr, &e.addr, sizeof(e.addr)) == 0) g_hosts[i].pub.valid = 0;
            } else if (g_state == NET_CONNECTED || g_state == NET_LINKING) {
                if (memcmp(&g_peer_addr, &e.addr, sizeof(e.addr)) == 0) {
                    set_error("Opponent left the game", 0); g_state = NET_DISCONNECTED;
                }
            }
            break;
        default: break;
        }
    }

    /* ---- stale host entries ---- */
    if (g_state == NET_SCANNING) {
        int i;
        for (i = 0; i < NET_MAX_HOSTS; i++)
            if (g_hosts[i].pub.valid && elapsed_us(g_hosts[i].seen) > 5000000ULL) g_hosts[i].pub.valid = 0;
    }

    /* ---- link watchdog ---- */
    if ((g_state == NET_JOINING || g_state == NET_LINKING) && elapsed_us(g_link_start) > LINK_TIMEOUT_US) {
        set_error("Could not reach the other Vita", 0);
        g_state = NET_ERROR;
    }

    /* ---- keep-alive ---- */
    if (g_state == NET_CONNECTED && elapsed_us(g_last_ping) > PING_EVERY_US) {
        Packet p; memset(&p, 0, sizeof(p)); p.type = PKT_PING;
        ring_push(&g_tx, &p);
        g_last_ping = t;
    }
}
