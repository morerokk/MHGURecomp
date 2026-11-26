// Applet manager (appletOE), apm, and lm.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

#define AM_NO_MESSAGE MAKE_RESULT(128, 3)
#define AM_NO_DATA MAKE_RESULT(128, 2)

static uint32_t msg_event_r, msg_event_w;
static KEvent *msg_event;
static uint32_t msg_queue[16];
static int msg_count;
static bool preselected_popped;

static void am_init_once(void) {
    if (msg_event) return;
    msg_event = kernel_create_event_pair(&msg_event_w, &msg_event_r);
    msg_queue[msg_count++] = 15;  // FocusStateChanged
    pthread_mutex_lock(&g_klock);
    kernel_signal(msg_event);
    pthread_mutex_unlock(&g_klock);
}

static uint32_t new_event(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

#define UNHANDLED(iface) LOG("[am] %s cmd %u (unhandled, ok)\n", iface, req->cmd)

static void common_state_getter(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = msg_event_r; break;  // GetEventHandle
    case 1:  // ReceiveMessage
        pthread_mutex_lock(&g_klock);
        if (msg_count) {
            OUT(uint32_t, 0) = msg_queue[0];
            memmove(msg_queue, msg_queue + 1, --msg_count * sizeof(uint32_t));
            if (!msg_count) msg_event->signaled = false;
            resp_size(resp, 4);
        } else {
            resp->result = AM_NO_MESSAGE;
        }
        pthread_mutex_unlock(&g_klock);
        break;
    case 5: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;   // GetOperationMode: docked
    case 6: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetPerformanceMode: boost
    case 8: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;   // GetBootMode
    case 9: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;   // GetCurrentFocusState: in focus
    case 13: case 61: resp->copy_handles[resp->ncopy++] = new_event(); break;
    case 60: OUT(int32_t, 0) = 1920; OUT(int32_t, 4) = 1080; resp_size(resp, 8); break;  // GetDefaultDisplayResolution
    default: UNHANDLED("ICommonStateGetter");
    }
}

static void self_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: LOG("[am] application requested exit\n"); rt_exit(EXIT_NORMAL);
    case 9: {  // GetLibraryAppletLaunchableEvent: applets may be launched (the game waits for it at boot)
        uint32_t w, r;
        KEvent *ev = kernel_create_event_pair(&w, &r);
        pthread_mutex_lock(&g_klock);
        kernel_signal(ev);
        pthread_mutex_unlock(&g_klock);
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 40: OUT(uint64_t, 0) = 1; resp_size(resp, 8); break;  // CreateManagedDisplayLayer
    case 69: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: UNHANDLED("ISelfController");
    }
}

static void window_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: OUT(uint64_t, 0) = 0x1234; resp_size(resp, 8); break;  // GetAppletResourceUserId
    default: UNHANDLED("IWindowController");
    }
}

