// Large monster HP by the number of hunters (ini [Monster HP]). A quest gives each large monster a row of the
// difficulty table (resident.arc enemy\resident\em_nando_tbl: rows of 8 floats, the first the HP multiplier, the
// others attack, defense...; a village 5-star Rathalos uses row 14, HP x0.52). The game makes a monster's HP its base
// HP x that multiplier, in two functions that read the row through one getter (0x2baaac: monster, column 0 = HP,
// 1 = attack, 2 = the rest). Hooked there: for the HP column in a Hub-type quest the getter returns a copy of the
// row with only the HP multiplier scaled, so attack, defense, size, stamina... stay as the quest made them.
// Village quests (numbers below 10000, and the village Prowler quests 100000-109999) are left alone: they are
// played alone - unless posted in the Hub or the Hunters' Pub ([Village Quests], runtime/hle/villagehub.c): then
// their HP gets [Village Quests] HubHP first, then the factor for the number of hunters. Every other quest is
// Hub-type: Hub and G Rank, Arena, Special Permit, Event, Hub Prowler quests.
// The number of hunters is the quest's join count (sQuestSelector), as it was when the quest started.
// Multiplayer: every game computes a monster's HP itself, so all must use the same factors: the hub's host sends its
// settings to the others (ldn_send_aux), whose games use them instead of their own while they are in that hub.
#include <pthread.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "../net/net.h"

// Game data (main, v1.4.0; runtime addresses)
#define FN_NANDO_ROW  0x2baaacu  // (monster, column) -> the monster's row of the difficulty table
#define G_SENEMY      0x1a51fe0u  // sEnemy *: +0x257c rEnemyNandoData * (+0x64 rows, +0x68 row count)
#define G_SQUEST      0x1a8ad90u  // sQuest *: +0xa4 cQuestData * of the quest being played (+0x8 quest number)
#define G_SELECTOR    0x1a8ae38u  // sQuestSelector *: +0xd750 the quest's join count (hunters), +0xd74c its maximum
#define G_SPLAYER     0x1a85b58u  // sPlayer *: +0x1c the 4 player slots
#define G_GAMECONTROL 0x1a8ca48u  // sGameControl *: +0x4c area type (0 Hub, 1-6 villages, 7 Hunters' Pub)
#define EN_INFO       0x1428u     // enemy: status block pointer (+0x1b6 u16 network id; +0x39c HP row number)
#define ROW_SIZE      32u
#define RING          16u
#define SYNC_MAGIC    0xE6u       // aux message: the host's settings (emsync.c's claims are 0xE5)
#define SYNC_NS       2000000000ull

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static HpScale g_host;       // the hub host's settings, while we are a client in a hub
static bool g_have_host;
static uint32_t g_ring;      // guest copies of rows (a caller reads its row right away)
static unsigned g_ring_next;
static volatile int g_town = -1;  // the last town's area type seen (a quest is started from there)
static uint32_t g_watch;     // MHGU_LOG_HPSCALE: a monster whose resulting HP is logged a moment later
static uint64_t g_watch_at;

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static bool village_quest(uint32_t q) {
    uint32_t group = q % 1000000u / 10000u;  // (event quests are 1xxxxxx: 101xxxx Hub, 111xxxx Hub Prowler...)
    return group == 0 || group == 10;
}

static uint32_t quest_no(void) {
    uint32_t q = LD32(G_SQUEST);
    if (!sane(q)) return 0;
    uint32_t qd = LD32(q + 0xa4);
    return sane(qd) ? LD32(qd + 8) : 0;
}

static int hunters(void) {
    uint32_t sel = LD32(G_SELECTOR);
    int n = sane(sel) ? (int)LD32(sel + 0xd750) : 0;
    if (n >= 1 && n <= 4) return n;
    uint32_t sp = LD32(G_SPLAYER);  // (no join count: the players there are)
    n = 0;
    for (int i = 0; sane(sp) && i < 4; i++) n += sane(LD32(sp + 0x1c + 4u * (uint32_t)i));
    return n ? n : 1;
}

// The factor for a quest and number of hunters (the host's settings in a hub we joined).
static float factor(uint32_t quest, int n, bool *from_host) {
    HpScale own;  // ([Monster HP] as saved now)
    config_live_hp(&own);
    const HpScale *s = &own;
    pthread_mutex_lock(&g_lock);
    *from_host = g_have_host;
    if (g_have_host) s = &g_host;
    float f = s->scale[n - 1];
    for (int i = 0; i < s->quests; i++)
        if (s->quest[i].quest == quest) f = s->quest[i].scale[n - 1];
    pthread_mutex_unlock(&g_lock);
    return f;
}

// [Village Quests] HubHP (the host's in a hub we joined).
static float village_factor(void) {
    float own = config_live_hp_village();
    pthread_mutex_lock(&g_lock);
    float v = g_have_host ? g_host.village : own;
    pthread_mutex_unlock(&g_lock);
    return v;
}

