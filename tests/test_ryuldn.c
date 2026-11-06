// Host-only test of runtime/net/ryuldn.c against tests/ryuldn_stub_server.py (or a real LdnServer): no game needed.
//   test_ryuldn host <server>   creates a lobby, waits for a packet, answers it
//   test_ryuldn join <server>   scans, joins the lobby, sends a packet to the host, waits for the answer
// Exit code 0 = passed. tests/run_ryuldn_test.sh runs both against the stub server.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../runtime/config.h"
#include "../runtime/hle.h"
#include "../runtime/net/ryuldn.h"

Config g_config;

uint64_t plat_now_ns(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) {
    while (plat_now_ns() < t) {}
}

static volatile int got_data, got_lobby;
static RyuProxy last_p;
static char last_msg[64];

static void on_lobby(const uint8_t *info) {
    got_lobby++;
    printf("[test] lobby update: %s%s\n", info ? "players " : "gone", info ? (info[0x67] == 1 ? "1" : info[0x67] == 2 ? "2" : "3+") : "");
}
static void on_data(const RyuProxy *p, const void *d, uint32_t n) {
    last_p = *p;
    snprintf(last_msg, sizeof last_msg, "%.*s", n > 63 ? 63 : (int)n, (const char *)d);
    got_data++;
}
static bool wait_data(int s) {
    for (int i = 0; i < s * 100 && !got_data; i++) plat_sleep_until_ns(plat_now_ns() + 10000000);
    return got_data;
}
#define CHECK(c, what) do { if (!(c)) { printf("[test] FAIL: %s\n", what); return 1; } printf("[test] ok: %s\n", what); } while (0)

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    bool host = !strcmp(argv[1], "host");
    snprintf(g_config.ldn_ryu_servers, sizeof g_config.ldn_ryu_servers, "%s", argv[2]);
    snprintf(g_config.ldn_ryu_host, sizeof g_config.ldn_ryu_host, "%s", argv[2]);
    ryu_set_hooks(&(RyuHooks){on_lobby, on_data});
    CHECK(ryu_server_count() == 1 && ryu_enabled(), "server configured");
    uint8_t info[RYU_NETINFO], sec[0x44] = {0}, user[0x30] = {0}, cfg[0x20] = {0};
    uint32_t ip = 0, mask = 0;
    if (host) {
        memcpy(user, "TestHost", 8);
        cfg[0x12] = 4;  // players max
        cfg[0x14] = 1;  // local communication version (a joiner may not be newer)
        memcpy(cfg, "\x00\x80\xDD\x08\x00\x77\x00\x01", 8);
        CHECK(ryu_create(-1, cfg, sec, user, "ADV!", 4, info, &ip, &mask) == 0, "lobby created");
        CHECK(ip == 0x0A720001 && mask == 0xFFFF0000, "host address 10.114.0.1/16");
        CHECK(info[0x67] == 1 && info[0x66] == 4, "NetworkInfo: 1 of 4 players");
        CHECK(!memcmp(info + 0x26C, "ADV!", 4), "advertise data in the NetworkInfo");
        printf("[test] host ready\n");
        fflush(stdout);
        CHECK(wait_data(20), "packet from the joiner");
        CHECK(last_p.dst_ip == 0x0A720001 && last_p.src_ip == 0x0A720002 && last_p.proto == 17, "packet addresses");
        CHECK(!strcmp(last_msg, "ping"), "packet payload");
        RyuProxy back = {ip, last_p.src_ip, 12345, last_p.src_port, 17};
        ryu_send(&back, "pong", 4);
        plat_sleep_until_ns(plat_now_ns() + 500000000ull);
        CHECK(got_lobby >= 1, "lobby update after the join");
        ryu_leave();
        return 0;
    }
    uint8_t filter[0x60] = {0}, infos[8][RYU_NETINFO];
    int srv[8], n = 0;
    for (int t = 0; t < 40 && n == 0; t++) {  // (the scan answers from a worker: later calls see its results)
        n = ryu_scan(filter, infos, srv, 8);
        plat_sleep_until_ns(plat_now_ns() + 200000000ull);
    }
    CHECK(n == 1 && srv[0] == 0, "scan found the lobby");
    CHECK(!memcmp(infos[0] + 0x26C, "ADV!", 4), "scan result has the advertise data");
    memcpy(user, "TestJoin", 8);
    CHECK(ryu_connect(srv[0], infos[0], sec, user, 1, info, &ip, &mask) == 0, "joined");
    CHECK(ip >= 0x0A720002 && ip < 0x0A720009, "joiner address 10.114.0.x");
    CHECK(info[0x67] >= 2, "NetworkInfo: 2 or more players");
    RyuProxy p = {ip, 0x0A720001, 12345, 12345, 17};
    ryu_send(&p, "ping", 4);
    CHECK(wait_data(10), "answer from the host");
    CHECK(!strcmp(last_msg, "pong") && last_p.src_ip == 0x0A720001, "answer payload");
    ryu_leave();
    CHECK(!ryu_in_lobby(), "left the lobby");
    return 0;
}
