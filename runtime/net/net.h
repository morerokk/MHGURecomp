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
bool ldn_crossplay(void);        // the network is a lobby on a Ryujinx LDN server: the port-only features (emsync, hpscale) are off
void ldn_log_state(void);        // the state and the game's last state-changing call, whether it returned (window close)
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

// Session codes (link version 4: the hub server lists 3 and 4 together). A hub's code is 8 characters; shown and
// typed as "ABCD-EFGH" (case, dashes and spaces don't matter, O reads as 0 and I / L as 1).
bool ldn_code_normalize(const char *typed, char *out);  // out: 9 bytes; false: not a code
bool ldn_session_code(char *out, size_t size);  // the hub we host or joined through the hub server ("ABCD-EFGH"); false: none
// Join by code: the hub server (server; NULL / "": ini Server) is asked for the hub, which then comes first in the
// game's searches for 10 minutes, so the player picks it in Join Hub. false: not a code, or no hub server.
bool ldn_join_code(const char *typed, const char *server);
enum { LDN_INVITE_NONE, LDN_INVITE_SEARCHING, LDN_INVITE_READY, LDN_INVITE_NOT_FOUND, LDN_INVITE_FAILED,
       LDN_INVITE_OLD_SERVER };
int ldn_invite_state(char *code, size_t size);  // LDN_INVITE_*; code: the last code asked for ("ABCD-EFGH")
bool ldn_join_secret(char *out, size_t size);   // Discord's join secret for our hub ("ABCD-EFGH@server")
void ldn_join_from_secret(const char *secret);  // someone's Discord "Join": only for our own hub server

// Friends (link version 4): this install's friend code and the list, in the save folder (friends-id.bin,
// friends.txt); the hub server tells each player which of the friends it lists are online and list it back.
#define LDN_FRIENDS_MAX 64
enum { LDN_FRIEND_UNKNOWN, LDN_FRIEND_OFFLINE, LDN_FRIEND_ONLINE, LDN_FRIEND_HOSTING, LDN_FRIEND_IN_HUB };
typedef struct {
    char code[24];  // "ABCD-EFGH-JKMN-PQRS"
    char name[33];
    char hub[9];    // their hub's session code ("": none known)
    int state;      // LDN_FRIEND_*
    bool mutual;    // they list you back (the hub server told so this session): a friend to the game itself too
} LdnFriend;
void ldn_friends_start(void);                    // the thread that keeps the friends' states (at start)
void ldn_my_friend_code(char *out, size_t size);  // "ABCD-EFGH-JKMN-PQRS"
int ldn_friends(LdnFriend *out, int max, int *server_state);  // a copy of the list; server_state: HUB_* of the last ask
const char *ldn_friend_add(const char *code_and_name);  // NULL: added; else why not (a text to translate)
void ldn_friend_remove(const char *code);
// Friend requests: players who added you while you haven't added them (ini FriendRequests: Off, none). Accepting adds
// them; a rejected one stays out until the game restarts, unless you add that player yourself.
typedef struct { char code[24], name[33]; } LdnRequest;
int ldn_friend_requests(LdnRequest *out, int max);
void ldn_friend_request_answer(const char *code, bool accept);
// The other hunters in your hub whose game (link version 4, FriendRequests On) shared its friend code.
enum { LDN_HUNTER_ADD, LDN_HUNTER_ADDED, LDN_HUNTER_FRIEND };  // not in your list, in it (a request), a friend
typedef struct { char code[24], name[33]; int state; } LdnHunter;
int ldn_hub_hunters(LdnHunter *out, int max);
// The game's own friends (nn::account / nn::friends, runtime/services/misc.c, hle/offline.c): this install's account
// id, made from its friend code, and the mutual friends' ids, so the game counts them as friends (a passcode for
// "everyone but friends" lets them in).
uint64_t ldn_account_id(void);
int ldn_friend_account_ids(uint64_t *out, int max);
int ldn_game_friends(uint64_t *ids, char (*names)[33], int max);  // the same, with the names in our list
void friends_aux_received(const void *data, uint32_t len);  // an F_AUX message from another game (0xE9: a friend hello)

// vsock.c
void vsock_deliver(const VPacket *p, const void *data, uint32_t len);  // from the network to the local sockets
void vsock_peer_gone(uint32_t ip);  // a node left: its TCP connections end
void vsock_network_down(void);      // the network is gone: every connection ends, queues empty

// emsync.c: small monsters synced between the players in a zone
void emsync_tick(void);             // each frame, from the game's input polling
void emsync_aux_received(const void *data, uint32_t len);  // an F_AUX message from another game

// hitsync.c: melee hits reported by the hunter who made them
void hitsync_tick(void);            // each frame, from the game's input polling
void hitsync_aux_received(const void *data, uint32_t len);  // an F_AUX message, or in crossplay a datagram to this port:
#define HITSYNC_UDP_PORT 47318      // the emulators' Lag Compensation plugin's port (crossplay)

// partsync.c: a large monster's part breaks sent when they happen, not with its next action
void partsync_aux_received(const void *data, uint32_t len);  // an F_AUX message from another game

// hle/hpscale.c: large monster HP by the number of hunters; the host's settings go to the others
void hpscale_tick(void);            // each frame, from the game's input polling
void hpscale_aux_received(const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif
