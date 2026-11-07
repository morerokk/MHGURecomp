// Village quests at the Hub's and the Hunters' Pub's quest counter (ini [Village Quests] VillageQuestsInHub), so they
// can be played together: L / R in the counter's rank menu or star menu switches between the Hub's quests and the
// Village quests.
//
// Quest numbers: group * 10000 + star * 100 + index (group 0 Village, 1 Hub, 2 Arena, 3 Village training, 4 Special
// Permit...). A counter's quests come from sQuestSelector: when an area loads, a job (main 0x5b7cf8) walks every quest
// of quest\quest_group (village\common.arc) through the filter 0x5b8090(list, quest, counter type) and loads the data
// of those it takes; categories (one per star, Training, Urgent...) are filled from that (0x5b85d8, 0x5bb318) and the
// counter UI (uUIQuestCounter) gets them through 0x5bc96c(selector, slot), which maps slots to categories by a table
// per counter type. The counter type is the area type, sGameControl +0x4c: 0 Hub, 1..6 the villages (all Village
// quests are pooled: one category row for all), 7 the G-rank Hunters' Pub. The counter UI also reads it to lay out its
// menus (which ranks, which stars: a village has Low 1-6 and High 7-10 stars, the Hub Low 1-3, High 4-7 and G).
// In Village mode, all of these see a village while the area is the Hub or the Pub: the filter gets the village
// counter type (the list job runs on a worker thread, so the type is passed, not the area changed), and the others
// run with +0x4c reading 1 for their duration (the game thread; the counter UI only in its menus, see
// as_village_menu). Quests are then accepted, posted and departed on as at the Hub's counter; the game loads a Village
// quest anywhere (its data is in its own arc, quest\q<number>).
// The switch: the list job runs again (as at the area's loading; ~0.4 s the first time, then a few ms) and the UI goes
// back to its first state, which lays out the menu anew. Only in the rank menu (UI state 1) and star menu (2): in the
// quest list L / R page through a quest's details.
// Maps: a Village quest's supply box holds one Map (a Hub quest's four). Started from the Hub or the Pub (the test
// hpscale.c uses for its HP), the box gets a Map in its last 3 slots too, the bottom right. The box (sQuest +0x1a54):
// +4 cQuestData, +0xc 40 slots {u16 item, u16 count}, +0xc0 a bit per slot that is in the box now (delayed
// deliveries' slots are filled at the start and their bits set when they arrive). At a quest's start 0x5abc00 clears
// it, 0x5accbc fills it from the quest's rSupplyList (quest\supp\supp_<number>: 40 slots {item, count, group}, the
// group-0 items from slot 0 on, skipping empty ones) and then 0x5ace14 adds the deliveries' items after them (or from
// slot 32 on, for one kind). The maps go in after that, in the last empty slots: they never take a delivery's place.
//   MHGU_VQ_TRACE=1   log the filter's verdicts (once per counter type and quest group), and the supply box
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "../mods.h"
#include "input.h"

