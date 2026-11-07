// Virtual RomFS: the game's RomFS image, built in memory from a folder of loose files (the player's extracted
// romfs/) with mod folders layered on top. Only the image's tables live in memory; file data is read from the
// host files when the game reads that part of the image.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Builds the image from base_dir and the given overlay folders (later ones win; files they add are added).
bool romfs_open(const char *base_dir, const char *const *overlays, int noverlays);
uint64_t romfs_size(void);
// While romfs_open layers mods (any thread; the loading indicator): their files found so far (total 0, the folders
// still being read), or applied of total. False otherwise.
bool romfs_mods_progress(int *done, int *total);
// Reads image bytes [off, off+size) into dst. Returns the bytes read (short only past the end).
uint64_t romfs_read(uint8_t *dst, uint64_t off, uint64_t size);
// The RomFS path (e.g. "nativeNX/arc/...") of the file holding image offset off, or NULL.
const char *romfs_file_at(uint64_t off);
// The host file behind a RomFS path ("nativeNX/movie/movie00.mp4", a leading '/' allowed; a mod's copy if one
// replaces it), or NULL.
const char *romfs_host_path(const char *rpath);
