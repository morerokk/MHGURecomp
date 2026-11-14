// audren:u (audio renderer) and audout:u.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "../hle.h"
#include "../ipc.h"
#include "../config.h"

// ------------------------------------------------------------------ audren: a CPU mixer modelled on
// Ryujinx's AudioRenderer (VoiceState / DataSourceHelper / StateUpdater, copies in notes/ryujinx_audio).
// Voices decode PCM16 / ADPCM wave buffers, resample to 48 kHz, and mix into their destination mix's
// buffers; sub mixes route into their destination; the final mix feeds the device sink (5.1), which we fold
// down to stereo or play as 5.1 ([Audio] Output) through an SDL audio stream. Splitters route voices / sub mixes to several destinations (MHGU
// sends every voice through one). Effects: buffer mixer (1), Aux (2: send / return rings through which the game runs
// its own CPU-side effects), reverb (4) and I3DL2 reverb (5), the types MHGU uses; others are ignored.
#define WB_COUNT 4
#define MIXBUF_MAX 24
#define HIST 4

typedef struct {
    uint64_t addr, size, ctx;
    uint32_t start, end, loop_start, loop_end;
    int32_t loop_count;
    uint8_t loop, eos, sent;  // sent = IsSendToAudioProcessor
} WaveBuf;

typedef struct {  // per channel resource, "VoiceUpdateState"
    uint8_t valid[WB_COUNT];
    uint32_t wb_index, consumed;
    uint64_t played;
    int32_t offset, loop_count;
    float fraction;
    int16_t hist[HIST];
    int16_t pred_scale, h0, h1;
    float bq[2][2];  // biquad filter state per voice filter
} ChanState;

typedef struct {
    float mix[MIXBUF_MAX], prev[MIXBUF_MAX];
    uint8_t used;
} ChanRes;

typedef struct {
    uint8_t in_use, is_new, play_state;  // play_state: 0 started, 1 stopped, 2 stopping, 3 paused
    uint8_t fmt, chans, drop;
    uint32_t rate, wb_count, flush;
    float pitch, volume, prev_volume;
    int32_t mix_id;
    uint32_t splitter_id;
    uint32_t chan_ids[6];
    uint64_t ds_addr;
    uint16_t behaviour;
    uint8_t bq_on[2], bq_reset[2];
    float bq[2][5];  // a0 a1 a2 b1 b2
    WaveBuf wb[WB_COUNT];
} Voice;

typedef struct {
    uint8_t used;
    float volume;
    uint32_t bufs;
    int32_t dest;
    uint32_t dest_splitter;
    float vol[MIXBUF_MAX * MIXBUF_MAX];
    float *buf;  // bufs * sample_count
} Mix;

typedef struct Effect Effect;

// Splitters (REV4 layout, as Ryujinx / yuzu): a splitter lists destination ids; a destination is a mix and its 24
// per-buffer volumes. Channel c of a voice (buffer c of a sub mix) goes to destinations c, c + n, c + 2n... of its
// splitter (n = its channel / buffer count).
typedef struct { uint32_t ndest; int32_t *dest; } SplitInfo;
typedef struct { int32_t mix; uint8_t in_use; float vol[MIXBUF_MAX], prev[MIXBUF_MAX]; } SplitDest;

// MHGU_AUDIO_TRACE=1: a line per second on render pacing (frames, game updates, the longest stretch without one),
// device queue underruns and the aux rings, plus a line for every voice lifetime (first wave buffer appended to stop /
// free / reuse) that played nothing (SKIP), played only into nowhere or at volume 0 (SILENT), or stopped before the
// end of its non-looping buffers (CUT). =2 also logs the voices that played through and those never started.
typedef struct {
    uint8_t open, started, loop, fmt;
    uint32_t rate, bufs, dropped;
    int32_t mix_id;
    uint32_t splitter_id;
    uint64_t addr, size, start_frame, first_frame, updates;  // first_frame: frame of the first sample + 1
    uint64_t expected, played, unrouted;  // samples (channel 0)
    float vol_max;
} VoiceTrace;

typedef struct {
    uint64_t updates_total;
    uint32_t frames, updates, since_update, max_between, underruns, aux_glitches, send_overruns;
    uint32_t started, skipped, silent, cut;
    uint32_t in_use_min, in_use_max, playing_max, catch_ups, skips;  // voices the game holds / that rendered samples
    double last_update_ms, max_gap_ms, max_render_ms;
    int64_t queue_min;  // samples
    // aux: cumulative positions (all channels) of the return ring's DSP read / CPU write and the send ring's DSP
    // write / CPU read, advanced by the change of each offset per frame
    uint8_t aux_seen;
    uint32_t aux_ch, aux_last[4];
    int64_t aux_pos[4], aux_lag_min, aux_lag_max;
} TraceStats;

