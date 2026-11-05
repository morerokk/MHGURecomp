// A suyu-family emulator's multiplayer room (suyu, yuzu and the forks that kept its room server): an ENet connection to
// a room server, which hands each member a fake address (192.168.x.y) and relays the members' LDN packets (the
// emulators' stand-in for the Switch's local wireless: ldn.c "room" mode implements nn::ldn on top of them) and game
// packets. Only what a member does is here: join, leave, send, receive.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the emulators call LDNPacketType.
enum { RLDN_SCAN, RLDN_SCAN_RESP, RLDN_CONNECT, RLDN_SYNC_NETWORK, RLDN_DISCONNECT, RLDN_DESTROY_NETWORK };

// Called from the room's thread. ldn: one of the LDN packets (addresses as bytes, network order); proxy: a game packet
// (proto 3 TCP, 4 UDP); lost: the room connection ended.
typedef struct {
    void (*ldn)(uint8_t type, const uint8_t local_ip[4], const uint8_t remote_ip[4], bool broadcast, const void *data,
                uint32_t len);
    void (*proxy)(const uint8_t src_ip[4], uint16_t src_port, const uint8_t dst_ip[4], uint16_t dst_port, uint8_t proto,
                  bool broadcast, const void *data, uint32_t len);
    void (*lost)(void);
} RoomHooks;
void room_set_hooks(const RoomHooks *h);

bool room_enabled(void);    // ini [Multiplayer] RoomServer is set
bool room_connected(void);  // joined (stays so until it is lost or room_leave)
// Joins the room at server (host[:port], default 24872). Blocks up to a few seconds. On success fake_ip is this
// member's address. Error text in err (may be NULL).
bool room_join(const char *server, const char *nick, const char *password, uint8_t fake_ip[4], char *err, int err_size);
void room_leave(void);
void room_own_ip(uint8_t ip[4]);
void room_send_ldn(uint8_t type, const uint8_t local_ip[4], const uint8_t remote_ip[4], bool broadcast, const void *data,
                   uint32_t len);
void room_send_proxy(const uint8_t src_ip[4], uint16_t src_port, const uint8_t dst_ip[4], uint16_t dst_port,
                     uint8_t proto, bool broadcast, const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif
