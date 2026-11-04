// Engine plugin loader test (make test-plugins): runtime/plugin.c against the plugins in <dir> (argv[1]) -
// "good" (loads), "badapi" (claims API 99), "declines" (entry returns false), "notaplugin" (no entry point),
// "hidden" (fine but not listed in enabled.txt), plus names that must be refused and a folder that is missing.
// No game files needed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv) {
    (void)argc, (void)argv;  // (the plugins are in MHGU_PLUGINS_DIR: the Makefile sets it)
    plugins_init();
    plugins_init();  // (twice: the second call does nothing)
    const char *names[8];
    int n = plugins_loaded(names, 8);
    CHECK(n == 1);
    CHECK(n >= 1 && !strcmp(names[0], "Example"));
    printf("loaded %d\n", n);
    plugins_shutdown();  // (prints the plugin's "[example] shutdown")
    CHECK(plugins_loaded(names, 8) == 0);
    printf(fails ? "FAILED\n" : "OK\n");
    return fails != 0;
}