// main v1.4.0, runtime addresses
#define FN_QUEST_FILTER   0x5b8090u   // (list, quest number, counter type) -> listed?
#define FN_CATEGORY_BUILD 0x5b85d8u   // sQuestSelector: categories from the list
#define FN_CATEGORY_FILL  0x5bb318u   // sQuestSelector: category contents
#define FN_CATEGORY_GET   0x5bc96cu   // (selector, slot) -> category
#define FN_UI_ROWS        0x983074u   // uUIQuestCounter: the star rows of a rank
#define FN_UI_UPDATE      0x981e34u   // uUIQuestCounter vtable slot 9
#define FN_UI_DRAW        0x981bb0u   // uUIQuestCounter vtable slot 18
#define FN_LIST_LOAD      0x5b7fd8u   // sQuestSelector: start the list job
#define FN_LIST_DONE      0x5b8070u   // sQuestSelector: the list job has finished
#define FN_CATEGORIES     0x5ba8fcu   // sQuestSelector: build the categories anew (at a stage's start)
#define FN_QUEST_LOOKUP   0x5ba50cu   // (selector, quest number) -> its rQuestData (DLC, loaded, or in the list)
#define FN_QUEST_UNLOCKED 0x74af8cu   // (quest number, 0xff, 0) -> unlocked in this save (the counters' check)
#define G_RESOURCES       0x2326a64u  // the resource manager * (vtable +0x30: getResource(dti, path, mode))
#define DTI_QUESTDATA     0x1a8ab98u  // rQuestData
#define SEL_LIST          0x24u       // sQuestSelector: rQuestData *[LIST_MAX], +0x28 their number
#define SEL_COUNT         0x28u
#define LIST_MAX          0x360u
#define G_GAMECONTROL     0x1a8ca48u  // sGameControl *: +0x4c area type
#define G_SELECTOR        0x1a8ae38u  // sQuestSelector *
#define GC_AREA_TYPE      0x4cu
#define UI_STATE          0x3cu       // uUIQuestCounter: 1 rank menu, 2 star menu, 5 quest list
#define UI_RANK_MENU      1
#define UI_STAR_MENU      2
#define AREA_HUB          0
#define AREA_VILLAGE      1           // (any village: they share the Village quest categories)
#define AREA_PUB          7
#define FN_SUPPLY_DELIVER 0x5ace14u   // (supply box): the delayed deliveries' items, after the box's own (0x5accbc)
#define BOX_QUEST         0x4u        // supply box: cQuestData * (+0x8 quest number)
#define BOX_SLOTS         0xcu        // supply box: 40 x {u16 item, u16 count}
#define BOX_IN            0xc0u       // supply box: a bit per slot in the box now
#define BOX_COUNT         40u
#define ITEM_MAP          0x6b3u      // Map (the supply item)
#define HUB_MAPS          3u          // added: 4 with the quest's own

static bool g_village;  // the counter shows Village quests
static int g_trace;

static bool hub_area(uint8_t a) { return a == AREA_HUB || a == AREA_PUB; }

// The setting is on, or was turned off while the counter shows Village quests: those stay whole until ui_update
// switches back, so a quest list or accept screen that is open isn't changed underneath it.
static bool village_active(void) { return g_village || config_live_village_in_hub(); }

static uint32_t area_addr(void) {
    uint32_t gc = LD32(G_GAMECONTROL);
    return gc ? gc + GC_AREA_TYPE : 0;
}

// Runs the original with the area type reading Village, in Village mode while the area is the Hub or the Pub.
static void as_village(Cpu *c, uint32_t fn) {
    uint32_t a = area_addr();
    uint8_t was = a ? LD8(a) : 0xff;
    bool flip = g_village && hub_area(was);
    if (flip) ST8(a, AREA_VILLAGE);
    fnhook_call_original(c, fn);
    if (flip) ST8(a, was);
}

// Calls a guest function from inside a hook (the hooked call's registers are kept).
static uint32_t call4(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    guest_call(c, fn);
    uint32_t r = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return r;
}
static uint32_t call1(Cpu *c, uint32_t fn, uint32_t a0) { return call4(c, fn, a0, 0, 0, 0); }

// Village quests (and village training / Prowler ones) this save hasn't unlocked. A village's counter lists all
// Village quests of its star ranges and only hides the locked ones in its menus (FN_QUEST_UNLOCKED); joining a posted
// quest from the Quest Board asks the list alone. A village is never online, so the game has no check for joining a
// locked Village quest - in the Hub and Pub they are kept out of the list (and out of the on-demand load below), and
// the board refuses them as it refuses a quest one doesn't meet the requirements for.
static bool locked_village_quest(Cpu *c, uint32_t q) {
    uint32_t group = q / 10000 % 100;
    if (q > 999999 || (group != 0 && group != 3 && group != 10)) return false;
    return !(call4(c, FN_QUEST_UNLOCKED, q, 0xff, 0, 0) & 0xff);
}