typedef struct {
    uint32_t sample_rate, sample_count, mix_buffers, submixes, voices, sinks, effects, perf_frames;
    uint32_t splitters, revision;
    uint64_t elapsed_frames;
    uint32_t event_r, event_w;
    KEvent *event;
    uint32_t state;  // 0 started, 1 stopped
    pthread_mutex_t lock;
    Voice *voice;
    ChanRes *chres;
    ChanState *chst;
    Mix *mix;
    uint32_t nmix;
    uint8_t sink_used, sink_inputs, sink_input[6];
    Effect *fx;
    SplitInfo *spl;
    SplitDest *sdest;
    uint32_t nsdest;
    SDL_AudioStream *stream;
    bool output;     // sound on (not MHGU_NO_AUDIO, SDL audio up): the stream follows [Audio] Output
    int out_ch;      // the stream's channels: 2, or 6 (5.1)
    FILE *wav;
    uint32_t wav_bytes;
    int wav_ch;
    pthread_t thread;
    volatile bool quit;  // reboot (audio_reset)
    VoiceTrace *vt;  // MHGU_AUDIO_TRACE only
    TraceStats ts;
} Renderer;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int audio_trace(void) {
    static int level = -1;
    if (level < 0) level = getenv("MHGU_AUDIO_TRACE") ? atoi(getenv("MHGU_AUDIO_TRACE")) : 0;
    return level;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void trace_reset_second(TraceStats *ts) {
    ts->frames = ts->updates = ts->max_between = ts->underruns = ts->aux_glitches = ts->send_overruns = 0;
    ts->started = ts->skipped = ts->silent = ts->cut = 0;
    ts->in_use_min = UINT32_MAX;
    ts->in_use_max = ts->playing_max = ts->catch_ups = ts->skips = 0;
    ts->max_gap_ms = ts->max_render_ms = 0;
    ts->queue_min = INT64_MAX;
    ts->aux_lag_min = INT64_MAX;
    ts->aux_lag_max = INT64_MIN;
}

// A voice's first wave buffer since it was last closed opens a lifetime.
static void vt_open(Renderer *r, uint32_t vi, const Voice *v, const WaveBuf *wb) {
    VoiceTrace *t = &r->vt[vi];
    if (!t->open) {
        memset(t, 0, sizeof *t);
        t->open = 1;
        t->fmt = v->fmt;
        t->rate = v->rate;
        t->mix_id = v->mix_id;
        t->splitter_id = v->splitter_id;
        t->addr = wb->addr;
        t->size = wb->size;
        t->start_frame = r->elapsed_frames;
        t->updates = r->ts.updates_total;
        r->ts.started++;
    }
    t->bufs++;
    if (wb->loop) t->loop = 1;
    else if (wb->end > wb->start) t->expected += wb->end - wb->start;
}

static void vt_close(Renderer *r, uint32_t vi, const char *why) {
    VoiceTrace *t = &r->vt[vi];
    if (!t->open) return;
    t->open = 0;
    const char *kind = !t->started ? "unstarted"
                     : !t->played ? "SKIP"
                     : t->vol_max == 0 || t->unrouted >= t->played ? "SILENT"
                     : !t->loop && t->played < t->expected ? "CUT" : "ok";
    if (kind[0] == 'S' && kind[1] == 'K') r->ts.skipped++;
    else if (kind[0] == 'S') r->ts.silent++;
    else if (kind[0] == 'C') r->ts.cut++;
    if (audio_trace() < 2 && (kind[0] == 'o' || kind[0] == 'u')) return;
    char first[32] = "never";
    if (t->first_frame) snprintf(first, sizeof first, "%llu", (unsigned long long)(t->first_frame - 1 - t->start_frame));
    LOG("[atrace] f%llu v%u %s (%s): fmt %u %u Hz, %u buf%s (%u dropped), %llu/%llu%s samples, %llu unrouted, "
        "vol max %.3f, first sample after %s frames, lived %llu frames / %llu updates, mix %d spl %d, wb %llx+%llx\n",
        (unsigned long long)r->elapsed_frames, vi, kind, why, t->fmt, t->rate, t->bufs, t->bufs == 1 ? "" : "s",
        t->dropped, (unsigned long long)t->played, (unsigned long long)t->expected, t->loop ? " (loops)" : "",
        (unsigned long long)t->unrouted, t->vol_max, first, (unsigned long long)(r->elapsed_frames - t->start_frame),
        (unsigned long long)(r->ts.updates_total - t->updates), t->mix_id, (int32_t)t->splitter_id,
        (unsigned long long)t->addr, (unsigned long long)t->size);
}

// One sample of DSP ADPCM at absolute sample index s (frames of 8 bytes: header + 14 nibbles).
static int16_t adpcm_next(const uint8_t *data, uint64_t size, uint32_t s, const int16_t *coefs, ChanState *cs) {
    uint32_t frame = s / 14, n = s % 14;
    uint64_t hdr = (uint64_t)frame * 8;
    if (hdr + 1 + n / 2 >= size) return 0;
    uint8_t ps = data[hdr];
    int scale = ps & 15, ci = (ps >> 4) & 7;
    uint8_t b = data[hdr + 1 + n / 2];
    int nib = (n & 1) ? (int8_t)(b << 4) >> 4 : (int8_t)b >> 4;
    int pred = coefs[ci * 2] * cs->h0 + coefs[ci * 2 + 1] * cs->h1;
    int v = ((nib * (2048 << scale)) + pred + 1024) >> 11;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    cs->h1 = cs->h0;
    cs->h0 = (int16_t)v;
    return (int16_t)v;
}

static void mark_end(ChanState *cs, WaveBuf *wb, uint32_t *idx) {
    cs->valid[*idx] = 0;
    *idx = (*idx + 1) % WB_COUNT;
    cs->loop_count = 0;
    cs->consumed++;
    if (wb->eos) cs->played = 0;
}

// Decode up to `need` samples of channel `ch` into out; returns how many were produced.
static int decode_voice(Voice *v, int ch, ChanState *cs, int16_t *out, int need) {
    int y = 0;
    uint32_t idx = cs->wb_index;
    const int16_t *coefs = (const int16_t *)G2H((uint32_t)v->ds_addr);
    while (y < need) {
        if (!cs->valid[idx]) break;  // starving
        WaveBuf *wb = &v->wb[idx];
        if (cs->offset == 0 && v->fmt == 6 && wb->ctx) {
            const int16_t *c = (const int16_t *)G2H((uint32_t)wb->ctx);
            cs->pred_scale = c[0]; cs->h0 = c[1]; cs->h1 = c[2];
        }
        uint32_t start = wb->start, end = wb->end;
        if (cs->loop_count > 0 && wb->loop_start && wb->loop_end && wb->loop_start <= wb->loop_end) {
            start = wb->loop_start;
            end = wb->loop_end;
        }
        int total = (int)end - (int)start, n = 0;
        int avail = total - cs->offset;
        if (avail < 0) avail = 0;
        int want = need - y < avail ? need - y : avail;
        const uint8_t *data = (const uint8_t *)G2H((uint32_t)wb->addr);
        if (data && wb->size) {
            if (v->fmt == 2) {  // PCM16 interleaved
                const int16_t *pcm = (const int16_t *)data;
                uint64_t nsamp = wb->size / 2 / v->chans;
                for (; n < want; n++) {
                    uint64_t s = (uint64_t)start + cs->offset + n;
                    if (s >= nsamp) break;
                    out[y + n] = pcm[s * v->chans + ch];
                }
            } else if (v->fmt == 6 && coefs) {
                for (; n < want; n++) out[y + n] = adpcm_next(data, wb->size, start + cs->offset + n, coefs, cs);
            } else if (v->fmt == 3) {  // PCM float
                const float *pcm = (const float *)data;
                uint64_t nsamp = wb->size / 4 / v->chans;
                for (; n < want; n++) {
                    uint64_t s = (uint64_t)start + cs->offset + n;
                    if (s >= nsamp) break;
                    out[y + n] = (int16_t)clampf(pcm[s * v->chans + ch] * 32767.f, -32768.f, 32767.f);
                }
            }
        }
        y += n;
        cs->offset += n;
        cs->played += n;
        if (cs->offset >= total || n == 0) {
            cs->offset = 0;
            if (wb->loop) {
                cs->loop_count++;
                if (wb->loop_count >= 0 && (n == 0 || cs->loop_count > wb->loop_count)) mark_end(cs, wb, &idx);
                if (n == 0) break;
                if (v->behaviour & 1) cs->played = 0;  // PlayedSampleCountResetWhenLooping
            } else {
                mark_end(cs, wb, &idx);
            }
        }
    }
    cs->wb_index = idx;
    return y;
}

// UpdateParametersForCommandGeneration: returns whether the voice should be played this frame.
static int voice_prepare(Renderer *r, Voice *v) {
    int nch = v->chans > 6 ? 6 : v->chans;
    if (v->is_new) {
        for (int c = 0; c < nch; c++) memset(&r->chst[v->chan_ids[c] % r->voices], 0, sizeof(ChanState));
        v->prev_volume = v->volume;
        v->is_new = 0;
    }
    uint32_t vi = (uint32_t)(v - r->voice);
    if (v->flush) {
        if (r->vt) r->vt[vi].dropped += v->flush;
        uint32_t wi = 0;
        for (uint32_t i = 0; i < v->flush; i++) {
            for (int c = 0; c < nch; c++) {
                ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
                if (i == 0) wi = cs->wb_index;
                cs->valid[(wi + i) % WB_COUNT] = 0;
                cs->wb_index = (cs->wb_index + 1) % WB_COUNT;
                cs->consumed++;
            }
            v->wb[(wi + i) % WB_COUNT].sent = 1;
        }
        v->flush = 0;
    }
    if (v->play_state == 0) {
        for (int i = 0; i < WB_COUNT; i++) {
            if (v->wb[i].sent) continue;
            for (int c = 0; c < nch; c++) r->chst[v->chan_ids[c] % r->voices].valid[i] = 1;
            v->wb[i].sent = 1;
        }
        ChanState *p = &r->chst[v->chan_ids[0] % r->voices];
        for (int i = 0; i < WB_COUNT; i++) if (p->valid[i]) return 1;
        return 0;
    }
    if (v->play_state == 2) {  // stopping: drop everything queued
        if (r->vt) {
            ChanState *cs = &r->chst[v->chan_ids[0] % r->voices];
            for (int i = 0; i < WB_COUNT; i++) r->vt[vi].dropped += cs->valid[i] || !v->wb[i].sent;
            vt_close(r, vi, "stopped");
        }
        for (int i = 0; i < WB_COUNT; i++) {
            v->wb[i].sent = 1;
            for (int c = 0; c < nch; c++) {
                ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
                if (cs->valid[i]) { cs->wb_index = (cs->wb_index + 1) % WB_COUNT; cs->consumed++; }
                cs->valid[i] = 0;
            }
        }
        for (int c = 0; c < nch; c++) {
            ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
            cs->offset = 0; cs->played = 0; cs->fraction = 0; cs->loop_count = 0;
            memset(cs->hist, 0, sizeof cs->hist);
            cs->pred_scale = cs->h0 = cs->h1 = 0;
        }
        v->play_state = 1;
    }
    return 0;
}

static Mix *find_mix(Renderer *r, int32_t id) { return id >= 0 && (uint32_t)id < r->nmix && r->mix[id].used ? &r->mix[id] : NULL; }

// Destination k of splitter id (NULL past its list or when not configured).
static SplitDest *split_dest(Renderer *r, uint32_t id, uint32_t k) {
    if (!r->spl || id >= r->splitters || k >= r->spl[id].ndest) return NULL;
    int32_t d = r->spl[id].dest[k];
    if (d < 0 || (uint32_t)d >= r->nsdest || !r->sdest[d].in_use) return NULL;
    return &r->sdest[d];
}

// samples (N) into mix buffers with ramped volumes g0 -> g1 per buffer
static void mix_into(Mix *t, uint32_t N, const float *smp, const float *g0, const float *g1, float k0, float k1) {
    for (uint32_t b = 0; b < t->bufs && b < MIXBUF_MAX; b++) {
        float a = k0 * g0[b], z = k1 * g1[b];
        if (a == 0 && z == 0) continue;
        float *o = t->buf + b * N, dg = (z - a) / N;
        for (uint32_t i = 0; i < N; i++) o[i] += smp[i] * (a + dg * i);
    }
}

// ------------------------------------------------------------------ reverb (port of Ryujinx ReverbState/Command)
typedef struct { float *buf; uint32_t sr, cur, last, count, max; } DelayLine;

static void dl_set(DelayLine *d, float ms) {
    uint32_t n = (uint32_t)roundf(d->sr * ms);
    d->count = n < d->max ? n : d->max;
    d->cur = 0;
    d->last = d->count ? d->count - 1 : 0;
}
static void dl_init(DelayLine *d, uint32_t sr, float ms_max) {
    d->sr = sr;
    d->max = (uint32_t)roundf(sr * ms_max);
    free(d->buf);
    d->buf = calloc(d->max + 1, sizeof(float));
    dl_set(d, ms_max);
}
static float dl_read(DelayLine *d) { return d->buf[d->cur]; }
static float dl_update(DelayLine *d, float v) {
    float o = d->buf[d->cur];
    d->buf[d->cur++] = v;
    if (d->cur >= d->last) d->cur = 0;
    return o;
}
static float dl_tap_unsafe(DelayLine *d, uint32_t idx, int off) {
    int t = (int)d->cur - ((int)idx + off);
    if (t < 0) t += (int)d->count;
    if (t < 0 || (uint32_t)t > d->max) return 0;
    return d->buf[t];
}
static float dl_tap(DelayLine *d, uint32_t idx) {
    if (idx >= d->count) idx = d->count ? d->count - 1 : 0;
    return dl_tap_unsafe(d, idx, -1);
}

typedef struct { DelayLine dl; float rate; } DecayDelay;
static float dd_update(DecayDelay *d, float v) {
    float p = v - d->rate * dl_read(&d->dl);
    return dl_update(&d->dl, p) + p * d->rate;
}

typedef struct {
    uint8_t input[6], output[6];
    uint16_t ch_max, ch;
    int32_t sample_rate, early_mode, early_gain, pre_delay, late_mode, late_gain, decay_time, hf_decay_ratio,
        coloration, reverb_gain, out_gain, dry_gain;
} ReverbParam;

typedef struct {
    int ready;
    uint8_t raw[0x44];  // last parameter bytes seen
    ReverbParam p;
    DelayLine fdn[4], pre, fc;
    DecayDelay decay[4];
    uint32_t early_time[10], pre_time;
    float early_gain[10], hf_direct[4], hf_prev[4], prev_fb[4];
} Reverb;

static const float rv_fdn_times[20] = {53.953247f, 79.192566f, 116.238770f, 130.615295f, 53.953247f, 79.192566f,
    116.238770f, 170.615295f, 5, 10, 5, 10, 47.03f, 71, 103, 170, 53.953247f, 79.192566f, 116.238770f, 170.615295f};
static const float rv_decay_times[20] = {7, 9, 13, 17, 7, 9, 13, 17, 1, 1, 1, 1, 7, 7, 13, 9, 7, 9, 13, 17};
static const float rv_early_times[50] = {0.0f, 3.5f, 2.8f, 3.9f, 2.7f, 13.4f, 7.9f, 8.4f, 9.9f, 12.0f,
    0.0f, 11.8f, 5.5f, 11.2f, 10.4f, 38.1f, 22.2f, 29.6f, 21.2f, 24.8f,
    0.0f, 41.5f, 20.5f, 41.3f, 0.0f, 29.5f, 33.8f, 45.2f, 46.8f, 0.0f,
    33.1f, 43.3f, 22.8f, 37.9f, 14.9f, 35.3f, 17.9f, 34.2f, 0.0f, 43.3f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const float rv_early_gain[50] = {0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f,
    0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f, 0.68f, 0.68f,
    0.50f, 0.70f, 0.70f, 0.68f, 0.50f, 0.68f, 0.68f, 0.70f, 0.68f, 0.00f,
    0.93f, 0.92f, 0.87f, 0.86f, 0.94f, 0.81f, 0.80f, 0.77f, 0.76f, 0.65f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const float rv_pre_times[5] = {12.5f, 40.0f, 50.0f, 50.0f, 0.0f};

static float q14(int32_t v) { return v / 16384.0f; }
static uint32_t rv_rate(const ReverbParam *p) {
    uint32_t sr = (uint32_t)q14(p->sample_rate);  // samples per millisecond
    return sr && sr <= 192 ? sr : 48;
}

static void reverb_update(Reverb *rv) {
    ReverbParam *p = &rv->p;
    uint32_t sr = rv_rate(p);
    float pre_ms = q14(p->pre_delay), early_gain = q14(p->early_gain), coloration = q14(p->coloration);
    float decay_time = q14(p->decay_time);
    int em = p->early_mode >= 0 && p->early_mode < 5 ? p->early_mode : 4;
    int lm = p->late_mode >= 0 && p->late_mode < 5 ? p->late_mode : 0;
    for (int i = 0; i < 10; i++) {
        uint32_t t = (uint32_t)roundf(sr * (rv_early_times[i] + pre_ms));
        rv->early_time[i] = (t < rv->pre.max ? t : rv->pre.max) + 1;
        rv->early_gain[i] = rv_early_gain[i] * early_gain;
    }
    if (p->ch == 2) { rv->early_gain[4] *= 0.5f; rv->early_gain[5] *= 0.5f; }
    uint32_t pt = (uint32_t)roundf(sr * (rv_pre_times[em] + pre_ms));
    rv->pre_time = pt < rv->pre.max ? pt : rv->pre.max;
    float hf_ratio = q14(p->hf_decay_ratio), hf_cos = cosf(1280.0f / sr);
    for (int i = 0; i < 4; i++) {
        dl_set(&rv->fdn[i], rv_fdn_times[lm * 4 + i]);
        dl_set(&rv->decay[i].dl, rv_decay_times[lm * 4 + i]);
        float a = -3.0f * (rv->decay[i].dl.count + rv->fdn[i].count);
        float b = decay_time > 0 ? a / (decay_time * sr) : -1000.0f;
        float c, d;
        if (hf_ratio < 0.995f && hf_ratio > 0) {
            float e = powf(10.0f, (((1.0f / hf_ratio) - 1.0f) * 2) / 100 * (b / 10));
            float f = 1.0f - e, g = 2.0f - (e * 2 * hf_cos);
            float h = sqrtf(fmaxf(0, g * g - f * f * 4));
            c = f != 0 ? (g - h) / (f * 2) : 0;
            d = 1.0f - c;
        } else {
            c = 0;
            d = 1;
        }
        rv->hf_direct[i] = powf(10.0f, b / 1000) * d * 0.7071f;
        rv->hf_prev[i] = c;
        rv->prev_fb[i] = 0;
        rv->decay[i].rate = 0.6f * (1.0f - coloration);
    }
}

static void reverb_init(Reverb *rv) {
    uint32_t sr = rv_rate(&rv->p);
    for (int i = 0; i < 4; i++) {
        dl_init(&rv->fdn[i], sr, rv_fdn_times[16 + i]);
        dl_init(&rv->decay[i].dl, sr, rv_decay_times[16 + i]);
    }
    dl_init(&rv->pre, sr, 150.0f);  // long pre-delay needs REV3+... 350 ms
    dl_init(&rv->fc, sr, 5.0f);
    reverb_update(rv);
    rv->ready = 1;
}

static void reverb_process(Reverb *rv, float *const *in, float *const *out, uint32_t n) {
    static const int8_t oe1[10] = {0}, te[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static const int8_t o1[4] = {0}, tf1[4] = {0, 1, 2, 3};
    static const int8_t oe2[10] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1}, o2[4] = {0, 0, 1, 1}, tf2[4] = {2, 0, 3, 1};
    static const int8_t oe4[10] = {0, 0, 1, 1, 0, 1, 2, 2, 3, 3}, o4[4] = {0, 1, 2, 3}, tf4[4] = {0, 1, 2, 3};
    static const int8_t oe6[20] = {0, 5, 0, 5, 1, 5, 1, 5, 4, 5, 4, 5, 2, 5, 2, 5, 3, 5, 3, 5};
    static const int8_t te6[20] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9};
    static const int8_t o6[6] = {0, 1, 2, 3, 4, 5}, tf6[6] = {0, 1, 2, 3, -1, 3};
    int ch = rv->p.ch;
    const int8_t *oe, *tel, *o, *tf;
    int ne, nf;
    switch (ch) {
    case 1: oe = oe1; tel = te; o = o1; tf = tf1; ne = 10; nf = 4; break;
    case 2: oe = oe2; tel = te; o = o2; tf = tf2; ne = 10; nf = 4; break;
    case 4: oe = oe4; tel = te; o = o4; tf = tf4; ne = 10; nf = 4; break;
    case 6: oe = oe6; tel = te6; o = o6; tf = tf6; ne = 20; nf = 6; break;
    default: return;
    }
    float reverb_gain = q14(rv->p.reverb_gain), late_gain = q14(rv->p.late_gain);
    float out_gain = q14(rv->p.out_gain), dry_gain = q14(rv->p.dry_gain);
    for (uint32_t s = 0; s < n; s++) {
        float ov[6] = {0}, fb[4], fo[4], cin[6];
        for (int i = 0; i < ne; i++) ov[oe[i]] += dl_tap_unsafe(&rv->pre, rv->early_time[tel[i]], 0) * rv->early_gain[tel[i]];
        if (ch == 6) ov[5] *= 0.2f;
        float pre = 0;
        for (int c = 0; c < ch; c++) { cin[c] = in[c][s] * 64; pre += cin[c] * reverb_gain; }
        dl_update(&rv->pre, pre);
        float late = dl_tap(&rv->pre, rv->pre_time) * late_gain;
        for (int i = 0; i < 4; i++) {
            fo[i] = dl_read(&rv->fdn[i]) * rv->hf_direct[i] + rv->prev_fb[i] * rv->hf_prev[i];
            rv->prev_fb[i] = fo[i];
        }
        fb[0] = fo[2] + fo[1];
        fb[1] = -fo[0] - fo[3];
        fb[2] = fo[0] - fo[3];
        fb[3] = fo[1] - fo[2];
        for (int i = 0; i < 4; i++) {
            fo[i] = dd_update(&rv->decay[i], fb[i] + late);
            dl_update(&rv->fdn[i], fo[i]);
        }
        for (int i = 0; i < nf; i++) if (tf[i] >= 0) ov[o[i]] += fo[tf[i]];
        if (ch == 6) ov[4] += dl_update(&rv->fc, (fo[2] - fo[3]) * 0.5f);
        for (int c = 0; c < ch; c++) out[c][s] = (ov[c] * out_gain + cin[c] * dry_gain) / 64;
    }
}

// ------------------------------------------------------------------ I3DL2 reverb (port of Ryujinx Reverb3dState/Command)
// Its delay lines (DelayLine3d) write at `last` and read at `cur`, unlike the standard reverb's. They are rings of
// max + 1 samples as in yuzu: Ryujinx wraps the cursors at max, so slot max keeps its first sample, which the pre-delay
// taps then re-read every 400 ms (a click when the effect starts on sound).
typedef struct { float *buf; uint32_t sr, cur, last, count, max; } DelayLine3d;

static void dl3_set(DelayLine3d *d, float ms) {
    uint32_t n = (uint32_t)roundf(d->sr * ms);
    if (n <= d->max) { d->count = n; d->last = (d->cur + n) % (d->max + 1); }
}
static void dl3_init(DelayLine3d *d, uint32_t sr, float ms_max) {
    d->sr = sr;
    d->max = (uint32_t)roundf(sr * ms_max);
    free(d->buf);
    d->buf = calloc(d->max + 1, sizeof(float));
    d->cur = d->last = 0;
    dl3_set(d, ms_max);
}
static float dl3_read(DelayLine3d *d) { return d->buf[d->cur]; }
static float dl3_update(DelayLine3d *d, float v) {
    d->buf[d->last++] = v;
    float o = d->buf[d->cur++];
    if (d->cur > d->max) d->cur = 0;
    if (d->last > d->max) d->last = 0;
    return o;
}
static float dl3_tap(DelayLine3d *d, uint32_t idx, int off) {
    int t = (int)d->last - ((int)idx + off);
    if (t < 0) t += (int)d->max + 1;
    return t >= 0 && (uint32_t)t <= d->max ? d->buf[t] : 0;
}

typedef struct { DelayLine3d dl; float rate; } DecayDelay3d;
static float dd3_update(DecayDelay3d *d, float v) {
    float p = v - d->rate * dl3_read(&d->dl);
    return dl3_update(&d->dl, p) + p * d->rate;
}

typedef struct {  // Reverb3dParameter (0x48 bytes, then the status byte)
    uint8_t input[6], output[6];
    uint16_t ch_max, ch;
    uint32_t reserved, sample_rate;
    float room_hf, hf_reference, decay_time, hf_decay_ratio, room_gain, reflections_gain, reverb_gain, diffusion,
        reflection_delay, reverb_delay, density, dry_gain;
} I3dl2Param;

typedef struct {
    int ready;
    uint8_t raw[0x48];
    I3dl2Param p;
    DelayLine3d fdn[4], pre, fc;
    DecayDelay3d decay1[4], decay2[4];
    uint32_t early_time[20], refl_time;
    float dry_gain, prev_pre, prev_pre_gain, target_pre_gain, early_refl_gain, late_gain;
    float direct_fdn[4], cur_fdn[4], cur_out[4], prev_fb[4];
} I3dl2;

static const float i3_fdn_min[4] = {5.0f, 6.0f, 13.0f, 14.0f};
static const float i3_fdn_max[4] = {45.704f, 82.782f, 149.94f, 271.58f};
static const float i3_decay_max1[4] = {17.0f, 13.0f, 9.0f, 7.0f};
static const float i3_decay_max2[4] = {19.0f, 11.0f, 10.0f, 6.0f};
static const float i3_early_times[20] = {0.017136f, 0.059154f, 0.16173f, 0.39019f, 0.42526f, 0.45541f, 0.68974f,
    0.74591f, 0.83384f, 0.8595f, 0.0f, 0.075024f, 0.16879f, 0.2999f, 0.33744f, 0.3719f, 0.59901f, 0.71674f, 0.81786f,
    0.85166f};
static const float i3_early_gain[20] = {0.67096f, 0.61027f, 1.0f, 0.35680f, 0.68361f, 0.65978f, 0.51939f, 0.24712f,
    0.45945f, 0.45021f, 0.64196f, 0.54879f, 0.92925f, 0.38270f, 0.72867f, 0.69794f, 0.5464f, 0.24563f, 0.45214f,
    0.44042f};

// FloatingPointHelper: Pow10 limited to Nintendo's LUT range, Cos / Sin in degrees.
static float i3_pow10(float x) { return x >= 0 ? 1.0f : x <= -5.3f ? 0.0f : powf(10.0f, x); }
static float i3_cos(float deg) { return cosf(deg * (float)M_PI / 180); }
static float i3_sin(float deg) { return sinf(deg * (float)M_PI / 180); }
static uint32_t i3_rate(const I3dl2Param *p) {  // samples per millisecond
    uint32_t sr = p->sample_rate / 1000;
    return sr && sr <= 192 ? sr : 48;
}

static void i3dl2_update(I3dl2 *rv) {
    I3dl2Param *p = &rv->p;
    uint32_t sr = i3_rate(p);
    float fs = sr * 1000.0f;
    // (out of range values would make the gains NaN / inf for good: clamp to the I3DL2 ranges)
    float decay_time = fmaxf(p->decay_time, 0.1f), hf_ratio = fmaxf(p->hf_decay_ratio, 0.1f);
    float hf_ref = clampf(p->hf_reference, 20.0f, 20000.0f);
    rv->dry_gain = p->dry_gain;
    memset(rv->prev_fb, 0, sizeof rv->prev_fb);
    rv->prev_pre = 0;
    rv->early_refl_gain = i3_pow10(fminf(p->room_gain + p->reflections_gain, 5000.0f) / 2000.0f);
    rv->late_gain = i3_pow10(fminf(p->room_gain + p->reverb_gain, 5000.0f) / 2000.0f);
    float hf_gain = i3_pow10(p->room_hf / 2000.0f);
    if (hf_gain < 1.0f) {
        float a = 1.0f - hf_gain, b = 2.0f - 2.0f * hf_gain * i3_cos(256.0f * hf_ref / fs);
        float c = sqrtf(fmaxf(0, b * b - 4.0f * a * a));
        rv->prev_pre_gain = (b - c) / (2.0f * a);
        rv->target_pre_gain = 1.0f - rv->prev_pre_gain;
    } else {
        rv->prev_pre_gain = 0;
        rv->target_pre_gain = 1;
    }
    uint32_t rt = (uint32_t)roundf(sr * 1000.0f * (p->reflection_delay + p->reverb_delay));
    rv->refl_time = rt < rv->pre.max ? rt : rv->pre.max;
    float echo = 0.6f * p->diffusion * 0.01f;
    for (int i = 0; i < 4; i++) {
        dl3_set(&rv->fdn[i], i3_fdn_min[i] + p->density / 100 * (i3_fdn_max[i] - i3_fdn_min[i]));
        uint32_t n = rv->fdn[i].count + rv->decay1[i].dl.count + rv->decay2[i].dl.count;
        float a = (-60.0f * n) / (decay_time * fs), b = a / hf_ratio;
        float c = i3_cos(64.0f * hf_ref / fs) / i3_sin(64.0f * hf_ref / fs);
        float d = i3_pow10((b - a) / 40.0f), e = i3_pow10((b + a) / 40.0f) * 0.7071f;
        rv->direct_fdn[i] = e * (d * c + 1.0f) / (c + d);
        rv->cur_fdn[i] = e * (1.0f - d * c) / (c + d);
        rv->cur_out[i] = (c - d) / (c + d);
        rv->decay1[i].rate = echo;
        rv->decay2[i].rate = echo * -0.9f;
    }
    for (int i = 0; i < 20; i++) {
        uint32_t t = (uint32_t)roundf(sr * (p->reflection_delay * 1000.0f +
                                            i3_early_times[i] * 1000.0f * (p->reverb_delay * 0.9998f + 0.02f)));
        rv->early_time[i] = t < rv->pre.max ? t : rv->pre.max;
    }
}

static void i3dl2_init(I3dl2 *rv) {
    uint32_t sr = i3_rate(&rv->p);
    for (int i = 0; i < 4; i++) {
        dl3_init(&rv->fdn[i], sr, i3_fdn_max[i]);
        dl3_init(&rv->decay1[i].dl, sr, i3_decay_max1[i]);
        dl3_init(&rv->decay2[i].dl, sr, i3_decay_max2[i]);
    }
    dl3_init(&rv->pre, sr, 400.0f);
    dl3_init(&rv->fc, sr, 5.0f);
    i3dl2_update(rv);
    rv->ready = 1;
}

static void i3dl2_process(I3dl2 *rv, float *const *in, float *const *out, uint32_t n) {
    static const int8_t te[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    static const int8_t oe1[20] = {0}, tf1[1] = {0};
    static const int8_t oe2[20] = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1}, tf2[2] = {0, 1};
    static const int8_t oe4[20] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 3, 3, 3}, tf4[4] = {0, 1, 2, 3};
    // (Ryujinx indexes the surround output table by early delay index, so only its first 20 entries are used)
    static const int8_t oe6[20] = {4, 5, 0, 5, 0, 5, 1, 5, 1, 5, 1, 5, 1, 5, 2, 5, 2, 5, 2, 5};
    static const int8_t te6[40] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
        13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19};
    static const int8_t tf6[6] = {0, 1, 2, 3, -1, 3};
    int ch = rv->p.ch;
    const int8_t *oe, *tel, *tf;
    int ne, nf;
    switch (ch) {
    case 1: oe = oe1; tel = te; tf = tf1; ne = 20; nf = 1; break;
    case 2: oe = oe2; tel = te; tf = tf2; ne = 20; nf = 2; break;
    case 4: oe = oe4; tel = te; tf = tf4; ne = 20; nf = 4; break;
    case 6: oe = oe6; tel = te6; tf = tf6; ne = 40; nf = 6; break;
    default: return;
    }
    for (uint32_t s = 0; s < n; s++) {
        float ov[6] = {0}, cin[6], fo[4], fb[4], val[4];
        float tap = dl3_tap(&rv->pre, rv->refl_time, 1);
        for (int i = 0; i < ne; i++) {
            int k = tel[i];
            ov[oe[k]] += dl3_tap(&rv->pre, rv->early_time[k], 1) * i3_early_gain[k];
        }
        float pre = 0;
        for (int c = 0; c < ch; c++) { cin[c] = in[c][s]; pre += cin[c]; }
        for (int c = 0; c < ch; c++) ov[c] *= rv->early_refl_gain;
        rv->prev_pre = pre * rv->target_pre_gain + rv->prev_pre * rv->prev_pre_gain;
        dl3_update(&rv->pre, rv->prev_pre);
        for (int i = 0; i < 4; i++) {
            float v = dl3_read(&rv->fdn[i]);
            fo[i] = v * rv->direct_fdn[i] + rv->prev_fb[i];
            rv->prev_fb[i] = v * rv->cur_fdn[i] + fo[i] * rv->cur_out[i];
        }
        fb[0] = fo[2] + fo[1];
        fb[1] = -fo[0] - fo[3];
        fb[2] = fo[0] - fo[3];
        fb[3] = fo[1] - fo[2];
        for (int i = 0; i < 4; i++) {
            val[i] = dd3_update(&rv->decay2[i], dd3_update(&rv->decay1[i], tap * rv->late_gain + fb[i]));
            dl3_update(&rv->fdn[i], val[i]);
        }
        for (int c = 0; c < nf; c++) if (tf[c] >= 0) out[c][s] = ov[c] + val[tf[c]] + cin[c] * rv->dry_gain;
        if (ch == 1) out[0][s] += val[1];
        if (ch == 6) out[4][s] += ov[4] + dl3_update(&rv->fc, (val[2] - val[3]) * 0.5f) + cin[4] * rv->dry_gain;
    }
}

struct Effect {
    uint8_t type, enabled;
    int32_t mix_id;
    uint32_t order;
    Reverb rv;
    I3dl2 i3;
    struct { int8_t in[24], out[24]; float vol[24]; uint32_t n; } bm;  // BufferMixer (type 1)
    struct { int8_t in[24], out[24]; uint32_t ch, count_max, send, ret; } aux;  // Aux (type 2): guest info addresses
};

// ------------------------------------------------------------------ Aux (port of Ryujinx AuxiliaryBufferCommand)
// The send / return buffers are each an AuxiliaryBufferHeader (CPU info 0x40, DSP info 0x40: read offset, write
// offset, lost / total sample counts) followed by `count_max` s32 samples. Every frame each channel's input is
// appended to the send ring (at the DSP write offset, channel c at c * N) and the output read back from the return
// ring, where the game's own CPU-side effect has put its processed samples; the offsets advance by channels * N after
// the last channel.
static uint32_t aux_ring(uint32_t info, uint32_t base, uint32_t count_max, int32_t *smp, uint32_t n, uint32_t off,
                         uint32_t update, int write) {
    if (!count_max || !info) return 0;
    uint32_t *hdr = (uint32_t *)G2H(info);  // [0] read offset, [1] write offset
    uint32_t pos = off + hdr[write];
    if (pos > count_max) return 0;
    for (uint32_t done = 0; done < n;) {
        uint32_t k = count_max - pos < n - done ? count_max - pos : n - done;
        int32_t *ring = (int32_t *)G2H(base + pos * 4);
        if (write) memcpy(ring, smp + done, k * 4);
        else memcpy(smp + done, ring, k * 4);
        pos = (pos + k) % count_max;
        done += k;
    }
    if (update) hdr[write] = (hdr[write] + update) % count_max;
    return n;
}

