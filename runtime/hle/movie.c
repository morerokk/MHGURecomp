// movie:: (the Switch's media library, subsdk0, an Android MediaExtractor / MediaCodec-style API) implemented with
// FFmpeg. The game drives demuxing and decoding itself (its movie player thread) and draws the decoded NV12 frames
// with its own shaders; we only supply packets and decoded frames. Ported from LAS Recomp's runtime/hle/movie.c
// (the same library, 64-bit there).
//
// Guest objects (layouts from subsdk0):
//   Buffer (0x24 bytes): +0 range offset, +4 range size, +0x10 owns-data flag, +0x14 capacity, +0x20 data.
//   MediaData: +0 implementation pointer -> here a host handle.
//   Extractor / Decoder: +0 vtable, +4 implementation pointer -> here a host handle; the vtables are ours
//   (host functions at HLE addresses, slot order as subsdk0's _ZTVN5movie9ExtractorE / _ZTVN5movie7DecoderE).
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../romfs.h"

#define MOVIE_EOS 4u
#define MOVIE_ERR 0x1000u  // (subsdk0's "not initialized" result; any non-zero is a failure to the game)

// ------------------------------------------------------------------ host handles (guest objects -> host state)
#define MAX_HANDLES 1024
static void *g_handles[MAX_HANDLES];
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(void *p) {
    pthread_mutex_lock(&g_handle_lock);
    for (uint32_t i = 0; i < MAX_HANDLES; i++)
        if (!g_handles[i]) { g_handles[i] = p; pthread_mutex_unlock(&g_handle_lock); return i + 1; }
    pthread_mutex_unlock(&g_handle_lock);
    FATAL("[movie] out of handles\n");
}
static void *handle_get_ptr(uint32_t h) { return h && h <= MAX_HANDLES ? g_handles[h - 1] : NULL; }
static void handle_free(uint32_t h) { if (h && h <= MAX_HANDLES) g_handles[h - 1] = NULL; }

// Strings handed to the game (MediaData::FindString): interned in guest memory, never freed.
static uint32_t guest_string(const char *s) {
    static struct { char *s; uint32_t g; } tab[64];
    static int n;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n; i++)
        if (!strcmp(tab[i].s, s)) { pthread_mutex_unlock(&lock); return tab[i].g; }
    uint32_t g = hle_alloc((uint32_t)strlen(s) + 1);
    strcpy((char *)G2H(g), s);
    if (n < 64) tab[n].s = strdup(s), tab[n++].g = g;
    pthread_mutex_unlock(&lock);
    return g;
}

// ------------------------------------------------------------------ MediaData
typedef struct { char *key; char *sval; int64_t ival; bool is_str; } MdEntry;
typedef struct { MdEntry e[32]; int n; AVCodecParameters *codecpar; } MediaData;

static MediaData *md(uint32_t guest) {
    uint32_t h = LD32(guest);
    MediaData *m = handle_get_ptr(h);
    if (!m) {
        m = calloc(1, sizeof *m);
        ST32(guest, handle_new(m));
    }
    return m;
}
static void md_clear(MediaData *m) {
    for (int i = 0; i < m->n; i++) { free(m->e[i].key); free(m->e[i].sval); }
    m->n = 0;
    m->codecpar = NULL;
}
static MdEntry *md_slot(MediaData *m, const char *k) {
    for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i].key, k)) return &m->e[i];
    if (m->n == 32) return NULL;
    MdEntry *e = &m->e[m->n++];
    memset(e, 0, sizeof *e);
    e->key = strdup(k);
    return e;
}
static void md_set_int(MediaData *m, const char *k, int64_t v) { MdEntry *e = md_slot(m, k); if (e) { e->ival = v; e->is_str = false; } }
static void md_set_str(MediaData *m, const char *k, const char *v) {
    MdEntry *e = md_slot(m, k);
    if (e) { free(e->sval); e->sval = strdup(v); e->is_str = true; }
}
static MdEntry *md_find(MediaData *m, const char *k, bool str) {
    if (ENV("MHGU_MOVIE_TRACE")) LOG("[movie] MediaData lookup '%s'\n", k);
    for (int i = 0; i < m->n; i++)
        if (m->e[i].is_str == str && !strcmp(m->e[i].key, k)) return &m->e[i];
    static int nlog;
    if (nlog++ < 40) LOG("[movie] MediaData: no %s '%s'\n", str ? "string" : "number", k);
    return NULL;
}