static float f32(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static uint32_t u32f(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

static void nando_row(Cpu *c) {
    uint32_t em = c->r[0], column = c->r[1];
    fnhook_call_original(c, FN_NANDO_ROW);
    if (column != 0) return;
    uint32_t row = c->r[0], sen = LD32(G_SENEMY), tbl = sane(sen) ? LD32(sen + 0x257c) : 0;
    // (outside quests the getter gives a fixed row, not one of the table's)
    if (!sane(tbl) || row < LD32(tbl + 0x64) || row >= LD32(tbl + 0x64) + LD32(tbl + 0x68) * ROW_SIZE) return;
    uint32_t q = quest_no();
    bool from_hub = g_town == 0 || g_town == 7;
    if (!q || (village_quest(q) && !from_hub)) return;
    int n = hunters();
    bool host;
    float v = village_quest(q) ? village_factor() : 1.0f, f = v * factor(q, n, &host), hp = f32(LD32(row));
    uint32_t info = sane(em) ? LD32(em + EN_INFO) : 0;
    LOG("[hpscale] quest %u, %d hunter(s): monster %u, HP x%.2f (row %u) x %.2f%s%s = x%.3f\n", q, n,
        sane(info) ? LD16(info + 0x1b6) : 0xffffu, hp, (row - LD32(tbl + 0x64)) / ROW_SIZE, f,
        v != 1.0f ? " (Village quest from the Hub)" : "", host ? " (the hub host's setting)" : "", hp * f);
    if (ENV("MHGU_LOG_HPSCALE")) g_watch = info, g_watch_at = plat_now_ns();
    if (f == 1.0f) return;
    uint32_t copy = g_ring + ROW_SIZE * (__atomic_fetch_add(&g_ring_next, 1, __ATOMIC_RELAXED) % RING);
    memcpy(G2H(copy), G2H(row), ROW_SIZE);
    ST32(copy, u32f(hp * f));
    c->r[0] = copy;
}

// ---- the host's settings to the others in the hub
// Message: u8 magic, u8 -, u16 quest overrides; float scale[4]; per override u32 quest, float scale[4]; float HubHP.
void hpscale_tick(void) {
    uint32_t gc = LD32(G_GAMECONTROL);
    if (sane(gc) && LD8(gc + 0x4c) <= 7) g_town = LD8(gc + 0x4c);
    if (g_watch && plat_now_ns() - g_watch_at > 500000000ull) {
        if (sane(g_watch)) LOG("[hpscale] monster %u: max HP %d\n", LD16(g_watch + 0x1b6), (int)LD32(g_watch + 0x374));
        g_watch = 0;
    }
    static uint64_t last;
    static bool was_up;
    bool up = ldn_net_up();
    if (!up && was_up) {
        pthread_mutex_lock(&g_lock);
        if (g_have_host) LOG("[hpscale] left the hub: own [Monster HP] settings again\n");
        g_have_host = false;
        pthread_mutex_unlock(&g_lock);
    }
    was_up = up;
    uint64_t now = plat_now_ns();
    if (!up || !ldn_hosting() || now - last < SYNC_NS) return;
    last = now;
    static uint8_t m[4 + 16 + HP_QUESTS_MAX * 20 + 4];
    static HpScale own;
    config_live_hp(&own);
    const HpScale *s = &own;
    int nq = s->quests;
    m[0] = SYNC_MAGIC, m[1] = 0, m[2] = (uint8_t)nq, m[3] = (uint8_t)(nq >> 8);
    memcpy(m + 4, s->scale, 16);
    for (int i = 0; i < nq; i++) {
        memcpy(m + 20 + 20 * i, &s->quest[i].quest, 4);
        memcpy(m + 24 + 20 * i, s->quest[i].scale, 16);
    }
    memcpy(m + 20 + 20 * nq, &s->village, 4);
    ldn_send_aux(m, 24u + 20u * (uint32_t)nq);
}

void hpscale_aux_received(const void *data, uint32_t len) {
    const uint8_t *m = data;
    if (len < 20 || m[0] != SYNC_MAGIC || ldn_hosting()) return;
    int nq = m[2] | m[3] << 8;
    if (nq > HP_QUESTS_MAX || len < 24u + 20u * (uint32_t)nq) return;
    static HpScale s;  // (one network thread receives)
    memcpy(s.scale, m + 4, 16);
    s.quests = nq;
    for (int i = 0; i < nq; i++) {
        memcpy(&s.quest[i].quest, m + 20 + 20 * i, 4);
        memcpy(s.quest[i].scale, m + 24 + 20 * i, 16);
    }
    memcpy(&s.village, m + 20 + 20 * nq, 4);
    if (!(s.village >= HP_SCALE_MIN && s.village <= HP_SCALE_MAX)) return;
    for (int i = 0; i <= nq; i++)  // (a damaged or hostile message: keep to the range the ini allows)
        for (int k = 0; k < 4; k++) {
            float v = i < nq ? s.quest[i].scale[k] : s.scale[k];
            if (!(v >= HP_SCALE_MIN && v <= HP_SCALE_MAX)) return;
        }
    pthread_mutex_lock(&g_lock);
    bool changed = !g_have_host || memcmp(g_host.scale, s.scale, 16) || g_host.quests != s.quests ||
                   g_host.village != s.village;
    g_host = s;
    g_have_host = true;
    pthread_mutex_unlock(&g_lock);
    if (changed)
        LOG("[hpscale] the hub host's [Monster HP] settings apply: x%.2f / %.2f / %.2f / %.2f for 1-4 hunters, "
            "%d quest(s) set apart, Village quests x%.2f first\n", s.scale[0], s.scale[1], s.scale[2], s.scale[3],
            s.quests, s.village);
}

void hpscale_init(void) {
    g_ring = hle_alloc(ROW_SIZE * RING);
    fnhook_set(FN_NANDO_ROW, hle_register("hpscale:nando_row", nando_row));
}
