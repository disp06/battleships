/*
 * adhoc_net.h - PS Vita Ad-Hoc layer: discovery/handshake via
 * SceNetAdhocMatching, game traffic via reliable PTP sockets.
 */
#ifndef BS_ADHOC_NET_H
#define BS_ADHOC_NET_H

#include <stdint.h>

#define NET_ADHOC_ID     "BTLSHP001"   /* 9-char Ad-Hoc product ID (== TITLE_ID) */
#define NET_MAX_HOSTS    8
#define NET_NICK_LEN     32

/* ---------------- Wire protocol: fixed 8-byte packets ---------------- */
typedef enum {
    PKT_HELLO   = 0x01,   /* version handshake right after PTP connect */
    PKT_READY   = 0x02,   /* fleet setup complete                      */
    PKT_FIRE    = 0x03,   /* a=x, b=y                                  */
    PKT_RESULT  = 0x04,   /* a=x, b=y, c=status, d..g = sunk ship      */
    PKT_REMATCH = 0x05,
    PKT_QUIT    = 0x06,
    PKT_PING    = 0x07    /* keep-alive                                */
} PacketType;

#define NET_PROTO_VERSION 1
#define NET_PKT_SIZE      8

typedef struct {
    uint8_t type;
    uint8_t a, b, c;      /* x, y, status (0 miss, 1 hit, 2 sunk)  */
    uint8_t d, e, f, g;   /* sunk ship: origin x, origin y, len, horiz */
} Packet;

typedef enum {
    NET_OFF = 0,          /* not initialised                       */
    NET_IDLE,             /* stack up, nothing running             */
    NET_HOSTING,          /* waiting for a challenger              */
    NET_SCANNING,         /* listening for hosts                   */
    NET_JOINING,          /* request sent, waiting for host        */
    NET_LINKING,          /* matching established, opening PTP     */
    NET_CONNECTED,        /* PTP up - game packets flow            */
    NET_DISCONNECTED,     /* peer left / timed out                 */
    NET_ERROR
} NetState;

typedef struct {
    char    nick[NET_NICK_LEN];
    uint8_t valid;
} NetHost;

int         net_init(void);                 /* lazily brings up sceNet/Ad-Hoc; 0 = ok */
void        net_shutdown(void);
int         net_host(const char *nickname);
int         net_scan(const char *nickname);
int         net_host_count(void);
const NetHost *net_host_get(int i);
int         net_join(int index);
void        net_leave(void);                /* back to NET_IDLE, sends QUIT if linked */
NetState    net_state(void);
const char *net_peer_nick(void);
const char *net_last_error(void);
int         net_is_host(void);

int         net_send(const Packet *p);      /* queued, non-blocking; 0 = ok */
int         net_recv(Packet *p);            /* 1 if a packet was dequeued   */
void        net_update(void);               /* call once per frame          */

#endif