// MHGU_AUDIO_TRACE: is the game's CPU side keeping pace with this frame? The return slots read now hold what the game
// wrote one ring earlier while its write position stays behind ours by 0..count_max - n ("lag", steady when the game
// keeps up); outside that they hold samples two rings old or ones written just now, a jump in the output. The send
// ring overruns when this frame's write passes over samples the game has not read yet.
static void aux_trace(TraceStats *ts, Effect *fx, uint32_t N) {
    const uint32_t *s = (const uint32_t *)G2H(fx->aux.send), *rt = (const uint32_t *)G2H(fx->aux.ret);
    uint32_t cm = fx->aux.count_max, n = fx->aux.ch * N;
    if (!cm) return;
    ts->aux_ch = fx->aux.ch;
    uint32_t now[4] = {rt[16] % cm, rt[1] % cm, s[17] % cm, s[0] % cm};  // return DSP read / CPU write, send DSP write / CPU read
    if (!ts->aux_seen) {
        ts->aux_seen = 1;
        memcpy(ts->aux_last, now, sizeof now);
        ts->aux_pos[0] = (now[0] + cm - now[1]) % cm;  // assume the game's return writes trail ours
        ts->aux_pos[2] = (now[2] + cm - now[3]) % cm;
        ts->aux_pos[1] = ts->aux_pos[3] = 0;
    }
    for (int i = 0; i < 4; i++) ts->aux_pos[i] += (now[i] + cm - ts->aux_last[i]) % cm, ts->aux_last[i] = now[i];
    int64_t lag = ts->aux_pos[0] - ts->aux_pos[1], unread = ts->aux_pos[2] - ts->aux_pos[3];
    if (lag < 0 || lag + n > cm) ts->aux_glitches++;
    if (unread + n > cm) ts->send_overruns++;
    if (lag < ts->aux_lag_min) ts->aux_lag_min = lag;
    if (lag > ts->aux_lag_max) ts->aux_lag_max = lag;
}

static void aux_process(Effect *fx, Mix *m, uint32_t N, uint64_t frame, TraceStats *ts) {
    uint32_t send_info = fx->aux.send + 0x40, ret_info = fx->aux.ret + 0x40;  // the DSP halves of the headers
    uint32_t send_base = fx->aux.send + 0x80, ret_base = fx->aux.ret + 0x80;
    if (!fx->aux.send || !fx->aux.ret || N > 4096) return;
    if (ts) {
        if (fx->enabled) aux_trace(ts, fx, N);
        else ts->aux_seen = 0;
    }
    int32_t tmp[4096];
    uint32_t wo = 0;
    static int aux_log = -1;
    if (aux_log < 0) aux_log = getenv("MHGU_AUDIO_LOG") ? 1 : 0;
    if (aux_log && frame % 1000 == 0) {
        const uint32_t *s = (const uint32_t *)G2H(fx->aux.send), *r = (const uint32_t *)G2H(fx->aux.ret);
        LOG("[audren] f%llu aux %x (%u ch, enabled %u): send cpu %u/%u dsp %u/%u, return cpu %u/%u dsp %u/%u\n",
            (unsigned long long)frame, fx->aux.send, fx->aux.ch, fx->enabled, s[0], s[1], s[16], s[17], r[0], r[1],
            r[16], r[17]);
    }
    for (uint32_t c = 0; c < fx->aux.ch && c < 24; c++, wo += N) {
        int a = fx->aux.in[c], b = fx->aux.out[c];
        if (a < 0 || b < 0 || (uint32_t)a >= m->bufs || (uint32_t)b >= m->bufs) continue;
        float *src = m->buf + a * N, *dst = m->buf + b * N;
        uint32_t update = c + 1 == fx->aux.ch ? wo + N : 0;
        if (!fx->enabled) {  // reset both rings (read / write offsets, total count), pass the input through
            uint32_t *s = (uint32_t *)G2H(send_info), *r = (uint32_t *)G2H(ret_info);
            s[0] = s[1] = s[3] = r[0] = r[1] = r[3] = 0;
            if (src != dst) memcpy(dst, src, N * sizeof(float));
            continue;
        }
        for (uint32_t i = 0; i < N; i++) tmp[i] = (int32_t)lrintf(clampf(src[i], -2147483648.0f, 2147483520.0f));
        aux_ring(send_info, send_base, fx->aux.count_max, tmp, N, wo, update, 1);
        uint32_t got = aux_ring(ret_info, ret_base, fx->aux.count_max, tmp, N, wo, update, 0);
        for (uint32_t i = 0; i < N; i++) dst[i] = i < got ? (float)tmp[i] : 0;
    }
}

// Effect slot update from EffectInParameterVersion1 (0xC0 bytes: type, is_new, enabled, mix, buffers, order, data).
static void effect_update(Effect *fx, const uint8_t *e) {
    if (fx->type != e[0]) memset(&fx->rv.p, 0, sizeof fx->rv.p), fx->rv.ready = 0, fx->i3.ready = 0;
    fx->type = e[0];
    fx->enabled = e[2];
    memcpy(&fx->mix_id, e + 4, 4);
    memcpy(&fx->order, e + 0x18, 4);
    if (fx->type == 1) {  // BufferMixerParameter: s8 input[24], s8 output[24], f32 volume[24], u32 mix count
        memcpy(fx->bm.in, e + 0x20, 24);
        memcpy(fx->bm.out, e + 0x38, 24);
        memcpy(fx->bm.vol, e + 0x50, 96);
        memcpy(&fx->bm.n, e + 0xB0, 4);
        if (fx->bm.n > 24) fx->bm.n = 24;
        return;
    }
    if (fx->type == 2) {  // AuxiliaryBufferParameter: s8 input[24], s8 output[24], u32 channels, sample rate,
        uint64_t send, ret;  // storage size, channel max, u64 send info, send storage, return info, return storage
        memcpy(fx->aux.in, e + 0x20, 24);
        memcpy(fx->aux.out, e + 0x38, 24);
        memcpy(&fx->aux.ch, e + 0x50, 4);
        memcpy(&fx->aux.count_max, e + 0x58, 4);
        memcpy(&send, e + 0x60, 8);
        memcpy(&ret, e + 0x70, 8);
        if (fx->aux.ch > 24) fx->aux.ch = 24;
        if (getenv("MHGU_AUDIO_LOG") && (fx->aux.send != (uint32_t)send || fx->aux.ret != (uint32_t)ret))
            LOG("[audren] aux: %u channels, %u samples, send %llx (storage %llx) return %llx (storage %llx)\n",
                fx->aux.ch, fx->aux.count_max, (unsigned long long)send, (unsigned long long)*(const uint64_t *)(e + 0x68),
                (unsigned long long)ret, (unsigned long long)*(const uint64_t *)(e + 0x78));
        fx->aux.send = (uint32_t)send;
        fx->aux.ret = (uint32_t)ret;
        return;
    }
    if (fx->type != 4 && fx->type != 5) return;
    const uint8_t *d = e + 0x20;
    uint16_t ch_max, ch;
    memcpy(&ch_max, d + 12, 2);
    memcpy(&ch, d + 14, 2);
    if (ch_max != 1 && ch_max != 2 && ch_max != 4 && ch_max != 6) return;
    if (ch != 1 && ch != 2 && ch != 4 && ch != 6) return;
    if (fx->type == 5) {  // I3DL2: (re)initialised when new, updated when the parameters change (status byte ignored)
        I3dl2 *i3 = &fx->i3;
        if (e[1] || !i3->ready) {
            memcpy(i3->raw, d, sizeof i3->raw);
            memcpy(&i3->p, d, sizeof i3->p);
            if (getenv("MHGU_AUDIO_LOG"))
                LOG("[audren] i3dl2 init: ch %u rate %u room %.0f/%.0f hf ref %.0f decay %.2f/%.2f refl %.0f/%.3f "
                    "reverb %.0f/%.3f diffusion %.0f density %.0f dry %.3f\n", ch, i3->p.sample_rate, i3->p.room_gain,
                    i3->p.room_hf, i3->p.hf_reference, i3->p.decay_time, i3->p.hf_decay_ratio, i3->p.reflections_gain,
                    i3->p.reflection_delay, i3->p.reverb_gain, i3->p.reverb_delay, i3->p.diffusion, i3->p.density,
                    i3->p.dry_gain);
            i3dl2_init(i3);
        } else if (memcmp(i3->raw, d, sizeof i3->raw)) {
            memcpy(i3->raw, d, sizeof i3->raw);
            memcpy(&i3->p, d, sizeof i3->p);
            i3dl2_update(i3);
        }
        return;
    }
    if (e[1] || !fx->rv.ready) {
        memcpy(fx->rv.raw, d, sizeof fx->rv.raw);
        memcpy(&fx->rv.p, d, sizeof fx->rv.p);
        if (getenv("MHGU_AUDIO_LOG"))
            LOG("[audren] reverb init: ch %u rate %d early %d/%d pre %d late %d/%d decay %d hf %d col %d gains %d/%d/%d\n",
                ch, fx->rv.p.sample_rate, fx->rv.p.early_mode, fx->rv.p.early_gain, fx->rv.p.pre_delay,
                fx->rv.p.late_mode, fx->rv.p.late_gain, fx->rv.p.decay_time, fx->rv.p.hf_decay_ratio,
                fx->rv.p.coloration, fx->rv.p.reverb_gain, fx->rv.p.out_gain, fx->rv.p.dry_gain);
        reverb_init(&fx->rv);
    } else if (memcmp(fx->rv.raw, d, 0x40)) {  // parameters changed (status byte at 0x40 ignored)
        memcpy(fx->rv.raw, d, sizeof fx->rv.raw);
        memcpy(&fx->rv.p, d, sizeof fx->rv.p);
        reverb_update(&fx->rv);
    }
}