HLE_FUNC("_ZN5movie9MediaDataC1Ev", md_ctor) { ST32(ARG(0), handle_new(calloc(1, sizeof(MediaData)))); }
HLE_FUNC("_ZN5movie9MediaDataD1Ev", md_dtor) {
    uint32_t h = LD32(ARG(0));
    MediaData *m = handle_get_ptr(h);
    if (m) { md_clear(m); free(m); handle_free(h); }
    ST32(ARG(0), 0);
}
HLE_FUNC("_ZN5movie9MediaData5ClearEv", md_clear_hle) { md_clear(md(ARG(0))); }
HLE_FUNC("_ZNK5movie9MediaData9FindInt32EPKcPi", md_find_int32) {
    MdEntry *e = md_find(md(ARG(0)), gstr(ARG(1)), false);
    if (e && ARG(2)) ST32(ARG(2), (uint32_t)(int32_t)e->ival);
    RET(e != NULL);
}
HLE_FUNC("_ZNK5movie9MediaData9FindInt64EPKcPx", md_find_int64) {
    MdEntry *e = md_find(md(ARG(0)), gstr(ARG(1)), false);
    if (e && ARG(2)) { ST32(ARG(2), (uint32_t)e->ival); ST32(ARG(2) + 4, (uint32_t)((uint64_t)e->ival >> 32)); }
    RET(e != NULL);
}
HLE_FUNC("_ZNK5movie9MediaData10FindStringEPKcPS2_", md_find_string) {
    MdEntry *e = md_find(md(ARG(0)), gstr(ARG(1)), true);
    if (e && ARG(2)) ST32(ARG(2), guest_string(e->sval));
    RET(e != NULL);
}
// FindRect(key, int *left, int *top, int *right, int *bottom): the crop rectangle of a video format
HLE_FUNC("_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_", md_find_rect) {
    MediaData *m = md(ARG(0));
    const char *k = gstr(ARG(1));
    MdEntry *w = NULL, *h = NULL;
    for (int i = 0; i < m->n; i++) {
        if (!strcmp(m->e[i].key, "width")) w = &m->e[i];
        if (!strcmp(m->e[i].key, "height")) h = &m->e[i];
    }
    if (strcmp(k, "crop") || !w || !h) { LOG("[movie] MediaData: no rect '%s'\n", k); RET(0); return; }
    uint32_t right = STK(0), bottom = STK(1);
    if (ARG(2)) ST32(ARG(2), 0);
    if (ARG(3)) ST32(ARG(3), 0);
    if (right) ST32(right, (uint32_t)(w->ival - 1));
    if (bottom) ST32(bottom, (uint32_t)(h->ival - 1));
    RET(1);
}

// ------------------------------------------------------------------ Buffer
#define BUF_OFFSET 0x00
#define BUF_SIZE 0x04
#define BUF_OWNS 0x10
#define BUF_CAPACITY 0x14
#define BUF_DATA 0x20
#define BUF_BYTES 0x24

HLE_FUNC("_ZN5movie6BufferC1Ev", buf_ctor) { memset(ARGP(0), 0, BUF_BYTES); }
HLE_FUNC("_ZN5movie6BufferD1Ev", buf_dtor) { (void)c; }  // (only buffers of the game's own memory here)
HLE_FUNC("_ZNK5movie6Buffer4SizeEv", buf_size) { RET(LD32(ARG(0) + BUF_SIZE)); }
HLE_FUNC("_ZNK5movie6Buffer6OffsetEv", buf_offset) { RET(LD32(ARG(0) + BUF_OFFSET)); }
HLE_FUNC("_ZN5movie6Buffer8SetRangeEii", buf_set_range) { ST32(ARG(0) + BUF_OFFSET, ARG(1)); ST32(ARG(0) + BUF_SIZE, ARG(2)); }
HLE_FUNC("_ZN5movie6Buffer18SetDataAndCapacityEPvj", buf_set_data) {
    ST32(ARG(0) + BUF_DATA, ARG(1));
    ST32(ARG(0) + BUF_CAPACITY, ARG(2));
    *(uint8_t *)G2H(ARG(0) + BUF_OWNS) = 0;
}
static void buf_point(uint32_t b, uint32_t data, uint32_t capacity, uint32_t size) {
    memset(G2H(b), 0, BUF_BYTES);
    ST32(b + BUF_DATA, data);
    ST32(b + BUF_CAPACITY, capacity);
    ST32(b + BUF_SIZE, size);
}

HLE_FUNC("_ZN5movie12SetAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", movie_set_allocator) { RET(0); }
HLE_FUNC("_ZN5movie11SetCoreMaskEy", movie_set_core_mask) { RET(0); }

// ------------------------------------------------------------------ helpers
// MHGU_MOVIE_TRACE=1 logs the game's calls through our vtables (the first 400): each slot gets a logging thunk.
static HostFn g_slot_fns[2][32];
static void trace_slot(Cpu *c, int kind, int slot) {
    static int n;
    if (n++ < 400)
        LOG("[movie] %s slot %d (this %x, %x %x %x, lr %x) -> ", kind ? "Decoder" : "Extractor", slot, c->r[0],
            c->r[1], c->r[2], c->r[3], c->r[14]);
    g_slot_fns[kind][slot](c);
    if (n <= 400) LOG("%x\n", c->r[0]);
}
#define THUNK(k, s) static void thunk_##k##_##s(Cpu *c) { trace_slot(c, k, s); }
#define THUNKS(k) THUNK(k, 0) THUNK(k, 1) THUNK(k, 2) THUNK(k, 3) THUNK(k, 4) THUNK(k, 5) THUNK(k, 6) THUNK(k, 7) \
    THUNK(k, 8) THUNK(k, 9) THUNK(k, 10) THUNK(k, 11) THUNK(k, 12) THUNK(k, 13) THUNK(k, 14) THUNK(k, 15)      \
    THUNK(k, 16) THUNK(k, 17) THUNK(k, 18)
