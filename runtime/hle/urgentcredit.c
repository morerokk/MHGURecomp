// Urgent quests count for everyone who cleared them and has them unlocked, not only for the hunter who posted them
// (ini [Multiplayer] ShareUrgentCredit). Clearing a quest sets its bit in the save's clear flags in 0x588d40, the same
// for every quest, unless this game isn't the poster's (0x5ae3b4: online, and sQuestSelector +0xd788 != +0xd789) and
// one of:
//   0x5ae75c  an Urgent quest: sQuest +0xb8 bit 12, from bit 0 of cQuestData mFlag (+0x10c)
//   0x5ae768  a Special Permit quest (group 4)
//   0x5a3424  a quest of some requesters (mRequestVillage 5-8, 12) that this save hasn't unlocked
// The first is all that keeps a joiner's Urgent from counting: 0x5ae75c is called from there alone, and nothing else
// reads the bit. mFlag bit 0 is on 28 quests (Vaulting Outlaw 206, A Shocking Scoundrel 10307, Dirty Deals 11204...).
// A hunter who hasn't unlocked the Urgent gets nothing from joining it, as in the original: 0x74af8c is what the quest
// counters ask to show a quest. For an Urgent its script goes by a progress flag of the save, not by the clear flags
// (+0x500 of the object at 0x1a390e0: 206 and 303 in byte 1, 10216 in byte 0xc, 10307 in 0xd, 11204 in 0xaf). The script
// module isn't loaded during a hunt (the answer is then always no), but is on the result screen, where 0x588d40 runs.
// The save is each player's own, so this needs nothing from the host and changes nothing for the others in the quest.
#include <string.h>

#include "../config.h"
#include "../hle.h"

#define FN_URGENT   0x5ae75cu  // (sQuest) -> an Urgent quest
#define FN_UNLOCKED 0x74af8cu  // (quest number, 0xff, 0) -> unlocked in this save
#define SQ_QUEST_NO 0xe0u      // sQuest: the quest number

static bool unlocked(Cpu *c, uint32_t q) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = q, c->r[1] = 0xff, c->r[2] = 0;
    guest_call(c, FN_UNLOCKED);
    bool r = c->r[0] & 1;
    memcpy(c->r, saved, sizeof saved);
    return r;
}

static void urgent(Cpu *c) {
    uint32_t q = LD32(c->r[0] + SQ_QUEST_NO);
    fnhook_call_original(c, FN_URGENT);
    if (!(c->r[0] & 1) || !config_live_share_urgent()) return;
    if (!unlocked(c, q)) {
        LOG("[urgent] quest %07u cleared, posted by someone else: not unlocked in this save, it doesn't count\n", q);
        return;
    }
    LOG("[urgent] quest %07u cleared, posted by someone else: it counts here too\n", q);
    c->r[0] = 0;
}

void urgentcredit_init(void) {
    fnhook_set(FN_URGENT, hle_register("urgentcredit:urgent", urgent));
}