static void apply_effects(Renderer *r, int32_t mix_id, Mix *m) {
    uint32_t N = r->sample_count;
    for (uint32_t order = 0; order < r->effects; order++) {
        for (uint32_t i = 0; i < r->effects; i++) {
            Effect *fx = &r->fx[i];
            if (fx->type == 1 && fx->enabled && fx->mix_id == mix_id && fx->order == order) {  // buffer mixer
                for (uint32_t k = 0; k < fx->bm.n; k++) {
                    int a = fx->bm.in[k], b = fx->bm.out[k];
                    if (a < 0 || b < 0 || (uint32_t)a >= m->bufs || (uint32_t)b >= m->bufs || fx->bm.vol[k] == 0) continue;
                    float *src = m->buf + a * N, *dst = m->buf + b * N, g = fx->bm.vol[k];
                    for (uint32_t s = 0; s < N; s++) dst[s] += src[s] * g;
                }
                continue;
            }
            if (fx->mix_id != mix_id || fx->order != order) continue;
            if (fx->type == 2) {  // aux
                static int no_aux = -1;
                if (no_aux < 0) no_aux = getenv("MHGU_NO_AUX") ? 1 : 0;
                if (!no_aux) aux_process(fx, m, N, r->elapsed_frames, r->vt ? &r->ts : NULL);
                continue;
            }
            if (!(fx->type == 4 && fx->rv.ready) && !(fx->type == 5 && fx->i3.ready)) continue;
            const uint8_t *pin = fx->type == 4 ? fx->rv.p.input : fx->i3.p.input;
            const uint8_t *pout = fx->type == 4 ? fx->rv.p.output : fx->i3.p.output;
            int ch = fx->type == 4 ? fx->rv.p.ch : fx->i3.p.ch;
            float *in[6], *out[6];
            int ok = 1;
            for (int c = 0; c < ch; c++) {
                if (pin[c] >= m->bufs || pout[c] >= m->bufs) ok = 0;
                in[c] = m->buf + (pin[c] % MIXBUF_MAX) * N;
                out[c] = m->buf + (pout[c] % MIXBUF_MAX) * N;
            }
            if (!ok) continue;
            static int no_reverb = -1;
            if (no_reverb < 0) no_reverb = getenv("MHGU_NO_REVERB") ? 1 : 0;
            static int toggle = -1;
            if (toggle < 0) toggle = getenv("MHGU_REVERB_TOGGLE") ? atoi(getenv("MHGU_REVERB_TOGGLE")) : 0;
            int off = no_reverb || !!getenv("MHGU_NO_REVERB") || (toggle && (r->elapsed_frames / toggle) % 2);
            if (fx->enabled && !off && fx->type == 5) i3dl2_process(&fx->i3, in, out, N);
            else if (fx->enabled && !off) reverb_process(&fx->rv, in, out, N);
            else
                for (int c = 0; c < ch; c++) if (in[c] != out[c]) memcpy(out[c], in[c], N * sizeof(float));
        }
    }
}

static int hf_log = -1;
static double hf[1024];

// One frame of the device sink's six channels (FL FR FC LFE BL BR, as SDL's 5.1), interleaved, 1.0 = full scale
// (not clamped: to_device does that after the stereo fold)
static void render_frame(Renderer *r, float *sink) {
    if (hf_log < 0) hf_log = getenv("MHGU_AUDIO_HF") ? 1 : 0;
    uint32_t N = r->sample_count;
    for (uint32_t m = 0; m < r->nmix; m++)
        if (r->mix[m].buf) memset(r->mix[m].buf, 0, sizeof(float) * MIXBUF_MAX * N);
    int16_t tmp[HIST + 4096];
    float smp[4096];
    uint32_t playing = 0;
    for (uint32_t vi = 0; vi < r->voices; vi++) {
        Voice *v = &r->voice[vi];
        if (!v->in_use) continue;
        int play = voice_prepare(r, v);
        if (!play || v->wb_count == 0 || v->drop) continue;
        Mix *dst = find_mix(r, v->mix_id);
        float ratio = (float)v->rate / r->sample_rate * v->pitch;
        if (!(ratio > 0) || ratio > 8) ratio = 1;
        int nch = v->chans > 6 ? 6 : v->chans;
        for (int c = 0; c < nch; c++) {
            uint32_t rid = v->chan_ids[c] % r->voices;
            ChanState *cs = &r->chst[rid];
            ChanRes *cr = &r->chres[rid];
            int need = (int)(cs->fraction + ratio * N);
            if (need > 4096) need = 4096;
            memcpy(tmp, cs->hist, sizeof cs->hist);
            int got = decode_voice(v, c, cs, tmp + HIST, need);
            if (got < need) memset(tmp + HIST + got, 0, (need - got) * 2);
            VoiceTrace *vtr = r->vt && c == 0 && got ? &r->vt[vi] : NULL;
            int routed = dst != NULL;
            if (vtr) {
                playing++;
                vtr->played += got;
                if (!vtr->first_frame) vtr->first_frame = r->elapsed_frames + 1;
                if (v->volume > vtr->vol_max) vtr->vol_max = v->volume;
            }
            float pos = cs->fraction;
            for (uint32_t i = 0; i < N; i++, pos += ratio) {
                int k = (int)pos;
                float f = pos - k;
                if (k + 3 > need + HIST - 1) k = need + HIST - 4;
                float y0 = tmp[k], y1 = tmp[k + 1], y2 = tmp[k + 2], y3 = tmp[k + 3];
                if (ratio == 1.0f && f == 0) { smp[i] = y1; continue; }
                // 4-point Hermite between y1 and y2
                float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3;
                float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                smp[i] = ((c3 * f + c2) * f + c1) * f + y1;
            }
            for (int fi = 0; fi < 2; fi++) {
                if (!v->bq_on[fi]) continue;
                float *st = cs->bq[fi], *k = v->bq[fi];
                if (v->bq_reset[fi]) st[0] = st[1] = 0;
                for (uint32_t i = 0; i < N; i++) {
                    float in = smp[i], o = in * k[0] + st[0];
                    st[0] = in * k[1] + o * k[3] + st[1];
                    st[1] = in * k[2] + o * k[4];
                    smp[i] = o;
                }
            }
            cs->fraction = cs->fraction + ratio * N - need;
            if (cs->fraction < 0 || cs->fraction >= 1) cs->fraction = 0;
            memcpy(cs->hist, tmp + need, sizeof cs->hist);
            if (hf_log) {
                // Goertzel power around 19.6 kHz (scaled by volume)
                double w = 2 * M_PI * 19582.0 / r->sample_rate, cw = 2 * cos(w), q1 = 0, q2 = 0;
                for (uint32_t i = 0; i < N; i++) { double q0 = cw * q1 - q2 + smp[i]; q2 = q1; q1 = q0; }
                double e = (q1 * q1 + q2 * q2 - q1 * q2 * cw) * v->volume * v->volume / N;
                hf[vi] += e;
            }
            static int solo = -2;
            if (!dst && v->splitter_id != 0xFFFFFFFF) {  // through the voice's splitter
                if (solo >= 0 && (int)vi != solo) continue;
                for (uint32_t k = (uint32_t)c;; k += (uint32_t)nch) {
                    SplitDest *sd = split_dest(r, v->splitter_id, k);
                    if (!sd) { if (!r->spl || v->splitter_id >= r->splitters || k >= r->spl[v->splitter_id].ndest) break; continue; }
                    Mix *t = find_mix(r, sd->mix);
                    if (t) mix_into(t, N, smp, sd->prev, sd->vol, v->prev_volume, v->volume), routed = 1;
                }
                if (vtr && !routed) vtr->unrouted += got;
                continue;
            }
            if (vtr && !routed) vtr->unrouted += got;
            if (!dst) continue;
            if (solo == -2) solo = getenv("MHGU_AUDIO_SOLO") ? atoi(getenv("MHGU_AUDIO_SOLO")) : -1;
            if (solo >= 0 && (int)vi != solo) continue;
            for (uint32_t b = 0; b < dst->bufs && b < MIXBUF_MAX; b++) {
                float g0 = v->prev_volume * cr->prev[b], g1 = v->volume * cr->mix[b];
                if (g0 == 0 && g1 == 0) continue;
                float *o = dst->buf + b * N, dg = (g1 - g0) / N;
                for (uint32_t i = 0; i < N; i++) o[i] += smp[i] * (g0 + dg * i);
            }
        }
        for (int c = 0; c < nch; c++) {
            ChanRes *cr = &r->chres[v->chan_ids[c] % r->voices];
            memcpy(cr->prev, cr->mix, sizeof cr->prev);
        }
        v->bq_reset[0] = v->bq_reset[1] = 0;
        v->prev_volume = v->volume;
    }
    if (hf_log && r->elapsed_frames % 200 == 0) {
        LOG("[audren] hf f%llu:", (unsigned long long)r->elapsed_frames);
        for (uint32_t k = 0; k < r->voices; k++)
            if (hf[k] > 1e6)
                LOG(" v%u=%.0f(%llx bq %d%d %.3f/%.3f fmt %u rate %u pitch %.3f)", k, hf[k] / 1e6,
                    (unsigned long long)r->voice[k].wb[0].addr, r->voice[k].bq_on[0], r->voice[k].bq_on[1],
                    r->voice[k].bq[1][0], r->voice[k].bq[1][3], r->voice[k].fmt, r->voice[k].rate, r->voice[k].pitch);
        LOG("\n");
        memset(hf, 0, sizeof hf);
    }
    if (playing > r->ts.playing_max) r->ts.playing_max = playing;
    for (uint32_t d = 0; d < r->nsdest; d++) memcpy(r->sdest[d].prev, r->sdest[d].vol, sizeof r->sdest[d].prev);
    // sub mixes, deepest first
    int depth[64] = {0};
    int maxd = 0;
    for (uint32_t m = 1; m < r->nmix && m < 64; m++) {
        if (!r->mix[m].used) continue;
        int d = 0;
        for (Mix *x = &r->mix[m]; x && d < 64; x = find_mix(r, x->dest)) { if (x == &r->mix[0]) break; d++; }
        depth[m] = d;
        if (d > maxd) maxd = d;
    }
    for (int d = maxd; d >= 1; d--) {
        for (uint32_t m = 1; m < r->nmix && m < 64; m++) {
            Mix *s = &r->mix[m];
            if (!s->used || depth[m] != d) continue;
            apply_effects(r, (int32_t)m, s);
            Mix *t = find_mix(r, s->dest);
            if (!t && s->dest_splitter != 0xFFFFFFFF) {  // through the mix's splitter
                for (uint32_t i = 0; i < s->bufs; i++)
                    for (uint32_t k = i;; k += s->bufs) {
                        if (!r->spl || s->dest_splitter >= r->splitters || k >= r->spl[s->dest_splitter].ndest) break;
                        SplitDest *sd = split_dest(r, s->dest_splitter, k);
                        Mix *tt = sd ? find_mix(r, sd->mix) : NULL;
                        if (tt && tt != s) mix_into(tt, N, s->buf + i * N, sd->vol, sd->vol, s->volume, s->volume);
                    }
                continue;
            }
            if (!t || t == s) continue;
            for (uint32_t i = 0; i < s->bufs; i++)
                for (uint32_t j = 0; j < t->bufs; j++) {
                    float g = s->volume * s->vol[i * MIXBUF_MAX + j];
                    if (g == 0) continue;
                    float *src = s->buf + i * N, *o = t->buf + j * N;
                    for (uint32_t k = 0; k < N; k++) o[k] += src[k] * g;
                }
        }
    }
    // final mix -> device sink (FL FR FC LFE BL BR). MHGU always gives the sink six inputs and pans its sounds over
    // them (center and rears carry sound in a hunt; LFE stays silent), whatever the device's channel count.
    Mix *f = find_mix(r, 0);
    if (f) apply_effects(r, 0, f);
    for (uint32_t i = 0; i < N; i++) {
        float *ch = sink + i * 6;
        memset(ch, 0, 6 * sizeof *ch);
        if (f && r->sink_used)
            for (int c = 0; c < r->sink_inputs && c < 6; c++)
                if (r->sink_input[c] < f->bufs) ch[c] = f->buf[r->sink_input[c] * N + i] * f->volume / 32768.f;
    }
}

// The sink's 5.1 to the device's channels: 6 as they are, or 2, folded down (center and rears at -3 dB, LFE -12 dB)
static void to_device(const float *sink, float *out, uint32_t n, int channels) {
    for (uint32_t i = 0; i < n; i++, sink += 6) {
        if (channels == 6) {
            for (int c = 0; c < 6; c++) out[i * 6 + c] = clampf(sink[c], -1, 1);
            continue;
        }
        float c = 0.707f * sink[2] + 0.251f * sink[3];
        out[i * 2] = clampf(sink[0] + c + 0.707f * sink[4], -1, 1);
        out[i * 2 + 1] = clampf(sink[1] + c + 0.707f * sink[5], -1, 1);
    }
}

static int output_channels(void) { return config_audio_output() == AUDIO_OUT_51 ? 6 : 2; }
static void apply_gain(SDL_AudioStream *st);

// (Re)opens the SDL stream with `channels` (2, or 6 in SDL's 5.1 order, the sink's own). SDL converts to what the
// device has: a stereo device gets 5.1 folded down by SDL, a 7.1 one gets it on its front / center / rear speakers.
static void open_stream(Renderer *r, int channels) {
    if (r->stream) SDL_DestroyAudioStream(r->stream);
    SDL_AudioSpec spec = {SDL_AUDIO_F32, channels, (int)r->sample_rate};
    r->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    r->out_ch = channels;  // (also after a failure: no retry every frame, only when the setting changes again)
    if (!r->stream) { LOG("[audren] SDL audio open failed (%d channels): %s\n", channels, SDL_GetError()); return; }
    apply_gain(r->stream);
    SDL_ResumeAudioStreamDevice(r->stream);
}

static int64_t queued_frames(Renderer *r) {
    return r->stream ? SDL_GetAudioStreamQueued(r->stream) / (r->out_ch * (int)sizeof(float)) : 0;
}

