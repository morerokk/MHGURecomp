// Discord Rich Presence over Discord's local RPC: the Discord app listens on a named pipe (Windows,
// \\.\pipe\discord-ipc-0..9) or a Unix socket (Linux, discord-ipc-0..9 in $XDG_RUNTIME_DIR, also where the Flatpak
// and Snap packages put it). Frames: u32 opcode, u32 length (little endian), JSON. We say HANDSHAKE {v: 1,
// client_id}, Discord answers READY; then FRAME {cmd: SET_ACTIVITY, args: {pid, activity}} sets what our profile
// shows, and closing the connection clears it. Discord takes about 5 updates per 20 s, so a change waits until 4 s
// after the last one (the latest state goes then, the ones in between are dropped).
// No Discord running, or ini [Discord] RichPresence = Off: nothing happens; a connection is tried every 15 s.
// The application (its name is the profile's title, and where uploaded images would live) is the project's own,
// made in the Discord Developer Portal.
#include "discord.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "../config.h"
#include "../platform.h"
#include "../hle.h"

#define CLIENT_ID     "1566091085493571724"  // Discord application "Monster Hunter GU Recomp"
#define RETRY_NS      15000000000ull
#define MIN_GAP_NS    4000000000ull
#define FRAME_MAX     0x10000u
enum { OP_HANDSHAKE, OP_FRAME, OP_CLOSE, OP_PING, OP_PONG };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static char g_want[2048];  // the activity's JSON ("null": none)
static bool g_started;

// ---- the connection
#ifdef _WIN32
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static bool conn_open(void) {
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        g_pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE) return true;
    }
    return false;
}
static void conn_close(void) {
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
}
static bool conn_write(const void *p, uint32_t n) {
    DWORD w;
    return WriteFile(g_pipe, p, n, &w, NULL) && w == n;
}
static int conn_available(void) {  // bytes waiting, -1: closed
    DWORD n = 0;
    return PeekNamedPipe(g_pipe, NULL, 0, NULL, &n, NULL) ? (int)n : -1;
}
static bool conn_read(void *p, uint32_t n) {
    DWORD r;
    for (uint8_t *b = p; n; b += r, n -= r)
        if (!ReadFile(g_pipe, b, n, &r, NULL) || !r) return false;
    return true;
}
#else
static int g_sock = -1;
static bool conn_open(void) {
    const char *dirs[] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp"};
    const char *subs[] = {"", "/app/com.discordapp.Discord", "/snap.discord", "/.flatpak/com.discordapp.Discord/xdg-run"};
    for (size_t d = 0; d < sizeof dirs / sizeof *dirs; d++)
        for (size_t s = 0; dirs[d] && s < sizeof subs / sizeof *subs; s++)
            for (int i = 0; i < 10; i++) {
                struct sockaddr_un a = {.sun_family = AF_UNIX};
                if (snprintf(a.sun_path, sizeof a.sun_path, "%s%s/discord-ipc-%d", dirs[d], subs[s], i) >=
                    (int)sizeof a.sun_path)
                    continue;
                int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (fd < 0) return false;
                if (!connect(fd, (struct sockaddr *)&a, sizeof a)) { g_sock = fd; return true; }
                close(fd);
            }
    return false;
}
static void conn_close(void) {
    if (g_sock >= 0) close(g_sock);
    g_sock = -1;
}
static bool conn_write(const void *p, uint32_t n) {
    for (const uint8_t *b = p; n;) {
        ssize_t w = send(g_sock, b, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return false; }
        b += w, n -= (uint32_t)w;
    }
    return true;
}
static int conn_available(void) {
    struct pollfd p = {g_sock, POLLIN, 0};
    int r = poll(&p, 1, 0);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (!r) return 0;
    if (p.revents & POLLIN) return 1;  // (at least a byte; a closed socket reads 0)
    return -1;
}
static bool conn_read(void *p, uint32_t n) {
    for (uint8_t *b = p; n;) {
        ssize_t r = recv(g_sock, b, n, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r, n -= (uint32_t)r;
    }
    return true;
}
#endif

static bool g_connected;

static bool frame_send(uint32_t op, const char *json) {
    uint32_t n = (uint32_t)strlen(json), h[2] = {op, n};  // (little endian hosts only)
    return conn_write(h, sizeof h) && conn_write(json, n);
}

// A frame from Discord into buf (its JSON, cut to the buffer); false: the connection is gone.
static bool frame_recv(uint32_t *op, char *buf, size_t size) {
    uint32_t h[2];
    if (!conn_read(h, sizeof h) || h[1] > FRAME_MAX) return false;
    static char body[FRAME_MAX + 1];
    if (!conn_read(body, h[1])) return false;
    body[h[1]] = 0;
    *op = h[0];
    snprintf(buf, size, "%s", body);
    return true;
}

static void disconnect(const char *why) {
    if (g_connected) LOG("[discord] disconnected: %s\n", why);
    conn_close();
    g_connected = false;
}

// Waits up to ms for the first frame (the handshake's answer).
static bool wait_frame(int ms) {
    for (uint64_t end = plat_now_ns() + (uint64_t)ms * 1000000u; plat_now_ns() < end;) {
        int n = conn_available();
        if (n < 0) return false;
        if (n > 0) return true;
        plat_sleep_until_ns(plat_now_ns() + 20000000u);
    }
    return false;
}

static bool connect_discord(void) {
    if (!conn_open()) return false;
    char buf[512] = "";
    uint32_t op = 0;
    if (!frame_send(OP_HANDSHAKE, "{\"v\":1,\"client_id\":\"" CLIENT_ID "\"}") || !wait_frame(5000) ||
        !frame_recv(&op, buf, sizeof buf) || op != OP_FRAME || !strstr(buf, "\"READY\"")) {
        LOG("[discord] Discord did not accept the connection: %.200s\n", op == OP_CLOSE ? buf : "(no answer)");
        conn_close();
        return false;
    }
    g_connected = true;
    LOG("[discord] connected\n");
    return true;
}

// Discord's frames since the last look: pings answered, errors logged. false: the connection is gone.
static bool drain(void) {
    for (;;) {
        int n = conn_available();
        if (n < 0) return false;
        if (n == 0) return true;
        char buf[1024];
        uint32_t op;
        if (!frame_recv(&op, buf, sizeof buf)) return false;
        if (op == OP_PING && !frame_send(OP_PONG, buf)) return false;
        if (op == OP_CLOSE) { LOG("[discord] closed by Discord: %.200s\n", buf); return false; }
        if (op == OP_FRAME && strstr(buf, "\"ERROR\"")) LOG("[discord] %.300s\n", buf);
    }
}

static void *discord_thread(void *arg) {
    (void)arg;
    char sent[sizeof g_want] = "", want[sizeof g_want];
    uint64_t next_try = 0, last_sent = 0;
    unsigned nonce = 0;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        memcpy(want, g_want, sizeof want);
        pthread_mutex_unlock(&g_mu);

        uint64_t now = plat_now_ns();
        if (!config_live_discord()) {
            if (g_connected) disconnect("turned off");
            *sent = 0;
            continue;
        }
        if (!g_connected) {
            if (now < next_try) continue;
            next_try = now + RETRY_NS;
            if (!connect_discord()) continue;
            *sent = 0;
        }
        if (!drain()) { disconnect("connection lost"); continue; }
        if (!strcmp(want, sent) || now - last_sent < MIN_GAP_NS) continue;
        static char msg[sizeof g_want + 128];
        snprintf(msg, sizeof msg, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":%s},\"nonce\":\"%u\"}",
                 plat_process_id(), want, ++nonce);
        if (!frame_send(OP_FRAME, msg)) { disconnect("connection lost"); continue; }
        memcpy(sent, want, sizeof sent);
        last_sent = now;
        if (ENV("MHGU_LOG_DISCORD")) LOG("[discord] %s\n", want);
    }
    return NULL;
}