THUNKS(0)
THUNKS(1)
#define THUNK_LIST(k) {thunk_##k##_0, thunk_##k##_1, thunk_##k##_2, thunk_##k##_3, thunk_##k##_4, thunk_##k##_5,      \
    thunk_##k##_6, thunk_##k##_7, thunk_##k##_8, thunk_##k##_9, thunk_##k##_10, thunk_##k##_11, thunk_##k##_12,       \
    thunk_##k##_13, thunk_##k##_14, thunk_##k##_15, thunk_##k##_16, thunk_##k##_17, thunk_##k##_18}
static const HostFn g_thunks[2][19] = {THUNK_LIST(0), THUNK_LIST(1)};

static uint32_t make_vtable(HostFn *fns, int n, int kind) {
    uint32_t t = hle_alloc(32 * 4);
    char name[96];
    bool trace = getenv("MHGU_MOVIE_TRACE") != NULL;
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "<movie %s slot %d>", kind ? "Decoder" : "Extractor", i);
        HostFn f = i < n ? fns[i] : NULL;
        if (f && trace && i < 19) g_slot_fns[kind][i] = f, f = g_thunks[kind][i];
        ST32(t + 4 * i, hle_register(name, f));
    }
    return t;
}

static void *self_state(Cpu *c) { return handle_get_ptr(LD32(ARG(0) + 4)); }

static void guest_event_op(Cpu *c, uint32_t event, const char *fn) {
    if (!event) return;
    uint32_t saved[4];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = event;
    guest_call(c, guest_sym(fn));
    memcpy(c->r, saved, sizeof saved);
}

static void movie_log_slot(Cpu *c) {
    static int n;
    if (n++ < 20) LOG("[movie] unimplemented virtual (lr=%x)\n", c->r[14]);
    RET(0);
}

// ------------------------------------------------------------------ Extractor
typedef struct {
    AVFormatContext *fmt;
    int map[16], ntracks;  // exposed track -> stream
    uint64_t selected;     // bitmask of streams
    AVPacket *pkt;
    bool have_pkt, eof;
} Extractor;

// The game reads its audio and video tracks from different threads; an FFmpeg demuxer is not thread-safe.
static pthread_mutex_t ex_lock = PTHREAD_MUTEX_INITIALIZER;

static void ex_fill(Extractor *e) {
    while (e->fmt && !e->have_pkt && !e->eof) {
        if (av_read_frame(e->fmt, e->pkt) < 0) { e->eof = true; break; }
        if (e->selected & (1ull << e->pkt->stream_index)) e->have_pkt = true;
        else av_packet_unref(e->pkt);
    }
}

static void ex_open(Cpu *c, Extractor *e, const char *path) {
    // "rom:/nativeNX/movie/movie00.mp4" -> the RomFS file (a mod's copy if one replaces it)
    const char *rel = path && strstr(path, ":/") ? strstr(path, ":/") + 2 : path ? path : "";
    const char *host = romfs_host_path(rel);
    LOG("[movie] open %s -> %s\n", path ? path : "(null)", host ? host : "(not found)");
    char utf8[1400];  // (FFmpeg takes UTF-8 paths on Windows; the RomFS's are in the ANSI code page there)
    if (!host || avformat_open_input(&e->fmt, plat_utf8_path(host, utf8, sizeof utf8), NULL, NULL) < 0 ||
        avformat_find_stream_info(e->fmt, NULL) < 0) {
        LOG("[movie] cannot open %s\n", path ? path : "(null)");
        if (e->fmt) avformat_close_input(&e->fmt);
        RET(MOVIE_ERR);
        return;
    }
    e->pkt = av_packet_alloc();
    e->ntracks = 0;
    for (unsigned i = 0; i < e->fmt->nb_streams && e->ntracks < 16; i++) {
        enum AVMediaType t = e->fmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO || (t == AVMEDIA_TYPE_AUDIO && !ENV("MHGU_NO_MOVIE_AUDIO"))) e->map[e->ntracks++] = (int)i;
    }
    RET(0);
}
static void ex_set_data_source_path(Cpu *c) { ex_open(c, self_state(c), gstr(ARG(1))); }                // slot 2
static void ex_set_data_source_reader(Cpu *c) { ex_open(c, self_state(c), ARG(2) ? gstr(ARG(2)) : NULL); }  // slot 15

static void ex_get_track_count(Cpu *c) {  // slot 4 (int *)
    Extractor *e = self_state(c);
    ST32(ARG(1), e->fmt ? (uint32_t)e->ntracks : 0);
    RET(0);
}

