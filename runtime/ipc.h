// HIPC / CMIF message handling for HLE services.
#pragma once
#include "kernel.h"

typedef struct {
    uint64_t addr, size;  // guest address (32-bit process: fits in 32 bits)
} IpcBuf;

typedef struct IpcReq {
    uint32_t type;      // 4 = request, 5 = control, 2 = close, 6/7 = with context
    uint32_t cmd;
    const uint8_t *data;  // payload after the SFCI header
    uint32_t data_size;
    uint64_t pid;
    uint32_t copy_handles[8], move_handles[8];
    int ncopy, nmove;
    IpcBuf x[8], a[8], b[8], c[8];
    int nx, na, nb, nc;
    uint32_t domain_obj;
    uint32_t domain_in_objs[8];
    int ndomain_in;
} IpcReq;

typedef struct Service Service;

typedef struct IpcResp {
    uint32_t result;
    uint8_t data[0x200];
    uint32_t data_size;
    uint32_t copy_handles[8], move_handles[8];
    int ncopy, nmove;
    Service *objects[8];
    int nobjects;
} IpcResp;

typedef void (*ServiceHandler)(Service *s, IpcReq *req, IpcResp *resp);

struct Service {
    const char *name;
    ServiceHandler handler;
    void *state;
    int refs;
    void (*on_close)(Service *s);
};

typedef struct {
    Service *objs[256];
} Domain;

typedef struct {
    KObject obj;
    Service *svc;
    Domain *domain;
} KSession;

// payload helpers
#define IN(type, off) (*(const type *)(req->data + (off)))
#define OUT(type, off) (*(type *)(resp->data + (off)))
static inline void resp_size(IpcResp *resp, uint32_t n) { if (n > resp->data_size) resp->data_size = n; }
#define BUFP(b) ((uint8_t *)G2H((uint32_t)(b)->addr))  // host pointer to an IPC buffer

// Buffers may be sent as a pair (auto-select): use whichever one is non-empty.
static inline IpcBuf *out_buf(IpcReq *req, int i) {
    if (i < req->nb && req->b[i].size) return &req->b[i];
    if (i < req->nc && req->c[i].size) return &req->c[i];
    return i < req->nb ? &req->b[i] : i < req->nc ? &req->c[i] : NULL;
}
static inline IpcBuf *in_buf(IpcReq *req, int i) {
    if (i < req->na && req->a[i].size) return &req->a[i];
    if (i < req->nx && req->x[i].size) return &req->x[i];
    return i < req->na ? &req->a[i] : i < req->nx ? &req->x[i] : NULL;
}

Service *service_new(const char *name, ServiceHandler h, void *state);
Service *service_lookup(const char *name);
void ipc_return_object(IpcResp *resp, Service *s);

typedef struct { const char *name; ServiceHandler handler; } ServiceDef;
#if defined(__linux__)
#define SERVICE_SECTION "services"
#elif defined(__APPLE__)
#define SERVICE_SECTION "__DATA,services"  // Mach-O: bounds from ld64's section$start / section$end (runtime/ipc.c)
#else
#define SERVICE_SECTION ".svc$m"  // between the .svc$a / .svc$z markers in runtime/ipc.c
#endif
#define SERVICE(sname, fn)                                                                  \
    static void fn(Service *s, IpcReq *req, IpcResp *resp);                                 \
    __attribute__((used, section(SERVICE_SECTION))) static const ServiceDef                 \
        HLE_CAT(svc_def_, __LINE__) = {sname, fn};                                          \
    static void fn(Service *s, IpcReq *req, IpcResp *resp)
