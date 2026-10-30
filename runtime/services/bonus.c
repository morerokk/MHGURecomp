// Bonus content (free downloads: event / challenge quests, item packs, Palicos) carried into new saves.
//
// The game's Download menu fetched these from Capcom's server and stored them in the shared part of the save file
// (`system`), so every character slot sees them. The quests' own files are in the RomFS; the server only said which
// ones were out, and the item packs and Palicos came from it. That server can't be reached from the port, so the
// downloads are taken from a save that has them instead: a "donor" `system` file in the bonus folder beside
// mhgurecomp.exe (bonus\system; with a game folder argument: in the current folder), else in the player's folder
// (%APPDATA%\MHGURecomp\bonus\system), else (a macOS app) in the app's Contents/Resources/bonus; MHGU_BONUS_DIR
// overrides them all.
//
// Save file layout (MHGU v1.4.0, 0x4eb8bc bytes): a 0x24-byte header (+0x1c: data size, +0x34/+0x38/+0x3c: the three
// character slots, relative to 0x24), then the shared data. Two regions hold the downloads, and in a save that never
// downloaded anything both are all zero:
//   [0xb2a5, 0xb310)  flags: which downloaded quests are unlocked (without them the Event lists say "No Quests
//                     available"; meaning of the bits not decoded)
//   [0xc000, first character slot)  the downloads: item packs (0x68-byte records from 0xc011), Palicos (from 0xd625),
//                     160 event quest slots from 0xf899 and 40 challenge quest slots from 0x127899 ([u32 quest id]
//                     [u32 size][ARC], 0x1c00 apart), then the challenge quests' records
// When both are all zero in a save, the donor's are copied in; nothing else in the file is touched. A save that has
// downloads of its own is left alone (claims, deleted quests and so on stay the game's business).
// Only with BonusContent = On in mhgurecomp.ini (the default; Off: saves are never touched).
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../config.h"
#include "../rt.h"

#define HDR_SIZE    0x24u
#define FLAGS_START 0xb2a5u
#define FLAGS_END   0xb310u
#define AREA_START  0xc000u
#define SAVE_SIZE   0x4eb8bcu
#define EVENT_SLOTS 0xf899u   // 160 slots
#define CHAL_SLOTS  0x127899u // 40 slots
#define SLOT_SIZE   0x1c00u

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static const char *bonus_path(void) {
    static char path[1200];
    const char *e = getenv("MHGU_BONUS_DIR");
    if (e && *e) {
        snprintf(path, sizeof path, "%s/system", e);
        return path;
    }
    snprintf(path, sizeof path, "bonus/system");
    if (!access(path, F_OK)) return path;
    snprintf(path, sizeof path, "%s/../bonus/system", g_save_dir);
#ifdef __APPLE__
    // a macOS release app carries it in Contents/Resources/bonus (tools/make_app.py --bonus); the player's own wins
    char exe[1024];
    if (access(path, F_OK) && plat_in_app_bundle() && plat_exe_path(exe, sizeof exe) && strrchr(exe, '/')) {
        *strrchr(exe, '/') = 0;
        snprintf(path, sizeof path, "%s/../Resources/bonus/system", exe);
    }
#endif
    return path;
}

// The area [AREA_START, first character slot) of a save image, or 0 if it isn't one.
static uint32_t area_end(const uint8_t *s, size_t n) {
    if (n != SAVE_SIZE || rd32(s + 0x1c) + HDR_SIZE != SAVE_SIZE) return 0;
    uint32_t slot1 = rd32(s + 0x34) + HDR_SIZE;
    return slot1 > CHAL_SLOTS + 40 * SLOT_SIZE && slot1 < n ? slot1 : 0;
}

static int count_slots(const uint8_t *s, uint32_t at, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) k += rd32(s + at + (uint32_t)i * SLOT_SIZE) != 0;
    return k;
}

static uint8_t *g_donor;  // the donor save image (NULL: none)
static uint32_t g_end;

static bool load_donor(void) {
    static bool tried;
    if (tried) return g_donor != NULL;
    tried = true;
    const char *path = bonus_path();
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *s = malloc(SAVE_SIZE + 1);
    size_t n = fread(s, 1, SAVE_SIZE + 1, f);
    fclose(f);
    uint32_t end = area_end(s, n);
    bool any = false;
    for (uint32_t i = AREA_START; end && i < end && !any; i++) any = s[i] != 0;
    if (!end || !any) {
        LOG("[bonus] %s: %s, not used\n", path, !end ? "not an MHGU v1.4 save file" : "has no downloaded content");
        free(s);
        return false;
    }
    g_donor = s, g_end = end;
    LOG("[bonus] bonus content from %s: %d event quests, %d challenge quests\n", path,
        count_slots(s, EVENT_SLOTS, 160), count_slots(s, CHAL_SLOTS, 40));
    return true;
}

// Called before the game opens a save file (host path): fills in the bonus content if the file has none.
void bonus_fill(const char *host_path) {
    const char *base = strrchr(host_path, '/');
    base = base ? base + 1 : host_path;
    if (strcmp(base, "system") && strcmp(base, "system_backup")) return;
    if (!g_config.bonus_content) {
        static bool said;
        if (!said) LOG("[bonus] bonus content off (mhgurecomp.ini BonusContent): saves left as they are\n");
        said = true;
        return;
    }
    if (!load_donor()) return;
    int fd = open(host_path, O_RDWR | O_BINARY);
    if (fd < 0) return;
    uint8_t *s = malloc(SAVE_SIZE + 1);
    ssize_t n = pread(fd, s, SAVE_SIZE + 1, 0);
    uint32_t end = n > 0 ? area_end(s, (size_t)n) : 0;
    bool empty = end == g_end && memcmp(s + 0x34, g_donor + 0x34, 12) == 0;
    for (uint32_t i = FLAGS_START; empty && i < FLAGS_END; i++) empty = s[i] == 0;
    for (uint32_t i = AREA_START; empty && i < end; i++) empty = s[i] == 0;
    if (empty) {  // (a file the game has just created is all zero: no header yet, so area_end() said no)
        if (pwrite(fd, g_donor + FLAGS_START, FLAGS_END - FLAGS_START, FLAGS_START) == (ssize_t)(FLAGS_END - FLAGS_START) &&
            pwrite(fd, g_donor + AREA_START, end - AREA_START, AREA_START) == (ssize_t)(end - AREA_START))
            LOG("[bonus] %s: added the bonus content\n", host_path);
        else
            LOG("[bonus] %s: could not write the bonus content\n", host_path);
    }
    free(s);
    close(fd);
}
