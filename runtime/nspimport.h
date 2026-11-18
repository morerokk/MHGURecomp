// Game files from the player's own NSP files: the base game's NSP and the title update's NSP in, the exefs/ and romfs/
// folders the port reads out (the update's exefs, the base game's romfs with the update's changes merged in).
//
// What it does: reads the NSP (a PFS0 archive), decrypts the Program NCA in each with the player's own prod.keys (and
// the ticket's title key in the NSP, when the NCA uses one), takes the ExeFS from the update, and reads the update's
// RomFS (a BKTR patch) over the base game's RomFS. Nothing here comes with the keys; they are the player's, from their
// own Switch (Lockpick_RCM writes prod.keys). No signature or hash is checked: the port checks the result's build id
// and file count at the start (main.c), as it does for any dump.
//
// No platform code in here (the tests build this file alone): runtime/nsp_launch.c is the part that knows the game
// folder, the command line and the messages.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// what: "ExeFS" / "RomFS" while it works; done / total in bytes of the files written (total 0: not known yet)
typedef void (*NspProgress)(void *user, const char *what, uint64_t done, uint64_t total);

typedef struct {
    const char *const *nsp_paths;  // the NSP files: the base game and the update, in any order
    int nsp_count;
    const char *keys_path;         // prod.keys, or NULL for nsp_find_keys
    const char *out_dir;           // the game folder (data): exefs/ and romfs/ are made in it
    NspProgress progress;          // (may be NULL)
    void *user;
    // Called with the finished exefs folder, before the (long) RomFS is written: false (err filled) stops the import.
    // (main.c's check: the update's build id is v1.4.0's.)
    bool (*verify_exefs)(const char *exefs_dir, char *err, size_t err_size);
} NspImport;

// The first prod.keys found in the usual places (MHGU_KEYS, next to the program, the user's .switch folder, Ryujinx's and
// yuzu's folders), into path. False when there is none.
bool nsp_find_keys(char *path, size_t size);

// The places nsp_find_keys looks in, for the message when it finds none (one per line).
const char *nsp_keys_places(void);

// Does the whole import. err: what went wrong, in a sentence the player can act on.
// Files the game folder already had (exefs/, romfs/) are moved to previous-N/ in it, not deleted.
bool nsp_import(const NspImport *job, char *err, size_t err_size);
