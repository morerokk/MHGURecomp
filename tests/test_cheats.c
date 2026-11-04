// Host-only test of the cheats folder being read again (cheats_rescan): runtime/cheats.c on a fake code page.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "cheats.h"
#include "hle/input.h"
#include "interp.h"
#include "mods.h"
#include "rt.h"

#define DIR_NAME "cheats-test"
#define SHARED   0x100u  // (from main's start: the word both cheats change)
#define OTHER    0x104u
#define ORIGINAL 0x11111111u

Module g_modules[MAX_MODULES];
int g_nmodules = 1;
volatile uint64_t g_present_count;
bool kernel_mapped(uint64_t a, uint32_t size) { return a >= MAIN_BASE && a + size <= MAIN_BASE + 0x1000; }
uint32_t dispatch_function_at(uint32_t a, uint32_t *next) { (void)a; *next = 0; return 0; }
int dispatch_interpret(uint32_t entry, uint32_t end) { (void)entry; (void)end; return 0; }
void interp_add_range(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void input_get(uint64_t *buttons, int32_t sticks[4]) { *buttons = 0; memset(sticks, 0, 16); }

static uint32_t *word(uint32_t off) { return (uint32_t *)G2H(MAIN_BASE + off); }

static void put(const char *file, const char *text) {
    char path[256];
    snprintf(path, sizeof path, DIR_NAME "/%s", file);
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void gone(const char *file) {
    char path[256];
    snprintf(path, sizeof path, DIR_NAME "/%s", file);
    remove(path);
}

static void frame(void) { g_present_count++, cheats_frame(); }

int main(void) {
#ifdef _WIN32
    void *page = VirtualAlloc(G2H(MAIN_BASE), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    _putenv("MHGU_CHEATS_DIR=" DIR_NAME);
#else
    void *page = mmap(G2H(MAIN_BASE), 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    setenv("MHGU_CHEATS_DIR", DIR_NAME, 1);
#endif
    assert(page == G2H(MAIN_BASE));
    cheats_make_dir();
    g_modules[0].base = MAIN_BASE, g_modules[0].text_size = 0x1000, g_modules[0].ro_off = 0x1000;
    *word(SHARED) = ORIGINAL, *word(OTHER) = ORIGINAL;

    // Two cheats change the same word.
    put("a.txt", "[A]\n04000000 00000100 AAAAAAAA\n");
    put("b.txt", "[B]\n04000000 00000100 BBBBBBBB\n");
    gone("_off.txt");
    cheats_init();
    assert(*word(SHARED) == 0xBBBBBBBBu);

    // The first one's file edited, to another word: the second one keeps the shared word...
    put("a.txt", "[A]\n04000000 00000104 AAAAAAAA\n");
    cheats_rescan();
    assert(*word(SHARED) == 0xBBBBBBBBu);
    frame();
    assert(*word(OTHER) == 0xAAAAAAAAu && *word(SHARED) == 0xBBBBBBBBu);

    // ...and can still be undone, down to the game's own value.
    gone("b.txt");
    cheats_rescan();
    assert(*word(SHARED) == ORIGINAL);

    gone("a.txt");
    cheats_rescan();
    assert(*word(OTHER) == ORIGINAL);
    rmdir(DIR_NAME);
    puts("test-cheats: ok");
    return 0;
}