// Device queue levels for renderer_tick, in sample frames
typedef struct { int dev, target, low, high, prime; } Pacing;
static Pacing pacing(Renderer *r, uint32_t N) {
    Pacing p = {0, 4 * (int)N};  // target: queued audio to keep ahead of the device, >= 2 device periods
    SDL_AudioSpec dev;
    int dev_frames = 0;
    if (r->stream && SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(r->stream), &dev, &dev_frames) && dev_frames > 0) {
        p.dev = dev_frames * (int)r->sample_rate / (dev.freq ? dev.freq : 48000);
        if (2 * p.dev + (int)N > p.target) p.target = 2 * p.dev + (int)N;
        LOG("[audren] output %s; device %d Hz, %d channels, %d frames/period; queue target %d samples\n",
            r->out_ch == 6 ? "5.1" : "stereo", dev.freq, dev.channels, dev_frames, p.target);
    }
    p.low = p.target - p.dev - (int)N;
    p.high = p.target + p.dev + (int)N;
    p.prime = p.target - p.dev / 2;
    return p;
}

// Output gain: menu volume times MHGU_AUDIO_GAIN (applied live).
static void apply_gain(SDL_AudioStream *st) {
    static double env = -1;
    if (env < 0) env = getenv("MHGU_AUDIO_GAIN") ? atof(getenv("MHGU_AUDIO_GAIN")) : 1.0;
    float g = (float)env * config_audio_gain();
    if (st && SDL_GetAudioStreamGain(st) != g) SDL_SetAudioStreamGain(st, g);
}

// MHGU_AUDIO_WAV: 48 kHz float, WAVE_FORMAT_EXTENSIBLE with the channel mask (FL FR, or FL FR FC LFE BL BR), so
// Audacity and the like name a 5.1 dump's six tracks
static void wav_header(FILE *f, uint32_t bytes, int ch) {
    uint32_t h[17] = {0x46464952, 60 + bytes, 0x45564157, 0x20746d66, 40, 0xFFFEu | (uint32_t)ch << 16, 48000,
                      48000u * ch * 4, (uint32_t)ch * 4 | 32u << 16, 22u | 32u << 16, ch == 6 ? 0x3Fu : 0x3u,
                      0x00000003, 0x00100000, 0xaa000080, 0x719b3800,  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                      0x61746164, bytes};
    fseek(f, 0, SEEK_SET);
    fwrite(h, 4, 17, f);
    fseek(f, 0, SEEK_END);
}

static void *renderer_tick(void *arg) {
    Renderer *r = arg;
    uint32_t N = r->sample_count > 4096 ? 4096 : r->sample_count;
    float *sink = calloc((size_t)N * 6, sizeof(float)), *out = calloc((size_t)N * 6, sizeof(float));
    uint64_t next = 0;
    Pacing p = pacing(r, N);
    // One frame per period by the clock, as the DSP renders, so the game gets an update between every two frames and
    // a sound it starts is heard a frame later, not after the next burst of frames (the game ends sounds by its own
    // clock, so that delay came off their ends). The device queue only corrects the clock's drift from the device's:
    // a frame at once when it runs low, a frame skipped when it runs high. Frames go out back to back until the queue
    // first holds half a device period less than the target (the device's pulls then swing it around the target).
    uint64_t period = (uint64_t)N * 1000000000ull / r->sample_rate;
    bool primed = !r->stream;
    while (!r->quit) {
        if (r->output && output_channels() != r->out_ch) {  // [Audio] Output changed in the settings menu
            open_stream(r, output_channels());
            p = pacing(r, N);
            primed = !r->stream, next = 0;
        }
        uint64_t t = plat_now_ns();
        int64_t q = queued_frames(r);
        if (!primed) {
            if (q >= p.prime) primed = true, next = t;
        } else if (!r->stream || q >= p.low) {
            if (!next || t > next + 4 * period) next = t;  // (behind after a stall: no burst to make up for it)
            plat_sleep_until_ns(next);  // (Windows: a high-resolution timer, not nanosleep's whole milliseconds)
            next += period;
            if (r->stream && queued_frames(r) >= p.high) {
                if (r->vt) r->ts.skips++;
                continue;
            }
        } else if (r->vt) {
            r->ts.catch_ups++;
        }
        int64_t queued = r->stream && r->vt ? queued_frames(r) : -1;
        pthread_mutex_lock(&r->lock);
        if (r->state == 0) {
            double t0 = r->vt ? now_ms() : 0;
            render_frame(r, sink);
            r->elapsed_frames++;
            if (r->vt) {
                TraceStats *ts = &r->ts;
                double dt = now_ms() - t0;
                if (dt > ts->max_render_ms) ts->max_render_ms = dt;
                if (++ts->since_update > ts->max_between) ts->max_between = ts->since_update;
                if (queued >= 0 && queued < ts->queue_min) ts->queue_min = queued;
                if (queued == 0 && r->elapsed_frames > r->sample_rate / N) ts->underruns++;  // not counting start-up
            }
        } else {
            memset(sink, 0, (size_t)N * 6 * sizeof(float));
        }
        TraceStats snap;
        uint32_t open = 0;
        int report = r->vt && r->state == 0 && ++r->ts.frames >= r->sample_rate / N;
        if (report) {
            snap = r->ts, trace_reset_second(&r->ts);
            for (uint32_t i = 0; i < r->voices; i++) open += r->vt[i].open;
        }
        pthread_mutex_unlock(&r->lock);
        if (report) {
            char aux[96] = "";
            if (snap.aux_lag_max != INT64_MIN) {
                double ms = 1000.0 / r->sample_rate / (snap.aux_ch ? snap.aux_ch : 1);
                snprintf(aux, sizeof aux, "; aux lag %.1f..%.1f ms, %u glitch frames, %u send overruns",
                         snap.aux_lag_min * ms, snap.aux_lag_max * ms, snap.aux_glitches, snap.send_overruns);
            }
            LOG("[atrace] f%llu 1s: %u frames, %u updates (max %u frames between, longest gap %.1f ms), render max "
                "%.2f ms, queue min %lld, %u underruns, %u caught up, %u skipped%s; voices %u..%u in use, %u open, "
                "max %u playing, %u opened, %u SKIP, %u SILENT, %u CUT\n",
                (unsigned long long)r->elapsed_frames, snap.frames, snap.updates, snap.max_between, snap.max_gap_ms,
                snap.max_render_ms, (long long)(snap.queue_min == INT64_MAX ? -1 : snap.queue_min), snap.underruns,
                snap.catch_ups, snap.skips, aux, snap.in_use_min == UINT32_MAX ? 0 : snap.in_use_min, snap.in_use_max,
                open, snap.playing_max, snap.started, snap.skipped, snap.silent, snap.cut);
        }
        if (r->stream) {
            to_device(sink, out, N, r->out_ch);
            apply_gain(r->stream);
            SDL_PutAudioStreamData(r->stream, out, (int)(N * r->out_ch * sizeof(float)));
        }
        if (r->wav && r->state == 0) {  // (in the Output the dump was opened with, whatever the device does since)
            to_device(sink, out, N, r->wav_ch);
            uint32_t bytes = N * r->wav_ch * (uint32_t)sizeof(float);
            fwrite(out, 1, bytes, r->wav);
            r->wav_bytes += bytes;
            if ((r->elapsed_frames & 255) == 0) { wav_header(r->wav, r->wav_bytes, r->wav_ch); fflush(r->wav); }
        }
        pthread_mutex_lock(&g_klock);
        kernel_signal(r->event);
        pthread_mutex_unlock(&g_klock);
    }
    free(sink), free(out);
    return NULL;
}

static void update_voice(Renderer *r, Voice *v, const uint8_t *p, uint8_t *out) {
    uint8_t is_new = p[8];
    uint32_t vi = (uint32_t)(v - r->voice);
    if (is_new) {
        if (r->vt) vt_close(r, vi, "reused");
        memset(v, 0, sizeof *v);
        v->play_state = 1;
        v->mix_id = -1;
        v->splitter_id = 0xFFFFFFFF;
        for (int i = 0; i < WB_COUNT; i++) v->wb[i].sent = 1;
        v->is_new = 1;
    }
    v->in_use = 1;
    uint8_t ps = p[10];
    if (ps == 0) v->play_state = 0;
    else if (ps == 1) { if (v->play_state != 1) v->play_state = 2; }
    else if (ps == 2) v->play_state = 3;
    v->fmt = p[11];
    memcpy(&v->rate, p + 12, 4);
    uint32_t ch;
    memcpy(&ch, p + 24, 4);
    v->chans = ch > 6 ? 6 : (uint8_t)ch;
    memcpy(&v->pitch, p + 28, 4);
    memcpy(&v->volume, p + 32, 4);
    memcpy(&v->wb_count, p + 60, 4);
    memcpy(&v->ds_addr, p + 72, 8);
    memcpy(&v->mix_id, p + 88, 4);
    memcpy(&v->splitter_id, p + 92, 4);
    memcpy(v->chan_ids, p + 320, 24);
    v->flush += p[345];
    memcpy(&v->behaviour, p + 348, 2);
    if (p[344]) v->drop = 0;
    for (int f = 0; f < 2; f++) {  // BiquadFilterParameter: enable, reserved, s16 numerator[3], denominator[2] (Q14)
        const uint8_t *b = p + 36 + f * 12;
        if (b[0] && !v->bq_on[f]) {
            v->bq_reset[f] = 1;
            if (getenv("MHGU_AUDIO_LOG")) {
                int16_t k[5];
                memcpy(k, b + 2, 10);
                LOG("[audren] v%ld biquad%d on: %d %d %d / %d %d\n", (long)(v - r->voice), f, k[0], k[1], k[2], k[3], k[4]);
            }
        }
        v->bq_on[f] = b[0];
        int16_t c[5];
        memcpy(c, b + 2, 10);
        for (int k = 0; k < 5; k++) v->bq[f][k] = c[k] / 16384.0f;
    }
    ChanState *cs0 = &r->chst[v->chan_ids[0] % r->voices];
    if (is_new)
        for (int c = 0; c < v->chans; c++) memset(r->chst[v->chan_ids[c] % r->voices].valid, 0, WB_COUNT);
    for (int i = 0; i < WB_COUNT; i++) {
        const uint8_t *w = p + 96 + i * 0x38;
        WaveBuf *wb = &v->wb[i];
        if (!cs0->valid[i] && wb->sent) wb->addr = 0;
        if (!w[26]) {  // !SentToServer: a new buffer for the DSP
            memcpy(&wb->addr, w, 8);
            memcpy(&wb->size, w + 8, 8);
            memcpy(&wb->start, w + 16, 4);
            memcpy(&wb->end, w + 20, 4);
            wb->loop = w[24];
            wb->eos = w[25];
            memcpy(&wb->loop_count, w + 28, 4);
            memcpy(&wb->ctx, w + 32, 8);
            memcpy(&wb->loop_start, w + 48, 4);
            memcpy(&wb->loop_end, w + 52, 4);
            if ((r->revision >> 24) - '0' < 8) {  // WaveBuffer version 1: loop forever over [start, end)
                wb->loop_count = -1;
                wb->loop_start = wb->loop_end = 0;
            }
            wb->sent = 0;
            if (r->vt) vt_open(r, vi, v, wb);
            static int dumped;
            if (getenv("MHGU_AUDIO_DUMPWB") && !dumped && wb->size > 0x100000 && v->fmt == 6 && v->ds_addr) {
                dumped = 1;
                FILE *f = fopen("build/wb_dump.bin", "wb");
                if (f) {
                    uint32_t hdr[4] = {(uint32_t)wb->size, wb->start, wb->end, (uint32_t)(wb->ctx != 0)};
                    fwrite(hdr, 4, 4, f);
                    fwrite((const void *)G2H((uint32_t)v->ds_addr), 1, 32, f);
                    int16_t ctx[3] = {0};
                    if (wb->ctx) memcpy(ctx, (const void *)G2H((uint32_t)wb->ctx), 6);
                    fwrite(ctx, 2, 3, f);
                    fwrite((const void *)G2H((uint32_t)wb->addr), 1, wb->size, f);
                    fclose(f);
                    LOG("[audren] dumped wave buffer %llx (%llx bytes) ds %llx ctx %d %d %d\n", (unsigned long long)wb->addr,
                        (unsigned long long)wb->size, (unsigned long long)v->ds_addr, ctx[0], ctx[1], ctx[2]);
                }
            }
            static int vlog = -1;
            if (vlog < 0) vlog = getenv("MHGU_AUDIO_LOG") ? 1 : 0;
            if (vlog)
                LOG("[audren] f%llu v%ld wb%d fmt %u rate %u ch %u pitch %.3f vol %.3f mix %d res %u addr %llx size %llx "
                    "[%u,%u) loop %u/%d eos %u ctx %llx ds %llx\n", (unsigned long long)r->elapsed_frames,
                    (long)(v - r->voice), i, v->fmt, v->rate, v->chans, v->pitch, v->volume, v->mix_id, v->chan_ids[0],
                    (unsigned long long)wb->addr, (unsigned long long)wb->size, wb->start, wb->end, wb->loop,
                    wb->loop_count, wb->eos, (unsigned long long)wb->ctx, (unsigned long long)v->ds_addr);
            if (vlog && v->ds_addr) {
                const int16_t *k = (const int16_t *)G2H((uint32_t)v->ds_addr);
                LOG("[audren]   coefs v%ld:", (long)(v - r->voice));
                for (int j = 0; j < 19; j++) LOG(" %d", k[j]);
                LOG("\n");
            }
        }
    }
    if (r->vt && r->vt[vi].open && v->play_state == 0) r->vt[vi].started = 1;
    // VoiceOutStatus
    if (is_new || v->is_new) {
        memset(out, 0, 0x10);
    } else {
        memcpy(out, &cs0->played, 8);
        memcpy(out + 8, &cs0->consumed, 4);
        out[12] = v->drop;
    }
}

