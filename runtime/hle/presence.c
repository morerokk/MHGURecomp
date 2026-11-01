// What Discord Rich Presence shows (runtime/net/discord.c), read from the game once a second on its own thread:
//   in a town: "Bherna Village" / "Hub (online)"; the hunter, HR and weapon type; (2 of 4) in a hub
//   in a quest: "Hunting Khezu, Volvidon" (or the quest's name when it has no monster to hunt); "Jurassic Frontier,
//   Area 4"; (2 of 4) in multiplayer; the time since departing
// The party (2 of 4) carries an id, the same in every player's game: Discord groups the players who share it. A hub's
// is a hash of its session id (ldn_party); a quest's adds the quest number, so it holds only the quest's hunters.
// Images: the large one is the "logo" art asset (maps later), the small one the weapon type's icon (g_weapon_icons),
// its hover text the hunter. ini [Discord] ShowHunter = Off leaves the hunter's name and HR out. Names of monsters
// and quests are the game's own English texts; the maps' names are not in the game's text files (the game draws
// them), so they are here.
//   MHGU_LOG_DISCORD=1   log every activity sent to Discord
//
// Where it comes from (main v1.4.0, runtime addresses):
// - sGameControl *0x1a8ca48: +0x4c the town's type (0 Hub, 1-6 villages, 7 Hunters' Pub; 0xff in a quest), +0x30 our
//   player slot; sPlayer *0x1a85b58 +0x1c the 4 player slots. A player +0x1054: its zone (u8, 0 the camp);
//   +0x13c4 its data: +0x514 the name, +0x554 HR (u16), +0x14c the weapon's type (u32, as the game's w<nn> files:
//   0 Great Sword ... 14 Charge Blade), +0x150 its number.
// - sQuest *0x1a8ad90 +0xa4: the quest's cQuestData (the quest file questData_<n>.ext, laid out with alignment):
//   +0x8 number, +0x10 stage (u8), main objectives at +0x1b and +0x20 (u8 type, u16 target at +1 / +2: 1 hunt or
//   slay, 2 capture, 3 all large monsters, 4 slay a total, 5 deliver, 6-7 gather, 8+ others with a monster), +0x64
//   the large monsters (5 of 14 bytes, u16 monster id first).
// - Monster ids (small << 12 | variant << 8 | em number) -> name: the game's (id, index) table 0x1797ea4 (137 pairs),
//   then the index's UI number 0x182e7f8, the string number in GUI\06_msg\monsterName_eng.gmd.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../config.h"
#include "../hle.h"
#include "../net/discord.h"
#include "../net/net.h"
#include "../romfs.h"

#define G_GAMECONTROL  0x1a8ca48u
#define G_SPLAYER      0x1a85b58u
#define G_SQUEST       0x1a8ad90u
#define G_SELECTOR     0x1a8ae38u  // sQuestSelector *: +0xd750 the quest's hunters, +0xd74c its maximum
#define GC_TOWN        0x4cu
#define GC_SLOT        0x30u
#define PL_ZONE        0x1054u
#define PL_DATA        0x13c4u
#define PD_NAME        0x514u
#define PD_HR          0x554u
#define PD_WEAPON      0x14cu
#define MON_PAIRS      0x1797ea4u
#define MON_PAIR_COUNT 137
#define MON_UI         0x182e7f8u
#define TOWN_NONE      0xffu
#define TICK_NS        1000000000ull
#define MENU_NS        15000000000ull

static const char *const g_weapons[] = {
    "Great Sword", "Sword & Shield", "Hammer", "Lance", "Heavy Bowgun", NULL, "Light Bowgun", "Long Sword",
    "Switch Axe", "Gunlance", "Bow", "Dual Blades", "Hunting Horn", "Insect Glaive", "Charge Blade"};
// Their icons: art assets of the Discord application ("bowgun": the Heavy Bowgun)
static const char *const g_weapon_icons[] = {
    "great_sword", "sword_and_shield", "hammer", "lance", "bowgun", NULL, "light_bowgun", "long_sword",
    "switch_axe", "gunlance", "bow", "dual_blades", "hunting_horn", "insect_glaive", "charge_blade"};
// By quest stage (101-104: the first four at another time of day). Inferred from the quests' texts; 15 unknown.
static const char *const g_stages[] = {
    NULL, "Jurassic Frontier", "Verdant Hills", "Arctic Ridge", "Misty Peaks", "Dunes", "Deserted Island",
    "Marshlands", "Volcano", "Arena", "Slayground", "Ancestral Steppe", "Volcanic Hollow", "Primal Forest",
    "Frozen Seaway", NULL, "Sanctuary", "Forlorn Arena", "Sacred Pinnacle", "Ingle Isle", "Polar Field",
    "Wyvern's End", "Desert", "Jungle", "Ruined Pinnacle", "Castle Schrade", "Fortress", "Forlorn Citadel"};
