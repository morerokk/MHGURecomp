// Desktop NSP importer: extracts MHGU base + 1.4.0 update NSPs into data/exefs + data/romfs via hactool.
// Isolated from boot()/loader/romfs: only the missing-files path in main.c calls this.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NSP_IMPORT_OK = 0,
    NSP_IMPORT_CANCELLED = 1,
    NSP_IMPORT_ERR = 2,
};

// Progress callback from the worker (may run on another thread). stage is a short status line; frac in [0,1].
typedef void (*NspImportProgressFn)(void *user, const char *stage, float frac);

typedef struct NspImportRequest {
    const char *data_dir;       // destination data/ (holds exefs + romfs)
    const char *base_nsp;       // base game NSP
    const char *update_nsp;     // 1.4.0 update NSP
    const char *prod_keys;      // required prod.keys
    const char *title_keys;     // optional title.keys (NULL if none)
    const char *hactool;        // path to hactool(.exe); NULL = look beside the executable
    volatile int *cancel;       // set nonzero to cancel; may be NULL
    NspImportProgressFn progress;
    void *progress_user;
    char error[512];            // filled on NSP_IMPORT_ERR
} NspImportRequest;

// True when this build supports NSP import (desktop Windows/Linux).
bool nsp_import_available(void);

// Resolve hactool next to the running executable into out. false if missing.
bool nsp_import_find_hactool(char *out, size_t size);

// Run extraction + install into data_dir. Blocking. Does not touch the original NSPs.
// On success, data_dir/exefs and data_dir/romfs pass the same checks as game_files_problem().
int nsp_import_run(NspImportRequest *req);

// Shared with main.c: what is wrong with the game files in data (into msg), or false when they are fine.
bool game_files_problem(const char *data, char *msg, size_t size);

// Offer dialog + file pickers + progress UI. true if import finished and data_dir should be rechecked.
// confirm_overwrite: ask before replacing an already-valid data_dir.
bool nsp_import_offer(const char *msg, char *data_dir, size_t data_size, bool confirm_overwrite);

#ifdef __cplusplus
}
#endif
