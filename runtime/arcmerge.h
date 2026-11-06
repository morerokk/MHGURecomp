// Merging the same MT Framework archive (.arc) from several mods (romfs.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>

// `orig` is the game's own archive, `prev` the one a mod put in its place so far, `mod` another mod's. Writes to a
// cache file (path in `out`) `prev` with the entries `mod` changed against `orig` (or added); entries `mod` left as in
// `orig` keep `prev`'s. False when a file isn't an archive of the same format: the caller then replaces `prev`.
bool arc_merge(const char *orig, const char *prev, const char *mod, char *out, size_t outsz);

// `mod` holds only some of `orig`'s entries, so it goes over `orig` rather than replacing it. One with none of them is
// another archive altogether (or an emptied one): that still replaces. Reads the entry tables only.
bool arc_partial(const char *orig, const char *mod);

// How many entries `a` and `b` (two mods' copies of the archive `orig`) both changed, differently: those can't be
// both kept. `name` gets the first one. -1 when a file isn't an archive of the same format.
int arc_overlap(const char *orig, const char *a, const char *b, char *name, size_t namesz);
