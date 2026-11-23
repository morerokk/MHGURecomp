// Small services: account, parental control, add-on content, settings, and other mostly-stubbed interfaces.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../hle/input.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ acc:u0 (one local user)
const uint8_t g_user_id[16] = {0x4d, 0x48, 0x47, 0x55, 0x52, 0x65, 0x63, 0x6f, 0x01, 0, 0, 0, 0, 0, 0, 0x01};

static void write_uid_list(IpcReq *req) {
    IpcBuf *c = out_buf(req, 0);
    if (!c) return;
    memset(BUFP(c), 0, c->size);
    if (c->size >= 16) memcpy(BUFP(c), g_user_id, 16);
}

static void profile(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: {  // Get / GetBase -> ProfileBase {uid, u64 timestamp, nickname[0x21]}
        memset(resp->data, 0, 0x38);
        memcpy(resp->data, g_user_id, 16);
        *(uint64_t *)(resp->data + 16) = 1600000000;
        const char *nick = getenv("MHGU_NICKNAME");
        snprintf((char *)resp->data + 24, 0x20, "%s", nick ? nick : "Hunter");
        resp_size(resp, 0x38);
        if (req->cmd == 0 && out_buf(req, 0)) memset(BUFP(out_buf(req, 0)), 0, out_buf(req, 0)->size);
        break;
    }
    case 10: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetImageSize
    case 11: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // LoadImage
    default: LOG("[acc] IProfile cmd %u (ok)\n", req->cmd);
    }
}