static void ex_get_track_config(Cpu *c) {  // slot 5 (int, MediaData *)
    Extractor *e = self_state(c);
    int i = (int)ARG(1);
    MediaData *m = md(ARG(2));
    md_clear(m);
    if (!e->fmt || i < 0 || i >= e->ntracks) { RET(MOVIE_ERR); return; }
    AVStream *s = e->fmt->streams[e->map[i]];
    AVCodecParameters *p = s->codecpar;
    m->codecpar = p;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
        md_set_str(m, "mime", p->codec_id == AV_CODEC_ID_VP9 ? "video/x-vnd.on2.vp9" :
                              p->codec_id == AV_CODEC_ID_VP8 ? "video/x-vnd.on2.vp8" : "video/avc");
        md_set_int(m, "width", p->width);
        md_set_int(m, "height", p->height);
        int fps = s->avg_frame_rate.den ? s->avg_frame_rate.num / s->avg_frame_rate.den : 30;
        md_set_int(m, "frame-rate", fps ? fps : 30);
        md_set_int(m, "max-input-size", 4 << 20);
    } else {
        md_set_str(m, "mime", p->codec_id == AV_CODEC_ID_VORBIS ? "audio/vorbis" :
                              p->codec_id == AV_CODEC_ID_OPUS ? "audio/opus" : "audio/mp4a-latm");
        md_set_int(m, "sample-rate", p->sample_rate);
        md_set_int(m, "channel-count", p->ch_layout.nb_channels);
        md_set_int(m, "max-input-size", 256 << 10);
    }
    int64_t dur = s->duration != AV_NOPTS_VALUE ? av_rescale_q(s->duration, s->time_base, (AVRational){1, 1000000})
                                                : e->fmt->duration;
    if (dur > 0) md_set_int(m, "durationUs", dur);
    LOG("[movie] track %d: %s\n", i, p->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio");
    RET(0);
}

static void ex_select_track(Cpu *c) {  // slot 6 (int)
    Extractor *e = self_state(c);
    int i = (int)ARG(1);
    if (i >= 0 && i < e->ntracks) e->selected |= 1ull << e->map[i];
    RET(0);
}
static void ex_unselect_track(Cpu *c) {  // slot 7 (int)
    Extractor *e = self_state(c);
    int i = (int)ARG(1);
    if (i >= 0 && i < e->ntracks) e->selected &= ~(1ull << e->map[i]);
    RET(0);
}

static void ex_seek(Cpu *c) {  // slot 8 (int64 timeUs: r2:r3)
    Extractor *e = self_state(c);
    if (!e->fmt) { RET(MOVIE_ERR); return; }
    if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
    e->eof = false;
    av_seek_frame(e->fmt, -1, (int64_t)ARG64(2), AVSEEK_FLAG_BACKWARD);
    RET(0);
}

static void ex_get_sample_track(Cpu *c) {  // slot 9 (uint32 *)
    Extractor *e = self_state(c);
    ex_fill(e);
    if (!e->have_pkt) { RET(MOVIE_ERR); return; }
    uint32_t t = 0;
    for (int i = 0; i < e->ntracks; i++) if (e->map[i] == e->pkt->stream_index) t = (uint32_t)i;
    ST32(ARG(1), t);
    RET(0);
}

static void ex_read(Cpu *c) {  // slot 10 (Buffer *)
    Extractor *e = self_state(c);
    uint32_t b = ARG(1);
    ex_fill(e);
    if (!e->have_pkt) { RET(MOVIE_ERR); return; }
    uint32_t n = (uint32_t)e->pkt->size, cap = LD32(b + BUF_CAPACITY);
    if (n > cap) { LOG("[movie] packet %u > buffer %u\n", n, cap); n = cap; }
    memcpy(G2H(LD32(b + BUF_DATA)), e->pkt->data, n);
    ST32(b + BUF_OFFSET, 0);
    ST32(b + BUF_SIZE, n);
    RET(0);
}

static void ex_advance(Cpu *c) {  // slot 11
    Extractor *e = self_state(c);
    if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
    RET(0);
}

static void ex_get_sample_time(Cpu *c) {  // slot 12 (int64 *)
    Extractor *e = self_state(c);
    ex_fill(e);
    if (!e->have_pkt) { RET(MOVIE_ERR); return; }
    AVStream *s = e->fmt->streams[e->pkt->stream_index];
    int64_t ts = e->pkt->pts != AV_NOPTS_VALUE ? e->pkt->pts : e->pkt->dts;
    uint64_t us = (uint64_t)av_rescale_q(ts, s->time_base, (AVRational){1, 1000000});
    ST32(ARG(1), (uint32_t)us);
    ST32(ARG(1) + 4, (uint32_t)(us >> 32));
    RET(0);
}

static void ex_buffered_range(Cpu *c) {  // slot 14 (int64 *start, int64 *end): a local file is all there
    Extractor *e = self_state(c);
    uint64_t dur = e->fmt && e->fmt->duration > 0 ? (uint64_t)e->fmt->duration : 0;
    if (ARG(1)) { ST32(ARG(1), 0); ST32(ARG(1) + 4, 0); }
    if (ARG(2)) { ST32(ARG(2), (uint32_t)dur); ST32(ARG(2) + 4, (uint32_t)(dur >> 32)); }
    RET(0);
}

static void ex_dtor(Cpu *c) {  // slots 0 / 1
    uint32_t h = LD32(ARG(0) + 4);
    Extractor *e = handle_get_ptr(h);
    if (e) {
        if (e->have_pkt) av_packet_unref(e->pkt);
        if (e->pkt) av_packet_free(&e->pkt);
        if (e->fmt) avformat_close_input(&e->fmt);
        free(e);
        handle_free(h);
        ST32(ARG(0) + 4, 0);
    }
}

