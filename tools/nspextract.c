// MHGU NSP extractor: a small program on its own (no game, no GPU, no recompiled code needed) that makes the game files the
// port wants from the base game's NSP and the 1.4.0 update's NSP. Put it in the MHGURecomp folder (next to mhgurecomp.exe)
// and drag both NSP files onto it, or put both beside it and open it: it writes data\exefs and data\romfs there (about
// 13 GB), then start mhgurecomp.exe. Needs the player's own prod.keys (next to it, in keys\, or where Ryujinx / yuzu keep it).
//
//   mhgu-nsp-extract [--keys prod.keys] [--out folder] [base.nsp update.nsp]
//
// The work is in runtime/nspimport.c (the port itself uses it too).
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define CHDIR _chdir
#else
#include <dirent.h>
#include <unistd.h>
#define CHDIR chdir
#endif
#include "../runtime/nspimport.h"

static bool is_nsp(const char *p) {
    size_t n = strlen(p);
    return n > 4 && tolower((unsigned char)p[n - 4]) == '.' && tolower((unsigned char)p[n - 3]) == 'n' &&
           tolower((unsigned char)p[n - 2]) == 's' && tolower((unsigned char)p[n - 1]) == 'p';
}

// the program's own folder, into dir; false when it can't be told
static bool exe_dir(char *dir, size_t size) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, (DWORD)size);
    if (!n || n >= size) return false;
#else
    ssize_t n = readlink("/proc/self/exe", dir, size - 1);
    if (n <= 0) return false;
    dir[n] = 0;
#endif
    char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    if (b > s) s = b;
    if (!s) return false;
    *s = 0;
    return true;
}

// the exefs/main of the update must be v1.4.0's: the port's recompiled code is, and no other version runs
static bool verify_exefs(const char *exefs_dir, char *err, size_t size) {
    char path[1300];
    snprintf(path, sizeof path, "%s/main", exefs_dir);
    if (getenv("MHGU_NSP_ANY_VERSION")) return true;  // (tests with made-up NSP files)
    static const unsigned char want[8] = {0xfb, 0x08, 0xf1, 0xd2, 0x0f, 0xd1, 0x20, 0x4f};
    unsigned char hdr[0x48] = {0};
    FILE *f = fopen(path, "rb");
    bool ok = f && fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4) && !memcmp(hdr + 0x40, want, 8);
    if (f) fclose(f);
    if (!ok)
        snprintf(err, size, "That update is not version 1.4.0 of Monster Hunter Generations Ultimate (the port runs only "
                 "that version). Use the base game's NSP together with the 1.4.0 update's NSP.");
    return ok;
}

static void progress(void *user, const char *what, unsigned long long done, unsigned long long total);
static void progress_cb(void *user, const char *what, uint64_t done, uint64_t total) { progress(user, what, done, total); }
static void progress(void *user, const char *what, unsigned long long done, unsigned long long total) {
    (void)user;
    if (!total) { printf("%s...\n", what); fflush(stdout); return; }
    printf("\r  %s: %llu / %llu MB   ", what, done >> 20, total >> 20);
    if (done >= total) printf("\n");
    fflush(stdout);
}

// a window that closes the moment the program ends (dragging a file onto it) must not hide what it said
static void wait_for_enter(void) {
#ifdef _WIN32
    printf("\nPress Enter to close this window.");
    fflush(stdout);
    getchar();
#endif
}

static int done_with(int code) {
    wait_for_enter();
    return code;
}

int main(int argc, char **argv) {
    const char *keys = NULL, *out = NULL;
    char *nsps[16];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (is_nsp(argv[i]) && n < 16) nsps[n++] = argv[i];
        else {
            printf("Usage: %s [--keys prod.keys] [--out folder] base.nsp update.nsp\n"
                   "Or drag the base game's NSP and the 1.4.0 update's NSP onto this program.\n", argv[0]);
            return done_with(2);
        }
    }
    printf("MHGU Recomp: extract the game from its NSP files\n\n");

    // the program's folder: the keys are looked for there, and data\ goes there
    char home[1100] = ".", dflt[1200], found[4][1100];
    char *list[4];
    if (exe_dir(home, sizeof home) && CHDIR(home) != 0) snprintf(home, sizeof home, ".");
    snprintf(dflt, sizeof dflt, "%s/data", home);
    if (!out) out = dflt;

    if (!n) {  // nothing dragged on it: the NSP files beside it
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA("*.nsp", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (n < 4) { snprintf(found[n], sizeof found[n], "%s", fd.cFileName); nsps[n] = found[n]; n++; }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR *d = opendir(".");
        for (struct dirent *e; d && (e = readdir(d));)
            if (is_nsp(e->d_name) && n < 4) { snprintf(found[n], sizeof found[n], "%s", e->d_name); nsps[n] = found[n]; n++; }
        if (d) closedir(d);
#endif
    }
    if (n != 2) {
        printf(n ? "Found %d NSP files: two are needed, the base game's and the 1.4.0 update's.\n\n"
                 : "No NSP files found.\n\n", n);
        printf("Drag the base game's NSP and the 1.4.0 update's NSP onto this program, or put both in this folder\n(%s)\n"
               "and open it again.\n", home);
        return done_with(1);
    }
    for (int i = 0; i < 4; i++) list[i] = i < n ? nsps[i] : NULL;

    printf("Base/update: %s\n             %s\nInto:        %s\n(about 13 GB, a few minutes)\n\n", nsps[0], nsps[1], out);
    NspImport job = {.nsp_paths = (const char *const *)list, .nsp_count = n, .keys_path = keys, .out_dir = out,
                     .progress = progress_cb, .verify_exefs = verify_exefs};
    char err[1100];
    if (!nsp_import(&job, err, sizeof err)) {
        printf("\nIt did not work:\n\n%s\n", err);
        return done_with(1);
    }
    printf("\nDone. The game files are in %s\nNow start mhgurecomp.exe.\n", out);
    return done_with(0);
}
