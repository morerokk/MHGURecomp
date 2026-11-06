// Mixed hubs: a hub this game hosts on a Ryujinx LDN server (ini RyuHost) that suyu-family room players (ini RoomHost) can
// join as well, so players of both emulator families share one hub with the port's player as host.
//
// The hub lives on the server. For every room player that connects, this joins the same lobby as one more RyuLdn client
// (ryuldn.c, "peers"): to the server, the host game and the Ryujinx players that is an ordinary player (10.114.x.y). Room
// players keep seeing room addresses (192.168.x.y): the host is this process's own room address, the other room players are
// themselves, a Ryujinx player gets the stand-in 192.168.250.<node> (see xb_alias), and the game's own addresses and MACs
// inside its datagrams are rewritten from one view to the other. Traffic between two room players never passes through
// here (the room delivers it itself). A room player's packet to a Ryujinx player's stand-in address only reaches this
// bridge if the room hands out addresses no member owns to a gateway; a stock room drops those, broadcasts always work.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The hub was created on the Ryujinx server (the host's address and mask there); info is the server's NetworkInfo.
void xb_begin(const uint8_t *info, uint32_t host_ip, uint32_t mask);
void xb_end(void);  // the hub is gone: room players are told, their peers closed
bool xb_active(void);
void xb_lobby(const uint8_t *info);  // the server pushed the hub's NetworkInfo (players came or went, new advertise data)

// The room's LDN packets and game packets while xb_active() (ldn.c hands them over instead of handling them).
void xb_room_ldn(uint8_t type, const uint8_t lip[4], const uint8_t rip[4], bool bc, const void *data, uint32_t len);
void xb_room_members(const uint8_t (*ips)[4], int n);  // the room's member list: players that left it are let go
void xb_room_proxy(const uint8_t sip[4], uint16_t sport, const uint8_t dip[4], uint16_t dport, uint8_t proto, bool bc,
                   const void *data, uint32_t len);

// Room players currently in the hub.
int xb_players(void);

#ifdef __cplusplus
}
#endif
