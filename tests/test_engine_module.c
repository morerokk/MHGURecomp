// Engine module loader test (make test-engine-modules): runtime/engine_module.c against the modules the Makefile builds in
// MHGU_ENGINE_MODULES_DIR - "good" (loads), "badapi" (claims API 99), "noapi" (no version), "declines" (entry returns false),
// "notamodule" (exports neither), "hidden" (fine but not listed in enabled.txt), plus names that must be refused and a folder
// that is missing. The Makefile checks the log. No game files needed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_module.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    engine_modules_init("test-version");
    engine_modules_init("test-version");  // (twice: the second call does nothing)
    const char *names[8];
    int n = engine_modules_loaded(names, 8);
    CHECK(n == 1);
    CHECK(n >= 1 && !strcmp(names[0], "Example"));
    printf("loaded %d\n", n);
    engine_modules_report();
    engine_modules_shutdown();  // (prints the module's "[example] shutdown")
    CHECK(engine_modules_loaded(names, 8) == 0);
    printf(fails ? "FAILED\n" : "OK\n");
    return fails != 0;
}