static void update_mix(Renderer *r, const uint8_t *p, int id) {
    if (id < 0 || (uint32_t)id >= r->nmix) return;
    Mix *m = &r->mix[id];
    memcpy(&m->volume, p, 4);
    memcpy(&m->bufs, p + 8, 4);
    if (m->bufs > MIXBUF_MAX) m->bufs = MIXBUF_MAX;
    m->used = p[12];
    memcpy(&m->dest, p + 0x924, 4);
    memcpy(&m->dest_splitter, p + 0x928, 4);
    memcpy(m->vol, p + 0x24, sizeof m->vol);
    if (!m->buf) m->buf = calloc(MIXBUF_MAX * r->sample_count, sizeof(float));
    if (getenv("MHGU_AUDIO_LOG")) {
        LOG("[audren] mix %d used %u vol %.3f bufs %u dest %d:", id, m->used, m->volume, m->bufs, m->dest);
        for (int i = 0; i < MIXBUF_MAX * MIXBUF_MAX; i++)
            if (m->vol[i] != 0) LOG(" %d>%d=%.3f", i / MIXBUF_MAX, i % MIXBUF_MAX, m->vol[i]);
        LOG("\n");
    }
}

// RequestUpdate: input is an UpdateDataHeader + sections; write a matching output.
static void request_update(Renderer *r, IpcReq *req, IpcResp *resp) {
    IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
    if (!in || !out || !in->size || !out->size) return;
    const uint8_t *ip = (const uint8_t *)G2H((uint32_t)in->addr);
    uint8_t *op = (uint8_t *)G2H((uint32_t)out->addr);
    const uint32_t *h = (const uint32_t *)ip;
    uint32_t behavior_sz = h[1], pools_sz = h[2], voices_sz = h[3], chans_sz = h[4], effects_sz = h[5];
    uint32_t mixes_sz = h[6], sinks_sz = h[7], perf_sz = h[8];
    {
        static int dump_n = -1;
        static long count;
        if (dump_n < 0) dump_n = getenv("MHGU_AUDIO_DUMP") ? 1 : 0;
        count++;
        if (dump_n && (count == 300 || count == 2000 || count == 6000 || count == 12000)) {
            char path[64];
            snprintf(path, sizeof path, "build/audren_in_%ld.bin", count);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(ip, 1, in->size, f); fclose(f); }
            LOG("[audren] dumped %s (%llu bytes, rev %08x)\n", path, (unsigned long long)in->size, r->revision);
        }
    }
    uint32_t npools = pools_sz / 0x20, nvoices = voices_sz / 0x170, neffects = effects_sz / 0xC0;
    uint32_t nchans = chans_sz / 0x70, nsinks = sinks_sz / 0x140;
    if (nvoices > r->voices) nvoices = r->voices;
    if (nchans > r->voices) nchans = r->voices;
    memset(op, 0, out->size);
    uint32_t *oh = (uint32_t *)op;
    oh[0] = h[0];
    uint32_t o = 0x40;
    pthread_mutex_lock(&r->lock);
    if (r->vt) {
        double t = now_ms();
        if (r->ts.last_update_ms && t - r->ts.last_update_ms > r->ts.max_gap_ms) r->ts.max_gap_ms = t - r->ts.last_update_ms;
        r->ts.last_update_ms = t;
        r->ts.updates++;
        r->ts.updates_total++;
        r->ts.since_update = 0;
    }
    // memory pools: acknowledge attach/detach requests
    const uint8_t *pin = ip + 0x40 + behavior_sz;
    oh[2] = npools * 0x10;
    for (uint32_t i = 0; i < npools && o + 0x10 <= out->size; i++, o += 0x10) {
        uint32_t st = *(const uint32_t *)(pin + i * 0x20 + 0x10);
        *(uint32_t *)(op + o) = st == 4 ? 5 : st == 2 ? 3 : st;
    }
    // voice channel resources
    const uint8_t *cin = pin + pools_sz;
    for (uint32_t i = 0; i < nchans; i++) {
        const uint8_t *c = cin + i * 0x70;
        uint32_t id;
        memcpy(&id, c, 4);
        if (id >= r->voices) continue;
        r->chres[id].used = c[0x64];
        if (c[0x64]) memcpy(r->chres[id].mix, c + 4, sizeof r->chres[id].mix);
    }
    // voices
    const uint8_t *vin = cin + chans_sz;
    oh[3] = r->voices * 0x10;
    for (uint32_t i = 0; i < r->voices; i++) r->voice[i].in_use = 0;
    for (uint32_t i = 0; i < nvoices && o + 0x10 <= out->size; i++, o += 0x10) {
        const uint8_t *v = vin + i * 0x170;
        if (v[9]) update_voice(r, &r->voice[i], v, op + o);
    }
    if (r->vt) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < r->voices; i++)
            if (!r->voice[i].in_use) vt_close(r, i, "freed");
            else used++;
        if (used < r->ts.in_use_min) r->ts.in_use_min = used;
        if (used > r->ts.in_use_max) r->ts.in_use_max = used;
    }
    // effects: report them as enabled / disabled per request
    const uint8_t *ein = vin + voices_sz;
    oh[5] = neffects * 0x10;
    for (uint32_t i = 0; i < neffects && o + 0x10 <= out->size; i++, o += 0x10) {
        const uint8_t *e = ein + i * 0xC0;
        op[o] = e[2] ? 3 : 4;  // EffectState Enabled / Disabled
        if (i < r->effects) effect_update(&r->fx[i], e);
        static uint32_t seen_types;
        if (e[0] && e[0] < 32 && !(seen_types & (1u << e[0]))) {
            seen_types |= 1u << e[0];
            bool known = e[0] == 1 || e[0] == 2 || e[0] == 4 || e[0] == 5;
            LOG("[audren] effect type %u used%s: slot %u mix %d enabled %u\n", e[0], known ? "" : " (not implemented)", i,
                *(const int32_t *)(e + 4), e[2]);
        }
        if (e[0] && getenv("MHGU_AUDIO_LOG")) {
            static uint8_t last[16][0xC0];
            if (i < 16 && memcmp(last[i], e, 0xC0)) {
                memcpy(last[i], e, 0xC0);
                LOG("[audren] f%llu effect %u:", (unsigned long long)r->elapsed_frames, i);
                for (int k = 0; k < 0x70; k += 4) LOG(" %08x", *(const uint32_t *)(e + k));
                LOG("\n");
            }
        }
    }
    const uint8_t *min = ein + effects_sz;
    if (r->splitters && *(const uint32_t *)min == 0x48444E53) {  // 'SNDH': info count, data count, then SNDI / SNDD
        uint32_t ninfo = *(const uint32_t *)(min + 4), ndata = *(const uint32_t *)(min + 8);
        const uint8_t *q = min + 0x20, *end = ip + h[15];
        if (!r->spl) r->spl = calloc(r->splitters, sizeof *r->spl);
        for (uint32_t i = 0; i < ninfo && q + 16 <= end; i++) {
            if (*(const uint32_t *)q != 0x49444E53) break;  // 'SNDI'
            int32_t id = *(const int32_t *)(q + 4);
            uint32_t n = *(const uint32_t *)(q + 12);
            if (n > 256) n = 256;
            if (id >= 0 && (uint32_t)id < r->splitters) {
                SplitInfo *s = &r->spl[id];
                s->dest = realloc(s->dest, (n ? n : 1) * sizeof *s->dest);
                memcpy(s->dest, q + 16, n * 4);
                s->ndest = n;
            }
            q += 16 + n * 4 + 12;  // (12 unused bytes after the destination ids, as Ryujinx skips)
        }
        for (uint32_t i = 0; i < ndata && q + 0x70 <= end; i++, q += 0x70) {
            if (*(const uint32_t *)q != 0x44444E53) break;  // 'SNDD'
            int32_t id = *(const int32_t *)(q + 4);
            if (id < 0 || id > 65535) continue;
            if ((uint32_t)id >= r->nsdest) {
                uint32_t nn = (uint32_t)id + 64;
                r->sdest = realloc(r->sdest, nn * sizeof *r->sdest);
                memset(r->sdest + r->nsdest, 0, (nn - r->nsdest) * sizeof *r->sdest);
                r->nsdest = nn;
            }
            SplitDest *d = &r->sdest[id];
            bool was = d->in_use;
            memcpy(d->vol, q + 8, sizeof d->vol);
            memcpy(&d->mix, q + 8 + 96, 4);
            d->in_use = q[8 + 96 + 4];
            if (!was) memcpy(d->prev, d->vol, sizeof d->prev);
        }
    }
    // mixes: the section ends right before the sinks
    const uint8_t *sin = ip + h[15] - perf_sz - sinks_sz;
    min = sin - mixes_sz;
    if ((r->revision >> 24) - '0' >= 7 && *(const uint32_t *)min == 0xCAFEBABE) {
        uint32_t n = *(const uint32_t *)(min + 4);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *m = min + 0x20 + i * 0x930;
            int32_t id;
            memcpy(&id, m + 16, 4);
            update_mix(r, m, id);
        }
    } else {
        for (uint32_t i = 0; i < mixes_sz / 0x930; i++) update_mix(r, min + i * 0x930, i);
    }
    // sinks
    oh[7] = nsinks * 0x20;
    for (uint32_t i = 0; i < nsinks; i++, o += 0x20) {
        const uint8_t *s = sin + i * 0x140;
        if (s[0] == 1) {  // device sink
            r->sink_used = s[1];
            uint32_t n;
            memcpy(&n, s + 0x20 + 0x100, 4);
            r->sink_inputs = n > 6 ? 6 : n;
            memcpy(r->sink_input, s + 0x124, 6);
        }
    }
    uint64_t elapsed = r->elapsed_frames;
    pthread_mutex_unlock(&r->lock);
    oh[8] = perf_sz ? 0x10 : 0;
    o += oh[8];
    oh[1] = 0xB0;  // behavior / error info
    o += 0xB0;
    if ((r->revision >> 24) - '0' >= 5) {  // render info (elapsed frame count): REV5 on; the SDK checks the total
        oh[10] = 0x10;
        if (o + 0x10 <= out->size) *(uint64_t *)(op + o) = elapsed;
        o += 0x10;
    }
    oh[15] = o;
    (void)resp;
}

static void renderer_open_output(Renderer *r) {
    if (getenv("MHGU_AUDIO_WAV")) {
        r->wav = fopen(getenv("MHGU_AUDIO_WAV"), "wb");
        r->wav_ch = output_channels();
        if (r->wav) wav_header(r->wav, 0, r->wav_ch);
    }
    if (getenv("MHGU_NO_AUDIO") && strcmp(getenv("MHGU_NO_AUDIO"), "0")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { LOG("[audren] SDL audio init failed: %s\n", SDL_GetError()); return; }
    r->output = true;
    open_stream(r, output_channels());
}

static void audio_renderer(Service *s, IpcReq *req, IpcResp *resp) {
    Renderer *r = s->state;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = r->sample_rate; resp_size(resp, 4); break;
    case 1: OUT(uint32_t, 0) = r->sample_count; resp_size(resp, 4); break;
    case 2: OUT(uint32_t, 0) = r->mix_buffers; resp_size(resp, 4); break;
    case 3: OUT(uint32_t, 0) = r->state; resp_size(resp, 4); break;
    case 4: case 10: request_update(r, req, resp); break;
    case 5: r->state = 0; break;
    case 6: r->state = 1; break;
    case 7: resp->copy_handles[resp->ncopy++] = r->event_r; break;
    case 8: break;
    case 9: OUT(uint32_t, 0) = 100; resp_size(resp, 4); break;
    default: LOG("[audren] IAudioRenderer cmd %u (ok)\n", req->cmd);
    }
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

static void audio_device(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 6: {  // ListAudioDeviceName(Auto)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 0x100) strcpy((char *)G2H((uint32_t)b->addr), "AudioTvOutput");
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    case 3: case 10: {  // GetActiveAudioDeviceName(Auto)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size) strncpy((char *)G2H((uint32_t)b->addr), "AudioTvOutput", b->size);
        break;
    }
    case 4: case 11: case 12: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    case 5: OUT(uint32_t, 0) = 2; resp_size(resp, 4); break;  // GetActiveChannelCount
    case 8: OUT(float, 0) = 1.0f; resp_size(resp, 4); break;
    default: break;
    }
}

typedef struct AudioOut AudioOut;
static void audio_track(Renderer *r, AudioOut *a);

SERVICE("audren:u", audren_u) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // OpenAudioRenderer(params, work size, aruid, pid, transfer mem, process)
        Renderer *r = calloc(1, sizeof *r);
        const uint32_t *p = (const uint32_t *)req->data;
        r->sample_rate = p[0]; r->sample_count = p[1]; r->mix_buffers = p[2]; r->submixes = p[3];
        r->voices = p[4]; r->sinks = p[5]; r->effects = p[6]; r->perf_frames = p[7];
        r->splitters = p[9];
        r->revision = p[12];
        r->state = 1;
        if (r->voices > 1024) r->voices = 1024;
        r->nmix = r->submixes + 1;
        pthread_mutex_init(&r->lock, NULL);
        r->voice = calloc(r->voices, sizeof(Voice));
        r->chres = calloc(r->voices, sizeof(ChanRes));
        r->chst = calloc(r->voices, sizeof(ChanState));
        r->mix = calloc(r->nmix, sizeof(Mix));
        r->fx = calloc(r->effects ? r->effects : 1, sizeof(Effect));
        if (audio_trace()) {
            r->vt = calloc(r->voices, sizeof *r->vt);
            trace_reset_second(&r->ts);
            LOG("[atrace] audio trace on (level %d)\n", audio_trace());
        }
        renderer_open_output(r);
        r->event = kernel_create_event_pair(&r->event_w, &r->event_r);
        pthread_create(&r->thread, NULL, renderer_tick, r);
        audio_track(r, NULL);
        LOG("[audren] renderer %u Hz, %u samples/frame, %u voices, %u mixes, %u mix buffers, %u splitters, %u effects, revision %.4s\n",
            r->sample_rate, r->sample_count, r->voices, r->nmix, r->mix_buffers, r->splitters, r->effects, (const char *)&r->revision);
        ipc_return_object(resp, service_new("IAudioRenderer", audio_renderer, r));
        break;
    }
    case 1: OUT(uint64_t, 0) = 0x1000000; resp_size(resp, 8); break;  // GetWorkBufferSize: 16 MB
    case 2: case 4: ipc_return_object(resp, service_new("IAudioDevice", audio_device, NULL)); break;
    default: LOG("[audren] cmd %u (ok)\n", req->cmd);
    }
}

