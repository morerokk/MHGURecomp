// RyuLdn: the lobby protocol of Ryujinx's LDN servers (Ryubing/LdnServer), for crossplay with Ryujinx players.
// A client keeps one TCP connection to a server, which owns the lobbies (it lists them, adds players, assigns each
// the virtual address 10.114.x.y) and relays the game's packets (ProxyData); a host may offer its own relay (P2P
// proxy), which a joining client then connects to as well. ldn.c uses this as a second transport next to its own
// link (ldn.c, "Crossplay").
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RYU_NETINFO 0x480
#define RYU_MAX_SERVERS 4

// A game packet in the server's terms (host order); proto 17 UDP, 6 TCP.
typedef struct { uint32_t src_ip, dst_ip; uint16_t src_port, dst_port; uint32_t proto; } RyuProxy;

// Called from the client's reader thread. lobby: the lobby's NetworkInfo changed (info NULL: the lobby is gone or the
// connection broke); data: a game packet arrived.
typedef struct {
    void (*lobby)(const uint8_t *info);
    void (*data)(const RyuProxy *p, const void *data, uint32_t len);
} RyuHooks;
void ryu_set_hooks(const RyuHooks *h);

// ini [Multiplayer] RyuServers: the servers searched (host[:port], comma separated; "" none).
int ryu_server_count(void);
bool ryu_enabled(void);
// The server hubs we create are made on: ini RyuHost, else the one this game runs itself (ryusrv.h; set by ldn.c). "" none.
const char *ryu_host_addr(void);
void ryu_set_self(const char *addr);

// Asks every server for its lobbies (a ScanFilter, 0x60 bytes). infos[i] = lobby, server_of[i] = the server's index.
// Returns how many. Blocks up to a few seconds per unreachable server: call it from the scan, which waits anyway.
int ryu_scan(const uint8_t *filter, uint8_t (*infos)[RYU_NETINFO], int *server_of, int max);

// Creates a lobby on server (index, or -1: ini RyuHost) / joins one. Return 0 or an nn::ldn result. On success
// ip / mask are this node's address and subnet mask (host order), and info the lobby as the server sees it.
uint32_t ryu_create(int server, const uint8_t *net_cfg, const uint8_t *sec_cfg, const uint8_t *user,
                    const void *adv, uint32_t adv_size, uint8_t *info, uint32_t *ip, uint32_t *mask);
uint32_t ryu_connect(int server, const uint8_t *info, const uint8_t *sec_cfg, const uint8_t *user, uint16_t lcv,
                     uint8_t *info_out, uint32_t *ip, uint32_t *mask);
void ryu_leave(void);  // leaves / closes the lobby and drops the server connection
bool ryu_in_lobby(void);
void ryu_set_advertise(const void *data, uint32_t len);
void ryu_set_accept_policy(uint8_t policy);
bool ryu_reject(uint32_t node_id, uint32_t reason);
void ryu_send(const RyuProxy *p, const void *data, uint32_t len);

// Peers: more clients of the server we host on (ini RyuHost), each a player who is not in this process: the room players
// bridged into a hub on that server (xbridge.c). A peer joins the lobby described by netinfo as its own player (own address),
// its hooks are called from its reader thread. result: 0 or an nn::ldn result.
typedef struct RyuPeer RyuPeer;
typedef struct {
    void (*lobby)(void *ud, const uint8_t *info);  // info NULL: the lobby or the connection is gone
    void (*data)(void *ud, const RyuProxy *p, const void *data, uint32_t len);
} RyuPeerHooks;
RyuPeer *ryu_peer_join(const uint8_t *netinfo, const uint8_t *sec_cfg, const uint8_t *user, uint16_t lcv, const RyuPeerHooks *hooks,
                       void *ud, uint8_t *info_out, uint32_t *ip, uint32_t *mask, uint32_t *result);
void ryu_peer_send(RyuPeer *peer, const RyuProxy *p, const void *data, uint32_t len);
void ryu_peer_close(RyuPeer *peer);

#ifdef __cplusplus
}
#endif
