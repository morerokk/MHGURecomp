// (debug) MHGU_QUICKHUNT=<quest number>: from start-up straight into that quest, for testing in a hunt (e.g. 101,
// Find the Ferns: Village 1 star, Jurassic Frontier, no large monster). Presses A through the title, the main menu
// (Continue), the character select (the first character) and the welcome message, then orders the quest and departs
// at the first update of the town the save is in. The save is not changed (no quest fee is paid).
// MHGU_QUICKHUNT=town: the same up to the town, and stays there.
// Only quests of the town the save starts in: a Hub quest (10101...) departed from the Village stops on a call
// through a missing vtable slot (main 0x2125b8, the stage's object; checked 2026-10-30 with no mods).
//
// How the game departs: the counter's accept (0x984e94) writes the quest number to sQuestSelector +0xd68 (0 = none)
// and sets bit 0 of its flags (+0xd65); "Depart on Quest" at the departure point sets bit 3 there. The selector's
// update (vtable +0x1c, 0x5b7b94) then turns bit 3 into a request (+0x1e0 = 4) that lasts one update. In a town
// (aVillage*, aLobbyBase) the area's update switches on a mode (+0xb8); mode 1 (0x8bd0fc, the town as you walk
// around) runs the selector's update and, seeing the request and an ordered quest, calls the area's depart method
// (0x8c02a4): stage from the ordered quest (sGameControl +0x48 / +0x4a), the area's quest flag (+0xda), mode 4,
// whose states fade out and jump to the stage's area (aMap<nn>), where aQuest loads the quest from +0xd68.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define FN_TOWN_IDLE     0x8bd0fcu   // town area update, mode 1 (r0: the area)
#define G_SELECTOR       0x1a8ae38u  // sQuestSelector *
#define SEL_FLAGS        0xd65u      // u8: bit 0 ordered at a counter, bit 3 depart
#define SEL_ORDERED      0xd68u      // the ordered quest's number
#define FLAG_ORDERED     0x01u
#define FLAG_DEPART      0x08u
#define TOWN_ONLY        0xffffffffu  // (MHGU_QUICKHUNT=town)

static uint32_t g_quest;          // the quest to start (0: off)
static volatile bool g_in_town;   // set by the game thread at the first town update
static bool g_done;

static void town_idle(Cpu *c) {
    if (g_quest && !g_done) {
        g_done = true;
        g_in_town = true;
        uint32_t sel = LD32(G_SELECTOR);
        if (g_quest == TOWN_ONLY) {
            LOG("[quickhunt] in town\n");
        } else if (sel) {
            ST32(sel + SEL_ORDERED, g_quest);
            ST8(sel + SEL_FLAGS, LD8(sel + SEL_FLAGS) | FLAG_ORDERED | FLAG_DEPART);
            LOG("[quickhunt] in town: ordered quest %u, departing\n", g_quest);
        } else {
            LOG("[quickhunt] in town, but there is no sQuestSelector\n");
        }
    }
    fnhook_call_original(c, FN_TOWN_IDLE);
}

// UI thread: A every 1.5 s (90 presented frames at 60 fps; more frames at higher frame rates, still fine) until the
// game reaches a town.
void quickhunt_poll(void) {
    static uint64_t next = 120;
    if (!g_quest || g_in_town || g_present_count < next) return;
    next = g_present_count + 90;
    host_input_press("A", 8);
}

// (debug) MHGU_FORCE_EVENT=<index>: the next call of the quest-event lookup (0x34cad8) answers with that entry of the
// event manager's table (0x17a06fc: 0x6b entries of 12 bytes: u8 type (1 quest demo, 2 stage, 5 movie), u8 event or
// stage-event number, u8 stage, u8 town/area, u32, u32 quest). The lookup runs where a quest's demo can start (e.g.
// quest 713's post-clear Valstrax demo, entry 105 = event082), and only then: not on arrival in a town.
#define FN_QUEST_EVENT 0x34cad8u
static int g_force_event = -1;
static void quest_event(Cpu *c) {
    uint32_t mgr = c->r[0];
    if (g_force_event >= 0) {
        uint32_t e = 0x17a06fcu + 12u * (uint32_t)g_force_event;
        ST32(mgr + 0x28, (uint32_t)g_force_event);
        ST8(mgr + 0x24, LD8(e));
        LOG("[quickhunt] forced event table entry %d (type %u, event %03u)\n", g_force_event, LD8(e), LD8(e + 1));
        g_force_event = -1;
        RET(1);
        return;
    }
    fnhook_call_original(c, FN_QUEST_EVENT);
}

// (debug) MHGU_KILL_AFTER=<s>: that long after the first quest update (sQuest 0x59f8b8), the first large monster is put
// at 1 HP and takes one hit through the game's own damage function (0x29d6f8), so it dies as in a hunt (carcass, clear,
// what the quest does after). Unlike an insta-clear (sQuest +0xec = 0x29), which leaves the monster alive.
#define FN_QUEST_UPDATE 0x59f8b8u
#define FN_EN_DAMAGE_Q  0x29d6f8u
static uint64_t g_kill_ns, g_kill_at;
static void quest_update(Cpu *c) {
    if (g_kill_ns) {
        uint64_t now = plat_now_ns();
        if (!g_kill_at) g_kill_at = now + g_kill_ns;
        if (now >= g_kill_at) {
            g_kill_ns = 0;
            uint32_t sen = LD32(0x1a51fe0u), en = 0;
            for (int k = 0; sen && k < 10 && !en; k++) {
                uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k), info = e ? LD32(e + 0x1428) : 0;
                if (info && LD16(info + 0x1b6) < 10000) en = e;
            }
            if (en) {
                uint32_t info = LD32(en + 0x1428);
                ST32(info + 0x370, 1);
                uint32_t saved[16];
                for (int i = 0; i < 16; i++) saved[i] = c->r[i];
                c->r[0] = en, c->r[1] = 0, c->r[2] = 100, c->r[3] = 0;
                guest_call(c, FN_EN_DAMAGE_Q);
                for (int i = 0; i < 16; i++) c->r[i] = saved[i];
                LOG("[quickhunt] killed monster %08x: HP now %d\n", en, (int)LD32(info + 0x370));
            } else {
                LOG("[quickhunt] kill: no large monster\n");
            }
        }
    }
    fnhook_call_original(c, FN_QUEST_UPDATE);
}

void quickhunt_init(void) {
    const char *ka = getenv("MHGU_KILL_AFTER");
    if (ka && *ka) {
        g_kill_ns = (uint64_t)atoi(ka) * 1000000000ull;
        fnhook_set(FN_QUEST_UPDATE, hle_register("quickhunt:quest_update", quest_update));
    }
    const char *fe = getenv("MHGU_FORCE_EVENT");
    if (fe && *fe) {
        g_force_event = atoi(fe);
        fnhook_set(FN_QUEST_EVENT, hle_register("quickhunt:quest_event", quest_event));
    }
    const char *e = getenv("MHGU_QUICKHUNT");
    if (!e || !*e) return;
    g_quest = !strcmp(e, "town") ? TOWN_ONLY : (uint32_t)strtoul(e, NULL, 10);
    if (!g_quest) return;
    fnhook_set(FN_TOWN_IDLE, hle_register("quickhunt:town_idle", town_idle));
    if (g_quest == TOWN_ONLY) LOG("[quickhunt] pressing A to the town\n");
    else LOG("[quickhunt] quest %u: pressing A to the town, then departing\n", g_quest);
}