static const char *const g_towns[] = {"Hub", "Bherna Village", "Kokoto Village", "Pokke Village", "Yukumo Village",
                                      NULL, "Wycademy", "Hunters' Pub"};  // (5: not seen yet)

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

// ---- the game's texts (GMD: the strings are the file's last part, its size at +0x20, NUL-terminated)
typedef struct { char *data; char **str; int n; } Gmd;

static bool gmd_load(Gmd *g, const char *rpath) {
    memset(g, 0, sizeof *g);
    const char *path = romfs_host_path(rpath);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = size > 0x30 && size < (8 << 20) ? malloc((size_t)size + 1) : NULL;
    bool ok = d && fread(d, 1, (size_t)size, f) == (size_t)size && !memcmp(d, "GMD", 4);
    fclose(f);
    uint32_t n, bytes;
    if (ok) memcpy(&n, d + 0x18, 4), memcpy(&bytes, d + 0x20, 4);
    if (!ok || bytes > (uint32_t)size || n > 0x10000) { free(d); return false; }
    d[size] = 0;
    g->data = d;
    g->str = calloc(n ? n : 1, sizeof *g->str);
    for (char *p = d + size - bytes; g->n < (int)n && p < d + size; p += strlen(p) + 1) g->str[g->n++] = p;
    return true;
}

static void gmd_free(Gmd *g) { free(g->data), free(g->str), memset(g, 0, sizeof *g); }

static const char *gmd_str(const Gmd *g, uint32_t i) { return i < (uint32_t)g->n ? g->str[i] : NULL; }

static const char *monster_name(uint16_t id) {
    static Gmd names;
    static bool tried;
    if (!tried) tried = true, gmd_load(&names, "nativeNX/eng/GUI/06_msg/monsterName_eng.gmd");
    for (uint32_t i = 0; i < MON_PAIR_COUNT; i++)
        if (LD32(MON_PAIRS + 8 * i) == id) {
            uint32_t index = LD32(MON_PAIRS + 8 * i + 4);
            return index < MON_PAIR_COUNT ? gmd_str(&names, LD32(MON_UI + 4 * index)) : NULL;
        }
    return NULL;
}

// The quest's name (string 0 of its text), kept for the quest being played.
static const char *quest_name(uint32_t quest) {
    static Gmd g;
    static uint32_t loaded;
    if (quest != loaded) {
        char path[96];
        snprintf(path, sizeof path, "nativeNX/eng/quest/questData/questData_%07u_eng.gmd", quest);
        gmd_free(&g);
        gmd_load(&g, path);
        loaded = quest;
    }
    return gmd_str(&g, 0);
}

static void add_name(char *list, size_t size, const char *name) {
    if (!name || !*name || strstr(list, name)) return;
    size_t n = strlen(list);
    snprintf(list + n, size - n, "%s%s", n ? ", " : "", name);
}

// "Khezu, Volvidon": the main objectives' monsters, else (all large monsters) the quest's large monsters.
static void quest_targets(uint32_t qd, char *out, size_t size) {
    *out = 0;
    static const uint32_t objectives[] = {0x1b, 0x20};
    bool all = false;
    for (int i = 0; i < 2; i++) {
        uint8_t type = LD8(qd + objectives[i]);
        uint16_t id = LD16(qd + ((objectives[i] + 2) & ~1u));
        if (type == 3) all = true;
        if (type == 1 || type == 2 || type >= 8)
            if (id && id < 0x1000) add_name(out, size, monster_name(id));
    }
    if (*out || !all) return;
    for (uint32_t i = 0; i < 5; i++) {
        uint16_t id = LD16(qd + 0x64 + 14 * i);
        if (id && id < 0x1000) add_name(out, size, monster_name(id));
    }
}