void discord_init(void) {
    pthread_mutex_lock(&g_mu);
    snprintf(g_want, sizeof g_want, "null");
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    if (!pthread_create(&th, NULL, discord_thread, NULL)) pthread_detach(th), g_started = true;
}

// ---- the activity as JSON
static void put(char **p, char *end, const char *s) {
    size_t n = strlen(s);
    if (n >= (size_t)(end - *p)) n = (size_t)(end - *p) - 1;
    memcpy(*p, s, n), *p += n, **p = 0;
}

static void put_string(char **p, char *end, const char *key, const char *s, bool *comma) {
    if (!*s) return;
    char buf[300], *o = buf;
    for (; *s && o < buf + sizeof buf - 8; s++) {  // (UTF-8 passes through; quotes, backslashes, controls escaped)
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') *o++ = '\\', *o++ = (char)ch;
        else if (ch < 0x20) o += snprintf(o, 7, "\\u%04x", ch);
        else *o++ = (char)ch;
    }
    *o = 0;
    put(p, end, *comma ? ",\"" : "\""), put(p, end, key), put(p, end, "\":\""), put(p, end, buf), put(p, end, "\"");
    *comma = true;
}

void discord_set_activity(const DiscordActivity *a) {
    char json[sizeof g_want], *p = json, *end = json + sizeof json;
    *p = 0;
    if (!a) {
        put(&p, end, "null");
    } else {
        bool c = false, ca = false;
        put(&p, end, "{");
        put_string(&p, end, "details", a->details, &c);
        put_string(&p, end, "state", a->state, &c);
        if (a->start) {
            char t[64];
            snprintf(t, sizeof t, "%s\"timestamps\":{\"start\":%lld}", c ? "," : "", (long long)a->start);
            put(&p, end, t), c = true;
        }
        char assets[700], *q = assets;
        *q = 0;
        put_string(&q, assets + sizeof assets, "large_image", a->large_image, &ca);
        put_string(&q, assets + sizeof assets, "large_text", a->large_text, &ca);
        put_string(&q, assets + sizeof assets, "small_image", a->small_image, &ca);
        put_string(&q, assets + sizeof assets, "small_text", a->small_text, &ca);
        if (ca) put(&p, end, c ? ",\"assets\":{" : "\"assets\":{"), put(&p, end, assets), put(&p, end, "}"), c = true;
        if (a->party > 0 && a->party_max >= a->party) {
            bool cp = false;
            put(&p, end, c ? ",\"party\":{" : "\"party\":{");
            put_string(&p, end, "id", *a->party_id ? a->party_id : "solo", &cp);
            char t[48];
            snprintf(t, sizeof t, ",\"size\":[%d,%d]}", a->party, a->party_max);
            put(&p, end, t), c = true;
        }
        put(&p, end, "}");
    }
    pthread_mutex_lock(&g_mu);
    bool changed = strcmp(json, g_want) != 0;
    if (changed) memcpy(g_want, json, sizeof g_want);
    pthread_mutex_unlock(&g_mu);
    if (changed && g_started) pthread_cond_signal(&g_cv);
}
