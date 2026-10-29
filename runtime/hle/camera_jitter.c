// The main 3D camera for the FSR upscaler and SMAA T2x (runtime/nvn/vulkan/fsr.c): its matrices, captured per frame
// for the camera motion vectors, and the sub-pixel jitter put into its projection, which they need to rebuild detail
// from several frames (FSR: a Halton sequence; SMAA T2x: its two quarter-pixel positions). The hooks are always in
// (SMAA T2x can be picked at any time); without jitter they only record the camera.
//
// The game's camera (an sCamera / uCamera object: +0x30 far, +0x34 near, +0x38 aspect, +0x3c vertical field of view
// in degrees, +0x40 eye, +0x50 up, +0x60 target) gets its matrices from two small functions:
//   main+0x1C33C  look-at: r0 = the view matrix to write, r1 = the camera
//   main+0x1C358  projection: r0 = the matrix to write, r1 = the camera (-> 0x9c617c perspective(near, far, fovy,
//                 aspect): m[0] = 1 / tan(fovy / 2) / aspect, m[5] = 1 / tan(fovy / 2), m[10] = f / (n - f), m[11] = -1,
//                 m[14] = n f / (n - f): row-vector (clip = v * M), right-handed, depth 0 at near .. 1 at far)
// (main+0x1C380 / 0x1C3A8 are the same with the arguments swapped: the CPU's world-to-screen for HUD markers. They are
// left alone, so the HUD's markers stay steady while the 3D picture is jittered.)
// Both are hooked: the view is kept per camera; the projection of a camera with the screen's aspect (16:9 as made, or
// the picture's shape after the AspectRatio patch: tools/gen.py PATCHES) is recorded together with that camera's
// view, and then gets the jitter: the third row's x / y (m[8], m[9], zero as made) offset by the jitter in NDC. With
// w = -z, ndc.x = ... - m[8], so m[8] -= 2 jx / w shifts the picture right by jx pixels. y: the translator's clip
// space has +y at the top, so a picture shift down by jy pixels is m[9] += 2 jy / h.
//
//   MHGU_JITTER_TEST=<x>,<y>  (debug) a constant shift of the 3D picture by (x, y) render pixels (x right, y down),
//                             with FSR on or off: shows the sign convention in screenshots
//   MHGU_JITTER_LOG=1         (debug) every capture in the log
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../camera_jitter.h"
#include "../config.h"
#include "../hle.h"

#define FN_LOOKAT     0x21c33cu  // look-at: r0 = out view matrix, r1 = camera
#define FN_PROJECTION 0x21c358u  // projection: r0 = out matrix, r1 = camera
#define CAM_FAR       0x30u
#define CAM_NEAR      0x34u
#define CAM_ASPECT    0x38u
#define RING          64u

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static CamRecord g_ring[RING];
static uint64_t g_seq;
static struct { uint32_t cam; float m[16]; } g_views[8];  // last view per camera
static int g_nviews;
static bool g_on;                   // jitter (renderer: FSR or SMAA T2x active on scene frames)
static bool g_t2x;                  // SMAA T2x's two jitters instead of the Halton sequence
static uint32_t g_rw = 1920, g_rh = 1080, g_phases = 8;
static bool g_test;                 // MHGU_JITTER_TEST
static float g_test_xy[2];
static bool g_log;

static bool main_aspect(float a) {
    return fabsf(a - 16.0f / 9.0f) < 0.01f || fabsf(a - g_render_aspect) < 0.01f;
}

static float halton(uint32_t i, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= (float)base; r += f * (float)(i % base); i /= base; }
    return r;
}

static bool readable(uint32_t a, uint32_t n) { return a >= 0x1000u && a < 0xFFFFFFFFu - n && !(a & 3); }

static void lookat(Cpu *c) {
    uint32_t out = c->r[0], cam = c->r[1];
    fnhook_call_original(c, FN_LOOKAT);
    if (!readable(out, 64) || !readable(cam, 0x70) || !main_aspect(LDF(cam + CAM_ASPECT))) return;
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g_nviews && g_views[i].cam != cam) i++;
    if (i == g_nviews) {
        if (g_nviews < 8) g_nviews++;
        else i = 7;  // (more cameras than that: the last slot is reused)
    }
    g_views[i].cam = cam;
    memcpy(g_views[i].m, G2H(out), 64);
    pthread_mutex_unlock(&g_lock);
}