#define EX_LOCKED(fn) \
    static void fn##_l(Cpu *c) { pthread_mutex_lock(&ex_lock); fn(c); pthread_mutex_unlock(&ex_lock); }
EX_LOCKED(ex_dtor)
EX_LOCKED(ex_set_data_source_path)
EX_LOCKED(ex_set_data_source_reader)
EX_LOCKED(ex_get_track_count)
EX_LOCKED(ex_get_track_config)
EX_LOCKED(ex_select_track)
EX_LOCKED(ex_unselect_track)
EX_LOCKED(ex_seek)
EX_LOCKED(ex_get_sample_track)
EX_LOCKED(ex_read)
EX_LOCKED(ex_advance)
EX_LOCKED(ex_get_sample_time)
EX_LOCKED(ex_buffered_range)

// Extractor(ContainerType, CacheSize)
HLE_FUNC("_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE", movie_extractor_ctor) {
    static uint32_t vt;
    if (!vt) {
        HostFn f[16] = {ex_dtor_l, ex_dtor_l, ex_set_data_source_path_l, movie_log_slot, ex_get_track_count_l,
                        ex_get_track_config_l, ex_select_track_l, ex_unselect_track_l, ex_seek_l,
                        ex_get_sample_track_l, ex_read_l, ex_advance_l, ex_get_sample_time_l, movie_log_slot,
                        ex_buffered_range_l, ex_set_data_source_reader_l};
        vt = make_vtable(f, 16, 0);
    }
    ST32(ARG(0), vt);
    ST32(ARG(0) + 4, handle_new(calloc(1, sizeof(Extractor))));
    LOG("[movie] Extractor(container %u, cache %u)\n", ARG(1), ARG(2));
}

// ------------------------------------------------------------------ Decoder
#define NIN 4
#define NOUT 8
#define NPENDING 96  // decoded frames waiting for an output slot: the game holds video frames until their time,
                     // which runs on the audio clock, and audio packets may sit a second or more into the file

typedef struct { int idx; int64_t time; uint32_t flags; } OutReady;

typedef struct {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    bool video;
    uint32_t events[4];  // DecoderEvents: input available, output available, format changed, error (EventType *)
    uint32_t in_mem[NIN], in_cap;
    bool in_busy[NIN];
    uint32_t out_mem[NOUT], out_cap, out_size[NOUT];
    bool out_busy[NOUT];
    OutReady ready[NOUT];
    int nready;
    int width, height, stride, sample_rate, channels;
    AVFrame *pending[NPENDING];
    int64_t pending_time[NPENDING];
    int npending;
    bool eos_pending;
    int64_t eos_time;
    bool stopped;      // Stop / destroyed: no more events (see dec_stop)
    bool format_told;  // the output format change before the first frame (the game sets its renderer up on it)
    pthread_mutex_t lock;
} Decoder;

// Input / output buffers are guest memory (the HLE heap is never freed): those of destroyed decoders are reused.
static struct { uint32_t mem, size; } g_buf_pool[64];
static int g_nbuf_pool;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t buf_get(uint32_t size) {
    pthread_mutex_lock(&g_pool_lock);
    for (int i = 0; i < g_nbuf_pool; i++)
        if (g_buf_pool[i].size == size) {
            uint32_t m = g_buf_pool[i].mem;
            g_buf_pool[i] = g_buf_pool[--g_nbuf_pool];
            pthread_mutex_unlock(&g_pool_lock);
            return m;
        }
    pthread_mutex_unlock(&g_pool_lock);
    return hle_alloc(size);
}
static void buf_put(uint32_t m, uint32_t size) {
    if (!m) return;
    pthread_mutex_lock(&g_pool_lock);
    if (g_nbuf_pool < 64) g_buf_pool[g_nbuf_pool].mem = m, g_buf_pool[g_nbuf_pool++].size = size;
    pthread_mutex_unlock(&g_pool_lock);
}

// Signal one of the game's decoder events unless the decoder was stopped (under the lock, so a Stop on another
// thread can't slip between the check and the signal).
static void dec_signal(Cpu *c, Decoder *d, int i) {
    pthread_mutex_lock(&d->lock);
    if (!d->stopped) guest_event_op(c, d->events[i], "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    pthread_mutex_unlock(&d->lock);
}

static void dec_configure(Cpu *c) {  // slot 2 (MediaData *)
    Decoder *d = self_state(c);
    AVCodecParameters *p = md(ARG(1))->codecpar;
    if (!p) { LOG("[movie] Configure without codec parameters\n"); RET(MOVIE_ERR); return; }
    const AVCodec *codec = avcodec_find_decoder(p->codec_id);
    if (!codec) { LOG("[movie] no decoder for codec %d\n", p->codec_id); RET(MOVIE_ERR); return; }
    d->ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d->ctx, p);
    d->ctx->thread_count = 4;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) { LOG("[movie] cannot open decoder\n"); RET(MOVIE_ERR); return; }
    d->frame = av_frame_alloc();
    d->pkt = av_packet_alloc();
    d->video = p->codec_type == AVMEDIA_TYPE_VIDEO;
    d->width = p->width;
    d->height = p->height;
    d->stride = p->width;  // tight NV12: MHGU copies Y (width x height) and then UV straight after it
    d->sample_rate = p->sample_rate;
    d->channels = p->ch_layout.nb_channels;
    d->in_cap = d->video ? 4 << 20 : 256 << 10;
    d->out_cap = d->video ? (uint32_t)(d->stride * d->height * 3 / 2 + d->stride) : 64 << 10;
    for (int i = 0; i < NIN; i++) d->in_mem[i] = buf_get(d->in_cap);
    for (int i = 0; i < NOUT; i++) d->out_mem[i] = buf_get(d->out_cap);
    LOG("[movie] decoder %s configured: %dx%d %d Hz %d ch\n", codec->name, d->width, d->height, d->sample_rate, d->channels);
    RET(0);
}