static void quest_filter(Cpu *c) {
    uint32_t q = c->r[1], type = c->r[2];
    if (!village_active()) { fnhook_call_original(c, FN_QUEST_FILTER); return; }
    if (g_village && hub_area((uint8_t)type)) c->r[2] = AREA_VILLAGE;
    fnhook_call_original(c, FN_QUEST_FILTER);
    if ((c->r[0] & 1) && hub_area((uint8_t)type) && locked_village_quest(c, q)) c->r[0] = 0;
    if (g_trace) {
        static uint8_t seen[8][10][2];
        uint32_t ok = c->r[0] & 1, group = q / 10000 % 10;
        if (type < 8 && !seen[type][group][ok]) {
            seen[type][group][ok] = 1;
            LOG("[vqhub] filter: counter type %u%s, quest %07u (group %u): %s\n", type,
                g_village && hub_area((uint8_t)type) ? " as a village" : "", q, group, ok ? "listed" : "no");
        }
    }
}

static void category_build(Cpu *c) { as_village(c, FN_CATEGORY_BUILD); }
static void category_fill(Cpu *c) { as_village(c, FN_CATEGORY_FILL); }
static void category_get(Cpu *c) { as_village(c, FN_CATEGORY_GET); }
static void ui_rows(Cpu *c) { as_village(c, FN_UI_ROWS); }

// The counter UI sees a village only while it lays out and shows the rank and star menus (states 0-2). In the quest
// list and while accepting (state 5) it stays the Hub's / Pub's, which posts the quest to the hub like any other.
static void as_village_menu(Cpu *c, uint32_t fn) {
    if (LD8(c->r[0] + UI_STATE) <= UI_STAR_MENU) as_village(c, fn);
    else fnhook_call_original(c, fn);
}
static void ui_draw(Cpu *c) { as_village_menu(c, FN_UI_DRAW); }

// A quest someone else posted in the hub may be from the other list (a Village quest while this counter shows the
// Hub's, or the other way round): joining looks its data up in the list (0x5ba50c(selector, quest number)) and says
// "You do not meet the requirements" when it isn't there. So a quest missing from the list is loaded as the list job
// loads each (the resource loc\quest\questData\questData_<number>) and added to the list, which has room for 864.
static uint32_t g_path;  // (guest memory for the resource path)
static void quest_lookup(Cpu *c) {
    uint32_t sel = c->r[0], q = c->r[1], args[2] = {c->r[0], c->r[1]};
    fnhook_call_original(c, FN_QUEST_LOOKUP);
    uint32_t a = area_addr();
    if (c->r[0] || !village_active() || !sel || !q || q > 9999999 || !a || !hub_area(LD8(a))) return;
    static uint32_t failed[16];  // (asked every frame for what the board shows: don't load what isn't there again)
    for (int i = 0; i < 16; i++) if (failed[i] == q) return;
    if (locked_village_quest(c, q)) {
        static uint32_t said;
        if (said != q) LOG("[vqhub] posted Village quest %07u: not unlocked in this save, can't be joined\n", q);
        said = q;
        return;
    }
    uint32_t list = LD32(sel + SEL_LIST), n = LD32(sel + SEL_COUNT), mgr = LD32(G_RESOURCES);
    if (!list || n >= LIST_MAX || !mgr) return;
    snprintf((char *)G2H(g_path), 64, "loc\\quest\\questData\\questData_%07u", q);
    uint32_t res = call4(c, LD32(LD32(mgr) + 0x30), mgr, DTI_QUESTDATA, g_path, 2);  // (getResource, loading now)
    if (!res) {
        static int next;
        failed[next++ % 16] = q;
        return;
    }
    ST32(list + 4 * n, res);
    ST32(sel + SEL_COUNT, n + 1);
    LOG("[vqhub] quest %07u added to the counter's list (posted in the hub)\n", q);
    c->r[0] = args[0], c->r[1] = args[1];
    fnhook_call_original(c, FN_QUEST_LOOKUP);
}

static void reload_list(Cpu *c) {
    uint32_t sel = LD32(G_SELECTOR);
    if (!sel) return;
    uint64_t t0 = plat_now_ns();
    call1(c, FN_LIST_LOAD, sel);
    while (!call1(c, FN_LIST_DONE, sel) && plat_now_ns() - t0 < 5000000000ull)
        plat_sleep_until_ns(plat_now_ns() + 1000000);
    // the categories (quest numbers per star...) are built anew too, as when a stage starts, for the new mode
    uint32_t a = area_addr();
    uint8_t was = a ? LD8(a) : 0xff;
    if (g_village && hub_area(was)) ST8(a, AREA_VILLAGE);
    call1(c, FN_CATEGORIES, sel);
    if (a) ST8(a, was);
    LOG("[vqhub] quest counter: %s quests (list loaded in %.0f ms)\n", g_village ? "Village" : "Hub",
        (plat_now_ns() - t0) / 1e6);
}