static void projection(Cpu *c) {
    uint32_t out = c->r[0], cam = c->r[1];
    fnhook_call_original(c, FN_PROJECTION);
    if (!readable(out, 64) || !readable(cam, 0x70) || !main_aspect(LDF(cam + CAM_ASPECT))) return;
    float m[16];
    memcpy(m, G2H(out), 64);
    pthread_mutex_lock(&g_lock);
    int v = 0;
    while (v < g_nviews && g_views[v].cam != cam) v++;
    if (v == g_nviews) { pthread_mutex_unlock(&g_lock); return; }  // (no view of this camera yet)
    float jx = 0, jy = 0;
    if (g_test) {
        jx = g_test_xy[0], jy = g_test_xy[1];
    } else if (g_on && g_t2x) {  // SMAA T2x: (0.25, -0.25), (-0.25, 0.25) with y up, as SMAA.hlsl lists them
        jx = jy = g_present_count & 1 ? -0.25f : 0.25f;
    } else if (g_on) {  // AMD's sequence: Halton(2, 3) - 0.5, phases = 8 (output / render)^2, one phase per frame
        uint32_t i = (uint32_t)(g_present_count % g_phases);
        jx = halton(i + 1, 2) - 0.5f;
        jy = halton(i + 1, 3) - 0.5f;
    }
    CamRecord *r = &g_ring[++g_seq % RING];
    memset(r, 0, sizeof *r);
    r->seq = g_seq;
    r->present = g_present_count;
    memcpy(r->view, g_views[v].m, 64);
    memcpy(r->proj, m, 64);
    r->jitter[0] = jx, r->jitter[1] = jy;
    r->far = LDF(cam + CAM_FAR), r->near = LDF(cam + CAM_NEAR);
    r->fovy = m[5] > 0 ? 2.0f * atanf(1.0f / m[5]) : 1.0f;
    r->cam = cam;
    uint64_t seq = g_seq;
    r->gpu8 = m[8] - 2.0f * jx / (float)g_rw;
    r->gpu9 = m[9] + 2.0f * jy / (float)g_rh;
    float g8 = r->gpu8, g9 = r->gpu9;
    pthread_mutex_unlock(&g_lock);
    if (jx != 0 || jy != 0) {  // into the GPU's copy of the projection only (see the top)
        STF(out + 0x20, g8);
        STF(out + 0x24, g9);
    }
    if (g_log)
        LOG("[camjit] capture %llu at present %llu: cam %08x near %g far %g fovy %.4f jitter %+.4f %+.4f m8 %g m9 %g\n",
            (unsigned long long)seq, (unsigned long long)g_present_count, cam, r->near, r->far, r->fovy, jx, jy,
            LDF(out + 0x20), LDF(out + 0x24));
}

void camjit_set(bool on, uint32_t render_w, uint32_t render_h, uint32_t phases) {
    pthread_mutex_lock(&g_lock);
    g_on = on;
    if (render_w && render_h) g_rw = render_w, g_rh = render_h;
    if (phases) g_phases = phases;
    pthread_mutex_unlock(&g_lock);
}

void camjit_t2x(bool on) {
    pthread_mutex_lock(&g_lock);
    g_t2x = on;
    pthread_mutex_unlock(&g_lock);
}

int camjit_t2x_phase(const float jitter[2]) { return jitter[0] < 0.0f; }

bool camjit_latest(CamRecord *out) {
    pthread_mutex_lock(&g_lock);
    bool ok = g_seq != 0;
    if (ok) *out = g_ring[g_seq % RING];
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool camjit_get(uint64_t seq, CamRecord *out) {
    pthread_mutex_lock(&g_lock);
    bool ok = seq && seq <= g_seq && seq + RING > g_seq && g_ring[seq % RING].seq == seq;
    if (ok) *out = g_ring[seq % RING];
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void camjit_init(void) {
    const char *e = getenv("MHGU_JITTER_TEST");
    g_test = e && sscanf(e, "%f,%f", &g_test_xy[0], &g_test_xy[1]) == 2;
    g_log = getenv("MHGU_JITTER_LOG") != NULL;
    g_rw = g_render_size[0], g_rh = g_render_size[1];
    fnhook_set(FN_LOOKAT, hle_register("camjit:lookat", lookat));
    fnhook_set(FN_PROJECTION, hle_register("camjit:projection", projection));
    if (g_test) LOG("[camjit] test: the 3D picture shifted by (%g, %g) render pixels (x right, y down)\n", g_test_xy[0], g_test_xy[1]);
    else LOG("[camjit] main camera hooked (main+0x1C33C view, main+0x1C358 projection): jitter while FSR or SMAA T2x runs\n");
}