// ------------------------------------------------------------------ audout (buffers released in real time)
typedef struct AudioOut {
    uint32_t rate, channels, state;
    uint64_t tags[64], sizes[64], datas[64];
    SDL_AudioStream *stream;
    FILE *wav;
    uint32_t wav_bytes;
    int queued, released_n;
    uint64_t released[64];
    uint32_t ev_r, ev_w;
    KEvent *event;
    uint64_t played;
    pthread_mutex_t lock;
    pthread_t thread;
    volatile bool quit;  // reboot (audio_reset)
} AudioOut;

// Every renderer / audio out of this boot: a reboot stops their threads (they read guest memory and signal this
// boot's kernel events) and closes their output streams.
static Renderer *g_rens[8];
static AudioOut *g_outs[8];
static pthread_mutex_t g_track_lock = PTHREAD_MUTEX_INITIALIZER;
static void audio_track(Renderer *r, AudioOut *a) {
    pthread_mutex_lock(&g_track_lock);
    for (int i = 0; i < 8; i++) {
        if (r && !g_rens[i]) { g_rens[i] = r; r = NULL; }
        if (a && !g_outs[i]) { g_outs[i] = a; a = NULL; }
    }
    pthread_mutex_unlock(&g_track_lock);
}

static void audout_free(AudioOut *a) {
    a->quit = true;
    pthread_join(a->thread, NULL);
    if (a->stream) SDL_DestroyAudioStream(a->stream);
    if (a->wav) fclose(a->wav);
    free(a);
}

// The game closes its IAudioOut after every movie (the title's attract loop opens one per showing): stop the
// thread and the output stream then, or each showing leaves both running and fills g_outs.
static void audout_close(Service *s) {
    AudioOut *a = s->state;
    bool mine = false;
    pthread_mutex_lock(&g_track_lock);
    for (int i = 0; i < 8; i++) if (g_outs[i] == a) { g_outs[i] = NULL; mine = true; }
    pthread_mutex_unlock(&g_track_lock);
    if (!mine) return;  // (a reboot already closed it)
    s->state = NULL;
    audout_free(a);
    LOG("[audout] closed\n");
}

RT_ON_RESET(audio_reset) {
    int n = 0;
    for (int i = 0; i < 8; i++) {
        Renderer *r = g_rens[i];
        if (r) {
            r->quit = true;
            pthread_join(r->thread, NULL);
            if (r->stream) SDL_DestroyAudioStream(r->stream);
            if (r->wav) { wav_header(r->wav, r->wav_bytes, r->wav_ch); fclose(r->wav); }
            for (uint32_t m = 0; m < r->nmix; m++) free(r->mix[m].buf);
            free(r->voice), free(r->chres), free(r->chst), free(r->mix), free(r->fx), free(r->vt);  // (reverb delay lines leak)
            free(r);
            g_rens[i] = NULL;
            n++;
        }
        if (g_outs[i]) { audout_free(g_outs[i]); g_outs[i] = NULL; n++; }
    }
    LOG("[audio] reboot: %d outputs closed\n", n);
}

// Buffers are copied into an SDL stream and released once the stream has drained below its target, so the
// stream plays the role of the hardware queue. Without a device, buffers are released in real time.
static void *audout_thread(void *arg) {
    AudioOut *a = arg;
    uint32_t ch = a->channels ? a->channels : 2;
    int target = 0;
    if (a->stream) {
        SDL_AudioSpec dev;
        int dev_frames = 0;
        if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &dev, &dev_frames) || dev_frames <= 0)
            dev_frames = 1024;
        target = 2 * dev_frames * 2 * (int)ch;  // bytes of s16 at the source rate (close enough at 48 kHz)
    }
    while (!a->quit) {
        pthread_mutex_lock(&a->lock);
        uint64_t size = 0, tag = 0, data = 0;
        if (a->state == 0 && a->queued) { size = a->sizes[0]; tag = a->tags[0]; data = a->datas[0]; }
        pthread_mutex_unlock(&a->lock);
        if (!size) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); continue; }
        uint64_t frames = size / (2 * ch);
        if (a->stream) {
            apply_gain(a->stream);
            if (data) SDL_PutAudioStreamData(a->stream, (const void *)G2H((uint32_t)data), (int)size);
            while (SDL_GetAudioStreamQueued(a->stream) > target && !a->quit) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
        } else {
            uint64_t ns = frames * 1000000000ull / (a->rate ? a->rate : 48000);
            struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
        if (a->wav && data) { fwrite((const void *)G2H((uint32_t)data), 1, size, a->wav); a->wav_bytes += (uint32_t)size; }
        pthread_mutex_lock(&a->lock);
        if (a->queued && a->tags[0] == tag) {
            if (a->released_n < 64) a->released[a->released_n++] = a->tags[0];
            memmove(a->tags, a->tags + 1, (a->queued - 1) * 8);
            memmove(a->sizes, a->sizes + 1, (a->queued - 1) * 8);
            memmove(a->datas, a->datas + 1, (a->queued - 1) * 8);
            a->queued--;
            a->played += frames;
        }
        pthread_mutex_unlock(&a->lock);
        pthread_mutex_lock(&g_klock);
        kernel_signal(a->event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

static void audio_out(Service *s, IpcReq *req, IpcResp *resp) {
    AudioOut *a = s->state;
    if (getenv("MHGU_AUDIO_LOG") && req->cmd != 3 && req->cmd != 7) {
        IpcBuf *ob = out_buf(req, 0);
        LOG("[audout] cmd %u out %llx/%llx\n", req->cmd, ob ? (unsigned long long)ob->addr : 0ull,
            ob ? (unsigned long long)ob->size : 0ull);
    }
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = a->state; resp_size(resp, 4); break;   // GetAudioOutState
    case 1: a->state = 0; break;                                      // Start
    case 2: a->state = 1; break;                                      // Stop
    case 3: case 7: {  // AppendAudioOutBuffer(tag) <- AudioOutBuffer {next, samples, capacity, size, offset}
        IpcBuf *b = in_buf(req, 0);
        uint64_t tag = IN(uint64_t, 0);
        uint64_t size = b && b->size >= 0x28 ? ((uint64_t *)G2H((uint32_t)b->addr))[3] : 0;
        if (getenv("MHGU_AUDIO_LOG") && b)
            LOG("[audout] append tag %llx buf %llx size %llx: %llx %llx %llx %llx %llx\n", (unsigned long long)tag,
                (unsigned long long)b->addr, (unsigned long long)b->size, ((unsigned long long *)G2H((uint32_t)b->addr))[0],
                ((unsigned long long *)G2H((uint32_t)b->addr))[1], ((unsigned long long *)G2H((uint32_t)b->addr))[2],
                ((unsigned long long *)G2H((uint32_t)b->addr))[3], ((unsigned long long *)G2H((uint32_t)b->addr))[4]);
        pthread_mutex_lock(&a->lock);
        // AudioOutBuffer {next, samples, capacity, size, offset}
        uint64_t data = b && b->size >= 0x28 ? ((uint64_t *)G2H((uint32_t)b->addr))[1] + ((uint64_t *)G2H((uint32_t)b->addr))[4] : 0;
        if (a->queued < 64) {
            a->tags[a->queued] = tag;
            a->sizes[a->queued] = size ? size : 4096;
            a->datas[a->queued] = size ? data : 0;
            a->queued++;
        }
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case 4: resp->copy_handles[resp->ncopy++] = a->ev_r; break;  // RegisterBufferEvent
    case 5: case 8: {  // GetReleasedAudioOutBuffers -> tags
        IpcBuf *b = out_buf(req, 0);
        pthread_mutex_lock(&a->lock);
        int n = a->released_n;
        int max = b ? (int)(b->size / 8) : 0;
        if (n > max) n = max;
        if (b && b->size) memset((void *)G2H((uint32_t)b->addr), 0, b->size);  // SDK reads entry 0 even when n == 0
        for (int i = 0; i < n; i++) ((uint64_t *)G2H((uint32_t)b->addr))[i] = a->released[i];
        if (getenv("MHGU_AUDIO_LOG") && n) LOG("[audout] released %d (first %llx)\n", n, (unsigned long long)a->released[0]);
        memmove(a->released, a->released + n, (a->released_n - n) * 8);
        a->released_n -= n;
        pthread_mutex_unlock(&a->lock);
        OUT(uint32_t, 0) = (uint32_t)n;
        resp_size(resp, 4);
        break;
    }
    case 6: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 9: OUT(uint32_t, 0) = (uint32_t)a->queued; resp_size(resp, 4); break;
    case 10: OUT(uint64_t, 0) = a->played; resp_size(resp, 8); break;
    case 11: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;
    case 12: break;
    case 13: OUT(float, 0) = 1.0f; resp_size(resp, 4); break;
    default: LOG("[audout] IAudioOut cmd %u (ok)\n", req->cmd);
    }
}

SERVICE("audout:u", audout_u) {
    (void)s;
    switch (req->cmd) {
    case 0: case 2: {  // ListAudioOuts
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 0x100) strcpy((char *)G2H((uint32_t)b->addr), "DeviceOut");
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    case 1: case 3: {  // OpenAudioOut(rate, channels, aruid, name) -> rate, channels, format, state + IAudioOut
        AudioOut *a = calloc(1, sizeof *a);
        a->rate = IN(uint32_t, 0) ? IN(uint32_t, 0) : 48000;
        a->channels = IN(uint16_t, 4) ? IN(uint16_t, 4) : 2;
        if (a->channels != 1 && a->channels != 2 && a->channels != 6) a->channels = 2;
        a->state = 1;
        pthread_mutex_init(&a->lock, NULL);
        a->event = kernel_create_event_pair(&a->ev_w, &a->ev_r);
        if (getenv("MHGU_AUDOUT_WAV")) a->wav = fopen(getenv("MHGU_AUDOUT_WAV"), "wb");
        if (!(getenv("MHGU_NO_AUDIO") && strcmp(getenv("MHGU_NO_AUDIO"), "0")) && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)a->channels, (int)a->rate};
            a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (a->stream) {
                apply_gain(a->stream);
                SDL_ResumeAudioStreamDevice(a->stream);
            }
        }
        pthread_create(&a->thread, NULL, audout_thread, a);
        audio_track(NULL, a);
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size) strncpy((char *)G2H((uint32_t)b->addr), "DeviceOut", b->size);
        OUT(uint32_t, 0) = a->rate;
        OUT(uint32_t, 4) = a->channels;
        OUT(uint32_t, 8) = 2;  // PCM16
        OUT(uint32_t, 12) = 1; // stopped
        resp_size(resp, 16);
        Service *svc = service_new("IAudioOut", audio_out, a);
        svc->on_close = audout_close;
        ipc_return_object(resp, svc);
        LOG("[audout] opened %u Hz, %u ch\n", a->rate, a->channels);
        break;
    }
    default: LOG("[audout] cmd %u (ok)\n", req->cmd);
    }
}