static void dec_register_events(Cpu *c) {  // slot 3 (DecoderEvents *)
    Decoder *d = self_state(c);
    for (int i = 0; i < 4; i++) d->events[i] = LD32(ARG(1) + 4 * i);
    RET(0);
}

static void dec_output_format(Decoder *d, MediaData *m) {
    md_clear(m);
    if (d->video) {
        md_set_str(m, "mime", "video/raw");
        md_set_int(m, "width", d->width);
        md_set_int(m, "height", d->height);
        md_set_int(m, "stride", d->stride);
        md_set_int(m, "slice-height", d->height);
        md_set_int(m, "color-format", 21);  // COLOR_FormatYUV420SemiPlanar (NV12)
        md_set_int(m, "nv12-y-stride", d->stride);
        md_set_int(m, "nv12-y-offset", 0);
        md_set_int(m, "nv12-uv-offset", d->stride * d->height);
        md_set_int(m, "nv12-colorspace", 0);
    } else {
        md_set_str(m, "mime", "audio/raw");
        md_set_int(m, "sample-rate", d->sample_rate);
        md_set_int(m, "channel-count", d->channels);
    }
}
static void dec_get_output_format(Cpu *c) { dec_output_format(self_state(c), md(ARG(1))); RET(0); }         // slot 4
static void dec_get_output_buffer_format(Cpu *c) { dec_output_format(self_state(c), md(ARG(2))); RET(0); }  // slot 18

static void dec_start(Cpu *c) {  // slot 5
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    d->stopped = false;
    pthread_mutex_unlock(&d->lock);
    dec_signal(c, d, 0);  // all input buffers are free
    RET(0);
}

// slot 6: Stop. A stopped hardware decoder signals nothing: stop signalling and clear what is still set (an event
// still set after the game deleted the decoder wakes its decoder thread into a call through a dead object).
static void dec_stop(Cpu *c) {
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    if (!d->stopped) {
        d->stopped = true;
        LOG("[movie] decoder %s stopped\n", d->video ? "video" : "audio");
        for (int i = 0; i < 4; i++) guest_event_op(c, d->events[i], "_ZN2nn2os10ClearEventEPNS0_9EventTypeE");
    }
    pthread_mutex_unlock(&d->lock);
    RET(0);
}

static void dec_flush(Cpu *c) {  // slots 7 (Reset) / 8 (Flush): drop everything in flight
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    if (d->ctx) avcodec_flush_buffers(d->ctx);
    for (int i = 0; i < d->npending; i++) av_frame_free(&d->pending[i]);
    d->npending = 0;
    d->nready = 0;
    d->eos_pending = false;
    memset(d->in_busy, 0, sizeof d->in_busy);
    memset(d->out_busy, 0, sizeof d->out_busy);
    pthread_mutex_unlock(&d->lock);
    RET(0);
}

static void dec_acquire_input(Cpu *c) {  // slot 9 (int *index, int *remaining)
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    if (d->npending >= NPENDING - 8) { pthread_mutex_unlock(&d->lock); RET(MOVIE_ERR); return; }  // backpressure
    int idx = -1, free_n = 0;
    for (int i = 0; i < NIN; i++)
        if (!d->in_busy[i]) { if (idx < 0) idx = i; else free_n++; }
    if (idx >= 0) d->in_busy[idx] = true;
    pthread_mutex_unlock(&d->lock);
    if (idx < 0) { RET(MOVIE_ERR); return; }
    ST32(ARG(1), (uint32_t)idx);
    if (ARG(2)) ST32(ARG(2), (uint32_t)free_n);
    RET(0);
}

static void dec_get_input_buffer(Cpu *c) {  // slot 10 (int, Buffer *)
    Decoder *d = self_state(c);
    int i = (int)ARG(1);
    if (i < 0 || i >= NIN) { RET(MOVIE_ERR); return; }
    buf_point(ARG(2), d->in_mem[i], d->in_cap, 0);
    RET(0);
}

