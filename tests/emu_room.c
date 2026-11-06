// An emulator player in a suyu room, as far as LDN goes (their LANDiscovery over the room's packets), for the room bridge tests.
//   emu_room host <room>   hosts a hub (answers Scan, takes a Connect, answers a "ping" packet with "pong")
//   emu_room join <room>   searches the room, joins the hub, sends "ping", waits for "pong"
// Exit code 0 = passed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "../runtime/config.h"
#include "../runtime/hle.h"
#include "../runtime/net/room.h"

Config g_config;
uint64_t plat_now_ns(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) { while (plat_now_ns() < t) {} }
#define xp_sleep_ms(ms) plat_sleep_until_ns(plat_now_ns() + (uint64_t)(ms) * 1000000ull)

static uint8_t info[0x480];
static uint8_t own[4];
static volatile int got_scan_resp, got_sync, got_ping, got_pong, joined;
static uint8_t peer[4];
static uint8_t host_info[0x480];

static void ldn(uint8_t type, const uint8_t lip[4], const uint8_t rip[4], bool bc, const void *d, uint32_t n) {
    (void)rip, (void)bc;
    if (type == RLDN_SCAN && info[0x67]) room_send_ldn(RLDN_SCAN_RESP, own, lip, false, info, sizeof info);
    if (type == RLDN_CONNECT && n >= 0x40) {  // a station: its NodeInfo into slot 1, everyone gets the NetworkInfo
        memcpy(info + 0x68 + 0x40, d, 0x40);
        info[0x68 + 0x40 + 0x0A] = 1, info[0x68 + 0x40 + 0x0B] = 1;
        info[0x67] = 2;
        memcpy(peer, lip, 4);
        joined = 1;
        room_send_ldn(RLDN_SYNC_NETWORK, own, lip, false, info, sizeof info);
        printf("[emu] a station joined from %u.%u.%u.%u\n", lip[0], lip[1], lip[2], lip[3]);
        fflush(stdout);
    }
    if (type == RLDN_SCAN_RESP && n >= sizeof host_info && !got_scan_resp) memcpy(host_info, d, sizeof host_info), got_scan_resp = 1;
    if (type == RLDN_SYNC_NETWORK && n >= sizeof info) got_sync = 1;
}
static void proxy(const uint8_t sip[4], uint16_t sp, const uint8_t dip[4], uint16_t dp, uint8_t proto, bool bc, const void *d, uint32_t n) {
    (void)dip, (void)bc, (void)dp;
    if (proto == 4 && n == 4 && !memcmp(d, "ping", 4)) {
        got_ping = 1;
        printf("[emu] got a ping\n");
        fflush(stdout);
        room_send_proxy(own, 4242, sip, sp, 4, false, "pong", 4);
    }
    if (proto == 4 && n == 4 && !memcmp(d, "pong", 4)) got_pong = 1;
}
static bool wait(volatile int *v, int sec) {
    for (int i = 0; i < sec * 100 && !*v; i++) xp_sleep_ms(10);
    return *v;
}
#define CHECK(c, what) do { if (!(c)) { printf("[emu] FAIL: %s\n", what); return 1; } printf("[emu] ok: %s\n", what); } while (0)

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    snprintf(g_config.ldn_room_server, sizeof g_config.ldn_room_server, "%s", argv[2]);
    room_set_hooks(&(RoomHooks){ldn, proxy, NULL});
    char err[96];
    bool host = !strcmp(argv[1], "host");
    CHECK(room_join(argv[2], host ? "EmuHost" : "EmuJoin", "", own, err, sizeof err), "joined the room");
    uint32_t ip = (uint32_t)own[0] << 24 | own[1] << 16 | own[2] << 8 | own[3];
    uint8_t mac[6] = {2, 0, own[0], own[1], own[2], own[3]};
    if (host) {
        uint64_t id = 0x0100770008DD8000ull;
        memcpy(info, &id, 8);
        memset(info + 0x10, 0xEE, 16);
        memcpy(info + 0x20, mac, 6);
        info[0x4A] = 3, info[0x4B] = 2, info[0x66] = 4, info[0x67] = 1;
        uint8_t *nd = info + 0x68;
        memcpy(nd, &ip, 4), memcpy(nd + 4, mac, 6), nd[0x0B] = 1;
        memcpy(nd + 0x0C, "EmuHost", 7);
        nd[0x2E] = 1;
        info[0x26A] = 4;
        memcpy(info + 0x26C, "EMUH", 4);
        printf("[emu] hosting\n");
        fflush(stdout);
        CHECK(wait(&got_ping, 40), "a ping from the station");
        xp_sleep_ms(500);
    } else {
        uint8_t all[4] = {255, 255, 255, 255};
        for (int t = 0; t < 20 && !got_scan_resp; t++) { room_send_ldn(RLDN_SCAN, own, all, true, NULL, 0); xp_sleep_ms(500); }
        CHECK(got_scan_resp, "found the hub");
        CHECK(!memcmp(host_info + 0x26C, "ADV!", 4) || !memcmp(host_info + 0x26C, "HOST", 4) || !memcmp(host_info + 0x26C, "EMUH", 4), "its advertise data");
        uint8_t hip[4];
        uint32_t hv = *(uint32_t *)(host_info + 0x68);
        hip[0] = (uint8_t)(hv >> 24), hip[1] = (uint8_t)(hv >> 16), hip[2] = (uint8_t)(hv >> 8), hip[3] = (uint8_t)hv;
        uint8_t node[0x40] = {0};
        memcpy(node, &ip, 4), memcpy(node + 4, mac, 6), node[0x0B] = 1;
        memcpy(node + 0x0C, "EmuJoin", 7);
        node[0x2E] = 1;
        room_send_ldn(RLDN_CONNECT, own, hip, false, node, sizeof node);
        CHECK(wait(&got_sync, 10), "the host synced the network");
        xp_sleep_ms(500);
        room_send_proxy(own, 12345, hip, 12345, 4, false, "ping", 4);
        CHECK(wait(&got_pong, 10), "pong from the host");
        if (getenv("EMU_HOLD")) xp_sleep_ms(atoi(getenv("EMU_HOLD")) * 1000);  // (stay in the hub a while: others join meanwhile)
    }
    room_leave();
    return 0;
}
