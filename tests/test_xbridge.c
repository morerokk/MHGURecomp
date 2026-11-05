// Host-only test of mixed hubs (runtime/net/xbridge.c): this process plays the game that hosts a hub on a Ryujinx LDN
// server and sits in a suyu-family room at the same time. A Ryujinx-side player (test_ryuldn join) and a room player
// (emu_room join) both join the hub; each sends "ping" to the host, which answers "pong". The host also checks that its
// hub shows all three players. No game needed.
//   test_xbridge <ryu server> <room server> <seconds>
// Exit code 0 = passed. tests/run_xbridge_test.sh runs all three programs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/config.h"
#include "../runtime/hle.h"
#include "../runtime/net/room.h"
#include "../runtime/net/ryuldn.h"
#include "../runtime/net/xbridge.h"

Config g_config;
uint64_t plat_now_ns(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) { while (plat_now_ns() < t) {} }

static volatile int pings, max_players;
static void on_lobby(const uint8_t *info) {
    if (info && info[0x67] > max_players) max_players = info[0x67];
    if (xb_active()) xb_lobby(info);
}
static void on_data(const RyuProxy *p, const void *d, uint32_t n) {
    if (n != 4 || memcmp(d, "ping", 4)) return;
    pings++;
    if ((p->src_ip >> 8) == 0x0A7200 && (p->src_ip & 255) >= 2) printf("[host] ping from 10.114.0.%u\n", p->src_ip & 255);
    RyuProxy r = {p->dst_ip, p->src_ip, p->dst_port, p->src_port, 17};
    ryu_send(&r, "pong", 4);
}
static void on_room_ldn(uint8_t t, const uint8_t l[4], const uint8_t r[4], bool bc, const void *d, uint32_t n) { if (xb_active()) xb_room_ldn(t, l, r, bc, d, n); }
static void on_room_proxy(const uint8_t s[4], uint16_t sp, const uint8_t d[4], uint16_t dp, uint8_t pr, bool bc, const void *data, uint32_t n) {
    if (xb_active()) xb_room_proxy(s, sp, d, dp, pr, bc, data, n);
}
#define CHECK(c, what) do { if (!(c)) { printf("[host] FAIL: %s\n", what); fflush(stdout); return 1; } printf("[host] ok: %s\n", what); fflush(stdout); } while (0)

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    snprintf(g_config.ldn_ryu_servers, sizeof g_config.ldn_ryu_servers, "%s", argv[1]);
    snprintf(g_config.ldn_ryu_host, sizeof g_config.ldn_ryu_host, "%s", argv[1]);
    snprintf(g_config.ldn_room_server, sizeof g_config.ldn_room_server, "%s", argv[2]);
    g_config.ldn_room_host = 1;
    int seconds = atoi(argv[3]);
    ryu_set_hooks(&(RyuHooks){on_lobby, on_data});
    room_set_hooks(&(RoomHooks){on_room_ldn, on_room_proxy, NULL});
    uint8_t info[RYU_NETINFO], sec[0x44] = {0}, user[0x30] = {0}, cfg[0x20] = {0}, rip[4];
    uint32_t ip = 0, mask = 0;
    memcpy(user, "GameHost", 8);
    cfg[0x12] = 4, cfg[0x14] = 1;
    memcpy(cfg, "\x00\x80\xDD\x08\x00\x77\x00\x01", 8);
    char err[96];
    CHECK(room_join(argv[2], "BridgeHost", "", rip, err, sizeof err), "joined the room");
    CHECK(ryu_create(-1, cfg, sec, user, "ADV!", 4, info, &ip, &mask) == 0, "hub created on the server");
    xb_begin(info, ip, mask);
    printf("[host] hub up\n");
    fflush(stdout);
    uint64_t end = plat_now_ns() + (uint64_t)seconds * 1000000000ull;
    while (plat_now_ns() < end && (max_players < 3 || pings < 2)) plat_sleep_until_ns(plat_now_ns() + 20000000);
    plat_sleep_until_ns(plat_now_ns() + 1500000000ull);  // (let the answers get out)
    CHECK(max_players >= 3, "the hub showed host + Ryujinx player + room player");
    CHECK(pings >= 2, "both players' packets arrived (translated to server addresses)");
    CHECK(xb_players() == 1, "one room player bridged");
    xb_end();
    ryu_leave();
    room_leave();
    return 0;
}