static void acc_manager(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: break;  // CheckAvailability
    // GetAccountId: the network service account id, this install's own (from its friend code, runtime/net/ldn.c),
    // so the game can tell its friends apart; every game had 0x1234567890abcdef before link version 4
    case 1: { uint64_t ldn_account_id(void); OUT(uint64_t, 0) = ldn_account_id(); resp_size(resp, 8); break; }
    default: LOG("[acc] IManagerForApplication cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("acc:u0", acc_u0) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(int32_t, 0) = 1; resp_size(resp, 4); break;  // GetUserCount
    case 1: OUT(uint8_t, 0) = !memcmp(req->data, g_user_id, 16); resp_size(resp, 1); break;  // GetUserExistence
    case 2: case 3: case 141: write_uid_list(req); break;  // ListAllUsers / ListOpenUsers / ListQualifiedUsers
    case 4: case 51: memcpy(resp->data, g_user_id, 16); resp_size(resp, 16); break;  // GetLastOpenedUser
    case 5: ipc_return_object(resp, service_new("IProfile", profile, NULL)); break;
    case 50: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 100: case 140: break;  // InitializeApplicationInfo
    case 101: ipc_return_object(resp, service_new("IManagerForApplication", acc_manager, NULL)); break;
    case 150: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: LOG("[acc] acc:u0 cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: {
        // (logged once per command: the game asks 1001, CheckFreeCommunicationPermission, many times a second online)
        static uint8_t seen[4096 / 8];
        if (req->cmd >= 4096 || !(seen[req->cmd / 8] & (1u << (req->cmd % 8)))) {
            if (req->cmd < 4096) seen[req->cmd / 8] |= (uint8_t)(1u << (req->cmd % 8));
            LOG("[pctl] IParentalControlService cmd %u (ok; logged once)\n", req->cmd);
        }
        resp_size(resp, 0x10);
    }
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ aoc:u (no DLC)
SERVICE("aoc:u", aoc_u) {
    (void)s;
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // CountAddOnContent
    case 3: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // ListAddOnContent
    case 5: OUT(uint64_t, 0) = 0x0100770008DD9000ull; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 8: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    default: ok_logger(s, req, resp);
    }
}

// ------------------------------------------------------------------ settings
static const char *const languages[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl",
                                         "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans",
                                         "zh-Hant", "pt-BR"};
#define NLANG 18

const char *settings_language(void) {
    const char *l = getenv("MHGU_LANG");
    return l && *l ? l : "en-US";
}

static uint64_t lang_code(const char *s) {
    uint64_t v = 0;
    memcpy(&v, s, strlen(s) < 8 ? strlen(s) : 8);
    return v;
}

static void language_list(IpcReq *req, IpcResp *resp, int max) {
    IpcBuf *b = out_buf(req, 0);
    int n = 0;
    if (b) {
        uint64_t *out = (uint64_t *)BUFP(b);
        for (; n < max && n < NLANG && (uint64_t)(n + 1) * 8 <= b->size; n++) out[n] = lang_code(languages[n]);
    }
    OUT(int32_t, 0) = n;
    resp_size(resp, 4);
}

SERVICE("set", set) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint64_t, 0) = lang_code(settings_language()); resp_size(resp, 8); break;  // GetLanguageCode
    case 1: language_list(req, resp, 15); break;
    case 2: {
        uint32_t i = IN(uint32_t, 0);
        OUT(uint64_t, 0) = i < NLANG ? lang_code(languages[i]) : 0;
        resp_size(resp, 8);
        break;
    }
    case 3: OUT(int32_t, 0) = 15; resp_size(resp, 4); break;
    case 4: {  // GetRegionCode: 2 Europe (MHGU_REGION: 0 Japan, 1 USA, 2 Europe, 3 Australia...)
        const char *r = getenv("MHGU_REGION");
        OUT(uint32_t, 0) = r && *r ? (uint32_t)atoi(r) : 2;
        resp_size(resp, 4);
        break;
    }
    case 5: language_list(req, resp, NLANG); break;
    case 6: OUT(int32_t, 0) = NLANG; resp_size(resp, 4); break;
    case 8: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: LOG("[set] cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("set:sys", set_sys) {
    (void)s;
    switch (req->cmd) {
    case 3: case 4: {  // GetFirmwareVersion
        IpcBuf *b = out_buf(req, 0);
        if (b) {
            uint8_t *v = BUFP(b);
            memset(v, 0, 0x100);
            v[0] = 8; v[1] = 1; v[2] = 0; v[4] = 8; v[5] = 1; v[6] = 0;
            strcpy((char *)v + 0x68, "8.1.0");
            strcpy((char *)v + 0x80, "8.1.0-1.0");
        }
        break;
    }
    case 23: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;
    default: LOG("[set:sys] cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

// ------------------------------------------------------------------ hid (input: runtime/hle/input.c)
KSharedMem *g_hid_shmem;
static uint32_t hid_shmem_handle;

static void applet_resource(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) {  // GetSharedMemoryHandle
        if (!g_hid_shmem) g_hid_shmem = kernel_create_shared_memory(0x40000, NULL, &hid_shmem_handle);
        resp->copy_handles[resp->ncopy++] = hid_shmem_handle;
    }
}

SERVICE("hid", hid) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IAppletResource", applet_resource, NULL)); break;
    case 40: case 106: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    case 101: OUT(uint32_t, 0) = 0x1F; resp_size(resp, 4); break;  // GetSupportedNpadStyleSet
    case 121: OUT(int64_t, 0) = 0; resp_size(resp, 8); break;      // GetNpadJoyHoldType
    case 129: OUT(int64_t, 0) = 0; resp_size(resp, 8); break;
    case 200: memset(resp->data, 0, 0x10); OUT(uint32_t, 0) = 1; resp_size(resp, 0x10); break;
    case 206: {  // SendVibrationValues(aruid): x[0] the devices' handles, x[1] a VibrationValue each: amplitude and
                 // frequency of the low band, then the high band (HD Rumble). MHGU sends both hands every 5 ms; it
                 // vibrates for large monsters' roars, with Vibration on in its options. The low band drives the
                 // controller's low frequency motor, the high band the high one, the stronger hand of each.
        uint32_t n = req->nx >= 2 ? (uint32_t)(req->x[1].size / 16) : 0;
        float lo = 0.0f, hi = 0.0f;
        for (uint32_t i = 0; i < n && i < 8; i++) {
            float v[4];
            memcpy(v, G2H((uint32_t)req->x[1].addr + 16u * i), sizeof v);
            if (v[0] > lo) lo = v[0];
            if (v[2] > hi) hi = v[2];
        }
        input_set_rumble((uint16_t)(fminf(lo, 1.0f) * 65535.0f), (uint16_t)(fminf(hi, 1.0f) * 65535.0f));
        break;
    }
    default: break;  // Activate* / Set*: accept
    }
}
SERVICE("hid:sys", hid_sys) { (void)s; (void)req; resp_size(resp, 0x10); }
