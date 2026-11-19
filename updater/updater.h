// MHGU Recomp updater (updater/updater.c): installs the latest GitHub release over the game folder it sits in, then
// starts the game. The pieces: HTTPS (http.c), SHA-256 (sha256.c), .zip / .tar.gz reading (archive.c), the
// release's JSON (json.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef MHGU_VERSION
#define MHGU_VERSION "unknown"  // (the Makefile gives the build's tag: git describe)
#endif

void ulog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  // update/updater.log and stderr

// ---- http.c: one GET, the body handed to sink as it arrives (false from it: stop)
typedef bool (*HttpSink)(void *ctx, const void *data, size_t size);
typedef struct {
    int status;             // HTTP status, 0: no answer (no connection, timed out)
    long rate_remaining;    // X-RateLimit-Remaining, -1: none
    char etag[128];         // ETag
    char error[256];        // why there was no answer
} HttpResult;
// if_none_match: an ETag (a 304 answer then means unchanged), or NULL. connect_timeout: seconds to the first byte;
// after that it waits as long as data keeps coming.
bool http_get(const char *url, const char *accept, const char *if_none_match, int connect_timeout, HttpSink sink,
              void *ctx, HttpResult *r);

// ---- sha256.c
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; } Sha256;
void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t size);
void sha256_hex(Sha256 *s, char out[65]);

// ---- archive.c: every file of a .zip or .tar.gz, through entry. name: the path inside it ('/' separated); mode: the
// tar's permission bits (0 from a zip). entry returns where to write the file's contents (NULL: skip it).
typedef struct SDL_IOStream SDL_IOStream;
typedef SDL_IOStream *(*ArchiveEntry)(void *ctx, const char *name, bool dir, unsigned mode);
bool archive_extract(const char *path, ArchiveEntry entry, void *ctx, char *error, size_t error_size);

// ---- json.c: the GitHub release (GET /repos/{owner}/{repo}/releases/latest)
typedef struct {
    char name[128], url[512], digest[80];  // digest: "sha256:<hex>" ("" when the asset has none)
    int64_t size;
} Asset;
typedef struct {
    char tag[64];
    bool draft, prerelease;
    int nassets;
    Asset assets[16];
} Release;
bool json_release(const char *text, size_t size, Release *r);
char *json_body(const char *text, size_t size);  // the release's notes (malloc'd; NULL: none)
