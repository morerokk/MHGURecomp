// time:u / time:a / time:s
#include <string.h>
#include <time.h>

#include "../hle.h"
#include "../ipc.h"

static const uint8_t clock_uuid[16] = {0x4d, 0x48, 0x47, 0x55, 0x52, 0x65, 0x63, 0x6f, 1, 2, 3, 4, 5, 6, 7, 8};

static int64_t steady_seconds(void) { return (int64_t)(rt_ticks() / 19200000ull); }
static int64_t unix_now(void) { return (int64_t)time(NULL); }

static void write_system_context(uint8_t *p) {  // SystemClockContext (0x20)
    int64_t steady = steady_seconds(), off = unix_now() - steady;
    memcpy(p, &off, 8);
    memcpy(p + 8, &steady, 8);
    memcpy(p + 0x10, clock_uuid, 16);
}

static void time_shmem_map(KSharedMem *s) {
    uint8_t *m = G2H(s->guest_addr);
    memset(m, 0, 0xD0);
    memcpy(m + 0x10, clock_uuid, 16);   // steady clock context
    write_system_context(m + 0x40);     // local system clock
    write_system_context(m + 0x88);     // network system clock
}

static void system_clock(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(int64_t, 0) = unix_now(); resp_size(resp, 8); break;
    case 2: write_system_context(resp->data); resp_size(resp, 0x20); break;
    default: LOG("[time] ISystemClock cmd %u (ok)\n", req->cmd);
    }
}

static void steady_clock(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:
        OUT(int64_t, 0) = steady_seconds();
        memcpy(resp->data + 8, clock_uuid, 16);
        resp_size(resp, 0x18);
        break;
    default: OUT(int64_t, 0) = 0; resp_size(resp, 8); LOG("[time] ISteadyClock cmd %u (ok)\n", req->cmd);
    }
}

static void to_calendar(int64_t t, IpcResp *resp, int utc) {
    time_t tt = (time_t)t;
    struct tm tm, g;
    if (utc) gmtime_r(&tt, &tm); else localtime_r(&tt, &tm);
    gmtime_r(&tt, &g);
    uint8_t *o = resp->data;
    uint16_t year = (uint16_t)(tm.tm_year + 1900);
    memcpy(o, &year, 2);
    o[2] = (uint8_t)(tm.tm_mon + 1); o[3] = (uint8_t)tm.tm_mday;
    o[4] = (uint8_t)tm.tm_hour; o[5] = (uint8_t)tm.tm_min; o[6] = (uint8_t)tm.tm_sec;
    uint32_t wday = (uint32_t)tm.tm_wday, yday = (uint32_t)tm.tm_yday, isdst = (uint32_t)(tm.tm_isdst > 0);
    memcpy(o + 8, &wday, 4);
    memcpy(o + 12, &yday, 4);
#ifdef _WIN32
    int32_t gmtoff = utc ? 0 : (int32_t)difftime(_mkgmtime(&tm), _mkgmtime(&g));
#else
    int32_t gmtoff = utc ? 0 : (int32_t)tm.tm_gmtoff;
#endif
    memcpy(o + 16, utc ? "UTC" : "LCL", 4);
    memcpy(o + 24, &isdst, 4);
    memcpy(o + 28, &gmtoff, 4);
    resp_size(resp, 0x20);
}

static void timezone_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: memcpy(resp->data, "UTC", 4); resp_size(resp, 0x24); break;
    case 100: to_calendar(IN(int64_t, 0), resp, 1); break;
    case 101: to_calendar(IN(int64_t, 0), resp, 0); break;
    case 201: {  // ToPosixTimeWithMyRule
        const uint8_t *in = req->data;
        struct tm tm = {0};
        uint16_t year;
        memcpy(&year, in, 2);
        tm.tm_year = year - 1900; tm.tm_mon = in[2] - 1; tm.tm_mday = in[3];
        tm.tm_hour = in[4]; tm.tm_min = in[5]; tm.tm_sec = in[6]; tm.tm_isdst = -1;
        int64_t t = mktime(&tm);
        IpcBuf *b = out_buf(req, 0);
        if (b) memcpy(BUFP(b), &t, 8);
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    default: {  // (cmd 4 LoadTimeZoneRule: a plugin can ask every frame)
        static uint32_t logged[8];
        if (req->cmd >= 256 || !(logged[req->cmd / 32] & 1u << (req->cmd % 32))) {
            if (req->cmd < 256) logged[req->cmd / 32] |= 1u << (req->cmd % 32);
            LOG("[time] ITimeZoneService cmd %u (ok; logged once)\n", req->cmd);
        }
        resp_size(resp, 0x10);
    }
    }
}

static uint32_t g_shmem_handle;

static void static_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: case 4: case 5:
        ipc_return_object(resp, service_new("ISystemClock", system_clock, NULL));
        break;
    case 2: ipc_return_object(resp, service_new("ISteadyClock", steady_clock, NULL)); break;
    case 3: ipc_return_object(resp, service_new("ITimeZoneService", timezone_service, NULL)); break;
    case 20:
        if (!g_shmem_handle) kernel_create_shared_memory(0x1000, time_shmem_map, &g_shmem_handle);
        resp->copy_handles[resp->ncopy++] = g_shmem_handle;
        break;
    case 100: case 101: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: LOG("[time] IStaticService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("time:u", time_u) { static_service(s, req, resp); }
SERVICE("time:a", time_a) { static_service(s, req, resp); }
SERVICE("time:s", time_s) { static_service(s, req, resp); }

HLE_FUNC("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_std_user_now) {
    if (ARG(0)) { int64_t t = unix_now(); memcpy(ARGP(0), &t, 8); }
    RET(0);
}