// Copy a decoded frame into an output slot (NV12 video / interleaved s16 audio); caller holds the lock.
static bool emit_frame(Decoder *d, AVFrame *f, int64_t time, uint32_t flags) {
    int slot = -1;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) { slot = i; break; }
    if (slot < 0) return false;
    uint8_t *o = G2H(d->out_mem[slot]);
    uint32_t size = 0;
    if (flags & MOVIE_EOS) {
        size = 0;
    } else if (d->video) {
        int h = d->height, w = d->width, uv_off = d->stride * h;
        for (int y = 0; y < h; y++) memcpy(o + y * d->stride, f->data[0] + y * f->linesize[0], w);
        if (f->format == AV_PIX_FMT_NV12) {
            for (int y = 0; y < h / 2; y++) memcpy(o + uv_off + y * d->stride, f->data[1] + y * f->linesize[1], w);
        } else {  // planar 4:2:0: interleave U and V
            for (int y = 0; y < h / 2; y++) {
                uint8_t *uv = o + uv_off + y * d->stride;
                const uint8_t *u = f->data[1] + y * f->linesize[1], *v = f->data[2] + y * f->linesize[2];
                for (int x = 0; x < w / 2; x++) { uv[2 * x] = u[x]; uv[2 * x + 1] = v[x]; }
            }
        }
        size = (uint32_t)(uv_off + d->stride * (h / 2));
    } else {
        int ch = f->ch_layout.nb_channels, n = f->nb_samples;
        int16_t *out = (int16_t *)o;
        if ((uint32_t)(n * ch * 2) > d->out_cap) n = (int)(d->out_cap / (ch * 2));
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++) {
                float s;
                switch (f->format) {
                case AV_SAMPLE_FMT_FLTP: s = ((const float *)f->data[k])[i]; break;
                case AV_SAMPLE_FMT_FLT: s = ((const float *)f->data[0])[i * ch + k]; break;
                case AV_SAMPLE_FMT_S16P: s = ((const int16_t *)f->data[k])[i] / 32768.0f; break;
                case AV_SAMPLE_FMT_S16: s = ((const int16_t *)f->data[0])[i * ch + k] / 32768.0f; break;
                default: s = 0.0f;
                }
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                out[i * ch + k] = (int16_t)(s * 32767.0f);
            }
        size = (uint32_t)(n * ch * 2);
    }
    d->out_busy[slot] = true;
    d->out_size[slot] = size;
    d->ready[d->nready++] = (OutReady){slot, time, flags};
    return true;
}

// Emit queued frames (and a pending end-of-stream marker) into free output slots; caller holds the lock.
static bool flush_pending(Decoder *d) {
    bool any = false;
    while (d->npending && emit_frame(d, d->pending[0], d->pending_time[0], 0)) {
        av_frame_free(&d->pending[0]);
        memmove(d->pending, d->pending + 1, (d->npending - 1) * sizeof d->pending[0]);
        memmove(d->pending_time, d->pending_time + 1, (d->npending - 1) * sizeof d->pending_time[0]);
        d->npending--;
        any = true;
    }
    if (!d->npending && d->eos_pending && emit_frame(d, NULL, d->eos_time, MOVIE_EOS)) {
        d->eos_pending = false;
        any = true;
    }
    return any;
}

// slot 11: SendInputBufferForDecode(int index, int offset, int size, int64 timeUs, uint32 flags)
static void dec_send_input(Cpu *c) {
    Decoder *d = self_state(c);
    int idx = (int)ARG(1), off = (int)ARG(2), size = (int)ARG(3);
    int64_t time = (int64_t)(((uint64_t)STK(1) << 32) | STK(0));
    uint32_t flags = STK(2);
    pthread_mutex_lock(&d->lock);
    if (size > 0 && idx >= 0 && idx < NIN) {
        av_packet_unref(d->pkt);
        d->pkt->data = (uint8_t *)G2H(d->in_mem[idx]) + off;
        d->pkt->size = size;
        d->pkt->pts = time;
        avcodec_send_packet(d->ctx, d->pkt);
    }
    if (flags & MOVIE_EOS) avcodec_send_packet(d->ctx, NULL);
    bool produced = false;
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        int64_t t = d->frame->pts != AV_NOPTS_VALUE ? d->frame->pts : time;
        if (d->npending || !emit_frame(d, d->frame, t, 0)) {  // keep order: behind anything already pending
            if (d->npending < NPENDING) {
                d->pending[d->npending] = av_frame_clone(d->frame);
                d->pending_time[d->npending++] = t;
            } else {
                LOG("[movie] output queue full, dropping a frame\n");
            }
        }
        produced = true;
    }
    if (flags & MOVIE_EOS) { d->eos_pending = true; d->eos_time = time; }
    produced |= flush_pending(d);
    if (idx >= 0 && idx < NIN) d->in_busy[idx] = false;
    pthread_mutex_unlock(&d->lock);
    dec_signal(c, d, 0);
    if (produced && !d->format_told) {  // (as a MediaCodec: INFO_OUTPUT_FORMAT_CHANGED before the first buffer)
        d->format_told = true;
        dec_signal(c, d, 2);
    }
    if (produced) dec_signal(c, d, 1);
    RET(0);
}