static void ui_update(Cpu *c) {
    static uint64_t held;
    uint64_t b;
    int32_t sticks[4];
    input_get(&b, sticks);
    uint64_t pressed = b & ~held;
    held = b;
    uint32_t ui = c->r[0], a = area_addr();
    uint8_t st = LD8(ui + UI_STATE);
    bool on = config_live_village_in_hub();
    if (a && hub_area(LD8(a)) && (st == UI_RANK_MENU || st == UI_STAR_MENU) &&
        (on ? (pressed & ((1ull << BTN_L) | (1ull << BTN_R))) != 0 : g_village)) {  // (off: back to the Hub's quests)
        g_village = !g_village;
        reload_list(c);
        ST8(ui + UI_STATE, 0);  // (lays the menu out again)
    }
    as_village_menu(c, FN_UI_UPDATE);
}

// The hooks are set whatever the setting: each follows [Village Quests] VillageQuestsInHub as it is now (a Save in the
// settings menu turns it on or off; when off, the counter goes back to the Hub's quests at its rank or star menu).
// A Village quest started from the Hub or the Pub: a Map for each hunter, as in a Hub quest (see the top).
static void supply_deliver(Cpu *c) {
    uint32_t box = c->r[0];
    fnhook_call_original(c, FN_SUPPLY_DELIVER);
    uint32_t qd = box ? LD32(box + BOX_QUEST) : 0, q = qd ? LD32(qd + 8) : 0;
    bool maps = q && village_active() && hpscale_village_from_hub(q);
    uint32_t added = 0;
    for (uint32_t i = BOX_COUNT; maps && i-- > 0 && added < HUB_MAPS;) {
        uint32_t slot = box + BOX_SLOTS + 4 * i, in = box + BOX_IN + 4 * (i / 32);
        if (LD16(slot)) continue;
        ST16(slot, ITEM_MAP);
        ST16(slot + 2, 1);
        ST32(in, LD32(in) | 1u << (i % 32));
        added++;
    }
    if (added) LOG("[vqhub] Village quest %07u from the Hub: %u Map(s) added to the supply box\n", q, added);
    if (g_trace && box) {
        char s[BOX_COUNT * 16] = "", *p = s;
        for (uint32_t i = 0; i < BOX_COUNT; i++) {
            uint32_t slot = box + BOX_SLOTS + 4 * i, in = LD32(box + BOX_IN + 4 * (i / 32)) >> (i % 32) & 1;
            if (LD16(slot))
                p += snprintf(p, (size_t)(s + sizeof s - p), " %u:%x x%u%s", i, LD16(slot), LD16(slot + 2), in ? "" : "(later)");
        }
        LOG("[vqhub] quest %07u supply box:%s\n", q, s);
    }
}

// Quest demos in a session. A quest's monster demo (event table 0x17a06fc, type 1 entries: event ev\NNN, area, quest)
// plays when a hunter first enters its area (lookup 0x34cad8, from the area's update). When it ends, the game releases
// the spawn slots that wait for it (0x5a3ecc(sQuest, 1): condition type 4 with parameter 0 -> status 1, spawned by the
// spawn manager on its next update). In a session the lookup answers "no demo" at once (sNetwork +0x63e), so nothing
// is released: Hub quests are made without such slots, but Village quests have them. "A Forest Fracas" (329): the
// Mizutsune demo (event049, area 5) releases the Bulldrome, the quest's target, so the quest couldn't be finished.
// For a Village quest in a session, every game in it releases those slots once any hunter is in a demo's area (the
// players' zones are synced), so the monster appears for all of them at about the same time, as after the demo.
#define FN_SPAWN_UPDATE   0x5a08e8u   // sQuest: the spawn manager's update (every game, every frame in a quest)
#define FN_SPAWN_RELEASE  0x5a3eccu   // (sQuest, 1): type 4 slots with parameter 0 -> ready (what a demo's end calls)
#define G_SNETWORK        0x1a843f0u  // sNetwork *: +0x63e in a session
#define NET_IN_SESSION    0x63eu
#define G_SPLAYER         0x1a85b58u  // sPlayer *: +0x1c the 4 player slots
#define PL_ZONE           0x1054u     // player: its area (u8, 0 the camp)
#define SQ_STATE          0x50u       // sQuest: u8 2 = in the hunt
#define SQ_QUEST          0xe0u       // sQuest: the quest's number
#define SQ_SLOT_COND      0x1514u     // sQuest: 5 spawn slots of 12 bytes: u8 condition type, u8 status (0 waiting), ...
#define SQ_SLOT_PARAM     0x151cu     //   ... u32 the condition's parameter
#define EVENT_TABLE       0x17a06fcu  // 0x6b x 12 bytes: u8 type, u8 event, u8 stage, u8 area, u32, u32 quest
#define EVENT_COUNT       0x6bu

