// Local play (the Switch's local wireless, nn::ldn) over IP: one player's game hosts a hub, the others join it by
// address. ldn.c is nn::ldn plus the link between the games (TCP: each client keeps one connection to the host,
// which relays); vsock.c is nn::socket on a virtual network: the sockets the game opens for its own traffic exist
// only here, their packets travel inside the link. Addresses are the ones a Switch would use (169.254.x.n, the
// host n = 1), so the game sees the network it expects.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A packet of the virtual network (fields in host byte order, as sent inside the link after the frame header).
enum { VPROTO_UDP = 0, VPROTO_TCP = 1 };
enum { VTCP_DATA = 0, VTCP_SYN, VTCP_SYNACK, VTCP_FIN, VTCP_RST };
typedef struct {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint8_t proto, kind;  // VPROTO_*; for TCP VTCP_*
    uint16_t pad;
} VPacket;

// MHGU_LOG_NET=2: every nn::ldn / nn::socket call (the first 40 of each, then every 1000th), with r0-r2.
#define NTRACE()                                                                                                   \
    do {                                                                                                           \
        const char *nt_ = ENV("MHGU_LOG_NET");                                                                     \
        static int ntn_;                                                                                           \
        if (nt_ && atoi(nt_) >= 2 && (ntn_++ < 40 || ntn_ % 1000 == 0))                                            \
            LOG("[net] %s #%d (%x, %x, %x)\n", __func__, ntn_, ARG(0), ARG(1), ARG(2));                            \
    } while (0)

// ldn.c
bool ldn_net_up(void);           // a network exists (hosting with it created, or joined)
bool ldn_hosting(void);          // we host it
// The hub's players and their maximum, and an id for it (a hash of its session id: the same in every player's
// game); false: no hub.
bool ldn_party(int *n, int *max, char *id, size_t size);
uint32_t ldn_own_ip(void);        // 0 without a network
uint32_t ldn_broadcast_ip(void);
void ldn_send(const VPacket *p, const void *data, uint32_t len);  // from a local socket: into the network
void ldn_send_aux(const void *data, uint32_t len);  // the port's own message to every other game (reliable)
// The hub server (internet hubs) for the settings menu: its last known state for these settings (ini Server, Key),
// from a search, a hosted hub or a check; ms: the answer's round trip, hubs: listed for the key (-1: not known).
enum { HUB_UNKNOWN, HUB_CHECKING, HUB_OFF, HUB_ONLINE, HUB_NOT_FOUND, HUB_UNREACHABLE, HUB_LOST, HUB_NO_ANSWER,
       HUB_BUSY, HUB_VERSION, HUB_REFUSED };
int ldn_hub_status(const char *server, const char *key, int *ms, int *hubs);  // HUB_*
void ldn_hub_check(const char *server, const char *key);  // asks the server for its hubs, in a thread

// vsock.c
void vsock_deliver(const VPacket *p, const void *data, uint32_t len);  // from the network to the local sockets
void vsock_peer_gone(uint32_t ip);  // a node left: its TCP connections end
void vsock_network_down(void);      // the network is gone: every connection ends, queues empty

// emsync.c: small monsters synced between the players in a zone
void emsync_tick(void);             // each frame, from the game's input polling
void emsync_aux_received(const void *data, uint32_t len);  // an F_AUX message from another game

// hle/hpscale.c: large monster HP by the number of hunters; the host's settings go to the others
void hpscale_tick(void);            // each frame, from the game's input polling
void hpscale_aux_received(const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif
