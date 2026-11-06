// Host-only test of runtime/net/room.c against a small stand-in for a suyu room server (this file's "server" mode),
// written from suyu's room protocol: join, fake addresses, relaying of LDN and proxy packets. No game needed.
//   test_room server <port>   |   test_room a <host:port>   |   test_room b <host:port>
// tests/run_room_test.sh runs all three. Exit code 0 = passed.
#include <enet/enet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../runtime/config.h"
#include "../runtime/hle.h"
#include "../runtime/net/room.h"
#include "../runtime/net/roomsrv.h"

Config g_config;
uint64_t plat_now_ns(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) { while (plat_now_ns() < t) {} }

// ---- the stand-in server
static int run_server(int port) {
    enet_initialize();
    ENetAddress a = {ENET_HOST_ANY, (enet_uint16)port};
    ENetHost *h = enet_host_create(&a, 8, 1, 0, 0);
    if (!h) return 2;
    struct { ENetPeer *peer; uint8_t ip[4]; bool used; } m[8] = {0};
    printf("[server] up\n");
    fflush(stdout);
    uint64_t end = plat_now_ns() + 30ull * 1000000000ull;
    while (plat_now_ns() < end) {
        ENetEvent e;
        if (enet_host_service(h, &e, 20) <= 0) continue;
        if (e.type == ENET_EVENT_TYPE_DISCONNECT) {
            for (int i = 0; i < 8; i++) if (m[i].peer == e.peer) m[i].used = false;
        } else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
            const uint8_t *d = e.packet->data;
            uint32_t n = (uint32_t)e.packet->dataLength;
            if (d[0] == 1) {  // join request: nickname, preferred ip, version, password, token
                uint32_t l = n > 5 ? (uint32_t)d[1] << 24 | d[2] << 16 | d[3] << 8 | d[4] : 0;
                int i = 0;
                while (i < 8 && m[i].used) i++;
                m[i].used = true, m[i].peer = e.peer;
                m[i].ip[0] = 192, m[i].ip[1] = 168, m[i].ip[2] = 1, m[i].ip[3] = (uint8_t)(1 + i);
                uint8_t r[5] = {2, 192, 168, 1, (uint8_t)(1 + i)};
                enet_peer_send(e.peer, 0, enet_packet_create(r, 5, ENET_PACKET_FLAG_RELIABLE));
                printf("[server] join '%.*s' -> 192.168.1.%d\n", (int)(l < 32 ? l : 32), d + 5, 1 + i);
                fflush(stdout);
            } else if (d[0] == 6 || d[0] == 5) {  // LDN / proxy packet: relay by the broadcast flag and remote ip
                bool bc;
                const uint8_t *rip;
                if (d[0] == 6) rip = d + 6, bc = d[10];  // type, local ip, remote ip, broadcast
                else rip = d + 8, bc = d[14];            // family, ip, port, family, ip, port, protocol, broadcast
                for (int i = 0; i < 8; i++) {
                    if (!m[i].used || m[i].peer == e.peer) continue;
                    if (bc || !memcmp(m[i].ip, rip, 4))
                        enet_peer_send(m[i].peer, 0, enet_packet_create(d, n, ENET_PACKET_FLAG_RELIABLE));
                }
                enet_host_flush(h);
            }
            enet_packet_destroy(e.packet);
        }
    }
    return 0;
}

// ---- clients
static volatile int got_scan, got_resp, got_proxy;
static uint8_t last_ip[4], peer_ip[4];
static char last_msg[32];
static void on_ldn(uint8_t type, const uint8_t lip[4], const uint8_t rip[4], bool bc, const void *d, uint32_t n) {
    (void)rip; (void)bc;
    memcpy(last_ip, lip, 4);
    if (type == RLDN_SCAN) got_scan++;
    if (type == RLDN_SCAN_RESP && n == 6 && !memcmp(d, "NETINF", 6)) memcpy(peer_ip, lip, 4), got_resp++;
}
static void on_proxy(const uint8_t sip[4], uint16_t sp, const uint8_t dip[4], uint16_t dp, uint8_t proto, bool bc, const void *d,
                     uint32_t n) {
    (void)dip; (void)bc;
    memcpy(peer_ip, sip, 4);
    snprintf(last_msg, sizeof last_msg, "%.*s", n > 31 ? 31 : (int)n, (const char *)d);
    last_ip[0] = (uint8_t)sp, last_ip[1] = (uint8_t)dp, last_ip[2] = proto;
    got_proxy++;
}
static bool wait_for(volatile int *v, int sec) {
    for (int i = 0; i < sec * 100 && !*v; i++) plat_sleep_until_ns(plat_now_ns() + 10000000);
    return *v;
}
#define CHECK(c, what) do { if (!(c)) { printf("[test] FAIL: %s\n", what); return 1; } printf("[test] ok: %s\n", what); } while (0)

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "server")) return run_server(atoi(argv[2]));
    if (!strcmp(argv[1], "roomsrv")) {  // the game's own room (runtime/net/roomsrv.c) in the stand-in's place
        char e[64];
        if (!roomsrv_start(atoi(argv[2]), "test", "", "", e, sizeof e)) return 2;
        printf("[server] up\n");
        fflush(stdout);
        plat_sleep_until_ns(plat_now_ns() + 12000000000ull);
        roomsrv_stop();
        return 0;
    }
    bool a = !strcmp(argv[1], "a");
    snprintf(g_config.ldn_room_server, sizeof g_config.ldn_room_server, "%s", argv[2]);
    room_set_hooks(&(RoomHooks){on_ldn, on_proxy, NULL});
    CHECK(room_enabled(), "room configured");
    uint8_t ip[4], none[4] = {255, 255, 255, 255};
    char err[96] = "";
    CHECK(room_join(argv[2], a ? "PlayerA" : "PlayerB", "", ip, err, sizeof err), "joined the room");
    CHECK((ip[0] == 192 && ip[1] == 168) || (ip[0] == 10 && ip[1] == 255), "a fake address (192.168.x.y or 10.255.x.y) from the room");
    if (a) {  // answers a scan, then echoes a game packet
        CHECK(wait_for(&got_scan, 15), "scan broadcast from b");
        uint8_t target[4];
        memcpy(target, last_ip, 4);
        room_send_ldn(RLDN_SCAN_RESP, ip, target, false, "NETINF", 6);
        CHECK(wait_for(&got_proxy, 15), "game packet from b");
        CHECK(!strcmp(last_msg, "ping") && last_ip[0] == 0x39 && last_ip[1] == 0x39 && last_ip[2] == 4, "packet ports and protocol (UDP)");
        uint8_t b[4];
        memcpy(b, peer_ip, 4);
        room_send_proxy(ip, 0x3939, b, 0x3939, 4, false, "pong", 4);
        plat_sleep_until_ns(plat_now_ns() + 500000000ull);
    } else {
        plat_sleep_until_ns(plat_now_ns() + 1000000000ull);  // (a is in the room first)
        room_send_ldn(RLDN_SCAN, ip, none, true, NULL, 0);
        CHECK(wait_for(&got_resp, 10), "scan answer with the network info");
        uint8_t dst[4];
        memcpy(dst, peer_ip, 4);
        room_send_proxy(ip, 0x3939, dst, 0x3939, 4, false, "ping", 4);
        got_proxy = 0;
        CHECK(wait_for(&got_proxy, 10) && !strcmp(last_msg, "pong"), "answer from a");
    }
    room_leave();
    CHECK(!room_connected(), "left the room");
    return 0;
}