static bool g_demo_released;
static bool g_demo_test;  // (debug) MHGU_VQ_DEMO_TEST=1: also alone and for any quest (to test the release solo)

static bool slots_wait_for_demo(uint32_t sq) {
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t s = sq + SQ_SLOT_COND + 12 * i;
        if (LD8(s) == 4 && LD8(s + 1) == 0 && LD32(sq + SQ_SLOT_PARAM + 12 * i) == 0) return true;
    }
    return false;
}

static void spawn_update(Cpu *c) {
    uint32_t sq = c->r[0], net = LD32(G_SNETWORK);
    if (sq && LD8(sq + SQ_STATE) != 2) {
        g_demo_released = false;  // (between quests)
    } else if (sq && !g_demo_released &&
               (g_demo_test || (net && LD8(net + NET_IN_SESSION) && hpscale_village_from_hub(LD32(sq + SQ_QUEST)))) &&
               slots_wait_for_demo(sq)) {
        uint32_t q = LD32(sq + SQ_QUEST), spl = LD32(G_SPLAYER);
        for (uint32_t e = EVENT_TABLE; e < EVENT_TABLE + 12 * EVENT_COUNT && !g_demo_released; e += 12) {
            if (LD8(e) != 1 || LD32(e + 8) != q) continue;
            for (uint32_t k = 0; k < 4 && spl; k++) {
                uint32_t p = LD32(spl + 0x1c + 4 * k);
                if (!p || LD8(p + PL_ZONE) != LD8(e + 3)) continue;
                call4(c, FN_SPAWN_RELEASE, sq, 1, 0, 0);
                g_demo_released = true;
                LOG("[vqhub] Village quest %07u in a session: no demo online (event%03u, area %u); hunter %u is there, "
                    "releasing the monsters it would have\n", q, LD8(e + 1), LD8(e + 3), k);
                break;
            }
        }
    }
    fnhook_call_original(c, FN_SPAWN_UPDATE);
}

void villagehub_init(void) {
    g_trace = getenv("MHGU_VQ_TRACE") != NULL;
    g_demo_test = getenv("MHGU_VQ_DEMO_TEST") != NULL;
    g_path = hle_alloc(64);
    fnhook_set(FN_SPAWN_UPDATE, hle_register("villagehub:spawn_update", spawn_update));
    fnhook_set(FN_SUPPLY_DELIVER, hle_register("villagehub:supply_deliver", supply_deliver));
    fnhook_set(FN_QUEST_FILTER, hle_register("villagehub:quest_filter", quest_filter));
    fnhook_set(FN_QUEST_LOOKUP, hle_register("villagehub:quest_lookup", quest_lookup));
    fnhook_set(FN_CATEGORY_BUILD, hle_register("villagehub:category_build", category_build));
    fnhook_set(FN_CATEGORY_FILL, hle_register("villagehub:category_fill", category_fill));
    fnhook_set(FN_CATEGORY_GET, hle_register("villagehub:category_get", category_get));
    fnhook_set(FN_UI_ROWS, hle_register("villagehub:ui_rows", ui_rows));
    fnhook_set(FN_UI_UPDATE, hle_register("villagehub:ui_update", ui_update));
    fnhook_set(FN_UI_DRAW, hle_register("villagehub:ui_draw", ui_draw));
}