static void generic_ok(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[am] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

// ------------------------------------------------------------------ IStorage / IStorageAccessor
typedef struct { uint8_t *data; uint64_t size; } AmStorage;

static void storage_accessor(Service *s, IpcReq *req, IpcResp *resp) {
    AmStorage *st = s->state;
    switch (req->cmd) {
    case 0: OUT(int64_t, 0) = (int64_t)st->size; resp_size(resp, 8); break;  // GetSize
    case 10: {  // Write(offset, A)
        int64_t off = IN(int64_t, 0);
        IpcBuf *a = in_buf(req, 0);
        uint64_t n = a->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy(st->data + off, BUFP(a), n);
        break;
    }
    case 11: {  // Read(offset, B)
        int64_t off = IN(int64_t, 0);
        IpcBuf *b = out_buf(req, 0);
        uint64_t n = b->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy(BUFP(b), st->data + off, n);
        break;
    }
    default: LOG("[am] IStorageAccessor cmd %u (ok)\n", req->cmd);
    }
}

static void am_storage(Service *s, IpcReq *req, IpcResp *resp) {
    if (req->cmd == 0) ipc_return_object(resp, service_new("IStorageAccessor", storage_accessor, s->state));
    else LOG("[am] IStorage cmd %u (ok)\n", req->cmd);
}

static Service *new_am_storage(const void *data, uint64_t size) {
    AmStorage *st = calloc(1, sizeof *st);
    st->data = calloc(1, size);
    st->size = size;
    if (data) memcpy(st->data, data, size);
    return service_new("IStorage", am_storage, st);
}

extern const uint8_t g_user_id[16];
const char *settings_language(void);

static void application_functions(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1:  // PopLaunchParameter(kind)
        if (IN(uint32_t, 0) == 2 && !preselected_popped) {  // PreselectedUser
            uint8_t p[0x88] = {0};
            *(uint32_t *)p = 0xC79497CA;
            *(uint32_t *)(p + 4) = 1;
            memcpy(p + 8, g_user_id, 16);
            preselected_popped = true;
            ipc_return_object(resp, new_am_storage(p, sizeof p));
        } else {
            resp->result = AM_NO_DATA;
        }
        break;
    case 20: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // EnsureSaveData
    case 21: {  // GetDesiredLanguage
        char lang[8] = {0};
        snprintf(lang, sizeof lang, "%s", settings_language());
        memcpy(resp->data, lang, 8);
        resp_size(resp, 8);
        break;
    }
    case 22: break;  // SetTerminateResult
    case 23: memcpy(resp->data, "1.4.0", 6); resp_size(resp, 0x10); break;  // GetDisplayVersion
    case 40: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // NotifyRunning
    case 130: case 140: resp->copy_handles[resp->ncopy++] = new_event(); break;
    default: UNHANDLED("IApplicationFunctions");
    }
}

static void application_proxy(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    static const struct { int cmd; const char *name; ServiceHandler h; } subs[] = {
        {0, "ICommonStateGetter", common_state_getter},
        {1, "ISelfController", self_controller},
        {2, "IWindowController", window_controller},
        {3, "IAudioController", generic_ok},
        {4, "IDisplayController", generic_ok},
        {10, "IProcessWindingController", generic_ok},
        {11, "ILibraryAppletCreator", generic_ok},
        {20, "IApplicationFunctions", application_functions},
        {1000, "IDebugFunctions", generic_ok},
    };
    for (unsigned i = 0; i < sizeof subs / sizeof *subs; i++) {
        if (subs[i].cmd == (int)req->cmd) {
            ipc_return_object(resp, service_new(subs[i].name, subs[i].h, NULL));
            return;
        }
    }
    UNHANDLED("IApplicationProxy");
}

SERVICE("appletOE", applet_oe) {
    (void)s;
    am_init_once();
    if (req->cmd == 0) ipc_return_object(resp, service_new("IApplicationProxy", application_proxy, NULL));
    else UNHANDLED("appletOE");
}

// ------------------------------------------------------------------ apm
static void apm_session(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 1) { OUT(uint32_t, 0) = 0x92220008; resp_size(resp, 4); }  // GetPerformanceConfiguration
}

SERVICE("apm", apm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("apm:ISession", apm_session, NULL));
    else if (req->cmd == 1) { OUT(uint32_t, 0) = 1; resp_size(resp, 4); }
}

// ------------------------------------------------------------------ lm (guest log output)
static void lm_logger(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s; (void)resp;
    if (req->cmd != 0) return;
    IpcBuf *b = in_buf(req, 0);
    if (!b || !b->size) return;
    const uint8_t *p = BUFP(b), *end = p + b->size;
    p += 0x18;
    char line[1024];
    int n = 0;
    while (p + 2 <= end) {
        uint8_t key = p[0], len = p[1];
        p += 2;
        if (p + len > end) break;
        if (key == 2 || key == 4 || key == 5 || key == 6 || key == 7) {
            n += snprintf(line + n, sizeof line - n, "%s%.*s", n ? " | " : "", len, (const char *)p);
            if (n >= (int)sizeof line) break;
        }
        p += len;
    }
    if (n) LOG("[lm] %s\n", line);
}

SERVICE("lm", lm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("lm:ILogger", lm_logger, NULL));
}