// ---- once a second, from the game's input polling
void presence_tick(void) {
    static uint64_t next;
    static int64_t since;
    static int last_place = -2;
    static uint32_t last_quest;
    uint64_t now = plat_now_ns();
    if (now < next) return;
    next = now + TICK_NS;

    uint32_t gc = LD32(G_GAMECONTROL), sp = LD32(G_SPLAYER);
    uint32_t slot = sane(gc) ? LD32(gc + GC_SLOT) : 0;
    uint32_t pl = sane(sp) && slot < 4 ? LD32(sp + 0x1c + 4 * slot) : 0, pd = sane(pl) ? LD32(pl + PL_DATA) : 0;
    uint32_t sq = LD32(G_SQUEST), qd = sane(sq) ? LD32(sq + 0xa4) : 0;
    int town = sane(gc) ? LD8(gc + GC_TOWN) : -1;
    uint32_t quest = town == TOWN_NONE && sane(qd) ? LD32(qd + 8) : 0;
    int place = !sane(pd) ? -1 : town == TOWN_NONE ? (quest ? 100 : -2) : town;  // -1 menus, 100 quest, -2 between
    // (no hunter while loading too: the menus only after a while; loading, the arena's waiting room...: what was
    // shown stays)
    static uint64_t no_hunter;
    if (place != -1) no_hunter = 0;
    else if (!no_hunter) no_hunter = now;
    if (place == -2 || (place == -1 && last_place != -1 && last_place != -2 && now - no_hunter < MENU_NS)) return;
    if (place != last_place && place >= 0 && place < 100) LOG("[discord] town %d\n", place);
    if (place != last_place || quest != last_quest) since = (int64_t)time(NULL);
    last_place = place, last_quest = quest;

    int party = 0, party_max = 0;
    char party_id[64] = "";
    bool online = ldn_party(&party, &party_max, party_id, sizeof party_id);

    DiscordActivity a;
    memset(&a, 0, sizeof a);
    snprintf(a.large_image, sizeof a.large_image, "logo");  // (an art asset too; the weapon icon needs a large image)
    if (place == -1) {
        snprintf(a.details, sizeof a.details, "In the menus");
        discord_set_activity(&a);
        return;
    }
    char hunter[96] = "";
    if (config_live_discord_hunter()) {
        char name[33];
        memcpy(name, G2H(pd + PD_NAME), 32), name[32] = 0;
        snprintf(hunter, sizeof hunter, "%s, HR %u", name, LD16(pd + PD_HR));
    }
    uint32_t w = LD32(pd + PD_WEAPON);
    const char *weapon = w < sizeof g_weapons / sizeof *g_weapons ? g_weapons[w] : NULL;
    const char *icon = weapon ? g_weapon_icons[w] : NULL;
    snprintf(a.small_image, sizeof a.small_image, "%s", icon ? icon : "");
    char who[160];
    snprintf(who, sizeof who, "%s%s%s", hunter, *hunter && weapon ? " - " : "", weapon ? weapon : "");

    if (place == 100) {
        char targets[96];
        quest_targets(qd, targets, sizeof targets);
        const char *title = quest_name(quest);
        if (*targets) snprintf(a.details, sizeof a.details, "Hunting %s", targets);
        else snprintf(a.details, sizeof a.details, "%s", title ? title : "On a quest");
        uint32_t stage = LD8(qd + 0x10) % 100;
        const char *map = stage < sizeof g_stages / sizeof *g_stages ? g_stages[stage] : NULL;
        int zone = sane(pl) ? LD8(pl + PL_ZONE) : -1;
        if (map && zone == 0) snprintf(a.state, sizeof a.state, "%s, Camp", map);
        else if (map && zone > 0) snprintf(a.state, sizeof a.state, "%s, Area %d", map, zone);
        else if (map) snprintf(a.state, sizeof a.state, "%s", map);
        snprintf(a.large_text, sizeof a.large_text, "%s", title ? title : "");
        snprintf(a.small_text, sizeof a.small_text, "%s", who);
        uint32_t sel = LD32(G_SELECTOR);
        if (online && sane(sel)) {  // (the quest's hunters, not the hub's)
            party = (int)LD32(sel + 0xd750), party_max = (int)LD32(sel + 0xd74c);
            if (party < 1 || party > 4 || party_max < party || party_max > 4) party = 0;
            size_t n = strlen(party_id);  // (the quest's own party: not the hunters who stayed in the hub)
            snprintf(party_id + n, sizeof party_id - n, "-q%u", quest);
        }
    } else {
        const char *name = town < (int)(sizeof g_towns / sizeof *g_towns) ? g_towns[town] : NULL;
        bool hub = town == 0 || town == 7;
        snprintf(a.details, sizeof a.details, "%s%s", name ? name : "In a village",
                 hub ? online ? " (online)" : " (offline)" : "");
        snprintf(a.state, sizeof a.state, "%s", who);
        snprintf(a.small_text, sizeof a.small_text, "%s", who);
    }
    if (online && party > 0) {
        a.party = party, a.party_max = party_max;
        snprintf(a.party_id, sizeof a.party_id, "%s", party_id);
    }
    a.start = since;
    discord_set_activity(&a);
}
