// Test driver for runtime/nspimport.c: test_nspimport KEYS OUTDIR BASE.nsp UPDATE.nsp  (see tests/run_nsp_test.sh)
#include <stdio.h>
#include <string.h>
#include "../runtime/nspimport.h"

static void progress(void *user, const char *what, uint64_t done, uint64_t total) {
    (void)user;
    if (total) fprintf(stderr, "\r%s %llu / %llu", what, (unsigned long long)done, (unsigned long long)total);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s KEYS OUTDIR NSP NSP...\n", argv[0]); return 2; }
    NspImport job = {.nsp_paths = (const char *const *)(argv + 3), .nsp_count = argc - 3,
                     .keys_path = strcmp(argv[1], "-") ? argv[1] : NULL, .out_dir = argv[2], .progress = progress};
    char err[900] = "";
    bool ok = nsp_import(&job, err, sizeof err);
    fprintf(stderr, "\n");
    if (!ok) { printf("FAILED: %s\n", err); return 1; }
    printf("OK\n");
    return 0;
}
