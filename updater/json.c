// The parts of a GitHub release's JSON the updater uses (updater.h Release); everything else is skipped.
#include <stdlib.h>
#include <string.h>

#include "updater.h"

typedef struct { const char *p, *end; int depth; } J;

static void ws(J *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}
static bool eat(J *j, char c) {
    ws(j);
    if (j->p < j->end && *j->p == c) { j->p++; return true; }
    return false;
}

static void put(char *out, size_t size, size_t *n, unsigned c) {  // (UTF-8)
    char b[4];
    int k = c < 0x80 ? (b[0] = (char)c, 1)
          : c < 0x800 ? (b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), 2)
                      : (b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), 3);
    for (int i = 0; i < k; i++) if (out && *n + 1 < size) out[(*n)++] = b[i];
}

// A string into out (cut to size; out NULL: skipped)
static bool str(J *j, char *out, size_t size) {
    size_t n = 0;
    if (!eat(j, '"')) return false;
    while (j->p < j->end && *j->p != '"') {
        unsigned c = (unsigned char)*j->p++;
        if (c == '\\') {
            if (j->p >= j->end) return false;
            c = (unsigned char)*j->p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                if (j->end - j->p < 4) return false;
                char hex[5] = {j->p[0], j->p[1], j->p[2], j->p[3], 0};
                c = (unsigned)strtoul(hex, NULL, 16);
                j->p += 4;
                put(out, size, &n, c);
                continue;
            }
            }
        }
        if (out && n + 1 < size) out[n++] = (char)c;
    }
    if (out && size) out[n] = 0;
    return eat(j, '"');
}

static bool skip(J *j) {
    ws(j);
    if (j->p >= j->end || ++j->depth > 64) return false;
    bool ok = true;
    if (*j->p == '"') ok = str(j, NULL, 0);
    else if (*j->p == '{' || *j->p == '[') {
        char close = *j->p++ == '{' ? '}' : ']';
        if (!eat(j, close)) {
            do {
                if (close == '}' && !(str(j, NULL, 0) && eat(j, ':'))) return false;
                if (!skip(j)) return false;
            } while (eat(j, ','));
            ok = eat(j, close);
        }
    } else {  // a number, true, false, null
        const char *s = j->p;
        while (j->p < j->end && !strchr(",}] \t\r\n", *j->p)) j->p++;
        ok = j->p > s;
    }
    j->depth--;
    return ok;
}

static bool boolean(J *j, bool *v) {
    ws(j);
    if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) { *v = true; j->p += 4; return true; }
    if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) { *v = false; j->p += 5; return true; }
    return skip(j);  // (null)
}

static bool number(J *j, int64_t *v) {
    ws(j);
    char buf[32];
    size_t n = 0;
    while (j->p < j->end && n + 1 < sizeof buf && strchr("-0123456789", *j->p)) buf[n++] = *j->p++;
    buf[n] = 0;
    if (!n) return skip(j);
    *v = strtoll(buf, NULL, 10);
    return true;
}

static bool asset(J *j, Asset *a) {
    char key[64];
    memset(a, 0, sizeof *a);
    if (!eat(j, '{')) return false;
    if (eat(j, '}')) return true;
    do {
        if (!str(j, key, sizeof key) || !eat(j, ':')) return false;
        bool ok = !strcmp(key, "name") ? str(j, a->name, sizeof a->name)
                : !strcmp(key, "browser_download_url") ? str(j, a->url, sizeof a->url)
                : !strcmp(key, "digest") ? (ws(j), j->p < j->end && *j->p == '"' ? str(j, a->digest, sizeof a->digest)
                                                                                    : skip(j))  // (null: none)
                : !strcmp(key, "size") ? number(j, &a->size)
                : skip(j);
        if (!ok) return false;
    } while (eat(j, ','));
    return eat(j, '}');
}

// The release's notes (its "body", Markdown as written on GitHub): malloc'd, NULL when it has none
char *json_body(const char *text, size_t size) {
    J j = {text, text + size, 0};
    char key[64];
    if (!eat(&j, '{') || eat(&j, '}')) return NULL;
    do {
        if (!str(&j, key, sizeof key) || !eat(&j, ':')) return NULL;
        ws(&j);
        if (!strcmp(key, "body") && j.p < j.end && *j.p == '"') {
            size_t cap = (size_t)(j.end - j.p) + 1;  // (the decoded text is never longer than the JSON's)
            char *out = malloc(cap);
            if (out && str(&j, out, cap)) return out;
            free(out);
            return NULL;
        }
        if (!skip(&j)) return NULL;
    } while (eat(&j, ','));
    return NULL;
}

bool json_release(const char *text, size_t size, Release *r) {
    J j = {text, text + size, 0};
    char key[64];
    memset(r, 0, sizeof *r);
    r->draft = r->prerelease = true;  // (unless it says otherwise)
    if (!eat(&j, '{')) return false;
    if (eat(&j, '}')) return false;
    do {
        if (!str(&j, key, sizeof key) || !eat(&j, ':')) return false;
        bool ok;
        if (!strcmp(key, "tag_name")) ok = str(&j, r->tag, sizeof r->tag);
        else if (!strcmp(key, "draft")) ok = boolean(&j, &r->draft);
        else if (!strcmp(key, "prerelease")) ok = boolean(&j, &r->prerelease);
        else if (!strcmp(key, "assets")) {
            ok = eat(&j, '[');
            if (ok && !eat(&j, ']')) {
                do {
                    Asset a;
                    if (!(ok = asset(&j, &a))) break;
                    if (r->nassets < (int)(sizeof r->assets / sizeof r->assets[0])) r->assets[r->nassets++] = a;
                } while (eat(&j, ','));
                ok = ok && eat(&j, ']');
            }
        } else ok = skip(&j);
        if (!ok) return false;
    } while (eat(&j, ','));
    return eat(&j, '}') && r->tag[0];
}