// slot 12: AcquireOutputBufferIndex(int *index, int64 *timeUs, uint32 *flags, int *remaining)
static void dec_acquire_output(Cpu *c) {
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    if (!d->nready) { pthread_mutex_unlock(&d->lock); RET(MOVIE_ERR); return; }
    OutReady r = d->ready[0];
    if (d->video && ENV("MHGU_MOVIE_TRACE")) {  // (debug) the game's frame consumer: player r4, consumer at +0xc
        uint32_t obj = LD32(c->r[4] + 0xc), vt = LD32(obj);
        LOG("[movie] video frame consumer %x vtable %x:", obj, vt);
        for (int i = 0; i < 20; i++) LOG(" %x", LD32(vt + 4 * i));
        LOG("\n");
    }
    memmove(d->ready, d->ready + 1, --d->nready * sizeof *d->ready);
    int remaining = d->nready;
    pthread_mutex_unlock(&d->lock);
    ST32(ARG(1), (uint32_t)r.idx);
    if (ARG(2)) { ST32(ARG(2), (uint32_t)r.time); ST32(ARG(2) + 4, (uint32_t)((uint64_t)r.time >> 32)); }
    if (ARG(3)) ST32(ARG(3), r.flags);
    if (STK(0)) ST32(STK(0), (uint32_t)remaining);
    RET(0);
}

// slots 13 (int, Buffer *) / 17 (int, Buffer *, MediaData *). The game hands in a Buffer around its own memory
// (SetDataAndCapacity + SetRange: its video texture / audio ring) and expects the decoded data copied there; a Buffer
// without memory is pointed at ours instead.
static void dec_get_output_buffer(Cpu *c) {
    Decoder *d = self_state(c);
    int i = (int)ARG(1);
    uint32_t b = ARG(2);
    if (i < 0 || i >= NOUT) { RET(MOVIE_ERR); return; }
    uint32_t data = LD32(b + BUF_DATA), cap = LD32(b + BUF_CAPACITY), off = LD32(b + BUF_OFFSET);
    if (data && cap) {
        if (ENV("MHGU_MOVIE_TRACE")) {
            static int nlog;
            if (nlog++ < 4) LOG("[movie] output %s into game buffer %x cap %x off %x (ours %x)\n", d->video ? "video" : "audio",
                                data, cap, off, d->out_size[i]);
        }
        uint32_t n = d->out_size[i];
        if (off > cap) off = cap;
        if (n > cap - off) n = cap - off;
        memcpy(G2H(data + off), G2H(d->out_mem[i]), n);
        ST32(b + BUF_SIZE, n);
    } else {
        buf_point(b, d->out_mem[i], d->out_cap, d->out_size[i]);
    }
    RET(0);
}
static void dec_get_output_buffer_md(Cpu *c) {
    dec_get_output_buffer(c);
    if (ARG(3)) dec_output_format(self_state(c), md(ARG(3)));
}

static void dec_get_output_buffer_size(Cpu *c) {  // slot 14 (int *size, int *count)
    Decoder *d = self_state(c);
    if (ARG(1)) ST32(ARG(1), d->out_cap);
    if (ARG(2)) ST32(ARG(2), NOUT);
    RET(0);
}

static void dec_release_output(Cpu *c) {  // slot 15 (int)
    Decoder *d = self_state(c);
    dec_signal(c, d, 0);  // input may be accepted again
    int i = (int)ARG(1);
    pthread_mutex_lock(&d->lock);
    if (i >= 0 && i < NOUT) d->out_busy[i] = false;
    bool produced = flush_pending(d);
    pthread_mutex_unlock(&d->lock);
    if (produced) dec_signal(c, d, 1);
    RET(0);
}

static void dec_last_error(Cpu *c) { RET(0); }  // slot 16

static void dec_dtor(Cpu *c) {  // slots 0 / 1
    uint32_t h = LD32(ARG(0) + 4);
    Decoder *d = handle_get_ptr(h);
    if (!d) return;
    dec_stop(c);
    pthread_mutex_lock(&d->lock);
    if (d->ctx) avcodec_free_context(&d->ctx);
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt) av_packet_free(&d->pkt);
    for (int i = 0; i < d->npending; i++) av_frame_free(&d->pending[i]);
    for (int i = 0; i < NIN; i++) buf_put(d->in_mem[i], d->in_cap);
    for (int i = 0; i < NOUT; i++) buf_put(d->out_mem[i], d->out_cap);
    pthread_mutex_unlock(&d->lock);
    pthread_mutex_destroy(&d->lock);
    free(d);
    handle_free(h);
    ST32(ARG(0) + 4, 0);
}

// Decoder(DecoderType, DecoderOutputFormat, DecoderMode)
HLE_FUNC("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE", movie_decoder_ctor) {
    static uint32_t vt;
    if (!vt) {
        HostFn f[19] = {dec_dtor, dec_dtor, dec_configure, dec_register_events, dec_get_output_format, dec_start,
                        dec_stop, dec_flush, dec_flush, dec_acquire_input, dec_get_input_buffer, dec_send_input,
                        dec_acquire_output, dec_get_output_buffer, dec_get_output_buffer_size, dec_release_output,
                        dec_last_error, dec_get_output_buffer_md, dec_get_output_buffer_format};
        vt = make_vtable(f, 19, 1);
    }
    Decoder *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->lock, NULL);
    ST32(ARG(0), vt);
    ST32(ARG(0) + 4, handle_new(d));
    LOG("[movie] Decoder(type %u, output format %u, mode %u)\n", ARG(1), ARG(2), ARG(3));
}
