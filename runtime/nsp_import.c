// NSP → data/exefs + data/romfs via a shipped hactool. No crypto here: hactool does decryption.
#include "nsp_import.h"

#include "platform.h"
#include "rt.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#define HACTOOL_NAME "hactool.exe"
#else
#define PATH_SEP '/'
#define HACTOOL_NAME "hactool"
#endif

enum { MHGU_TITLE_ID = 0x0100770008DD8000ull };
enum { MHGU_PATCH_TITLE_ID = MHGU_TITLE_ID | 0x800ull };  // update CNMT: 0100770008DD8800
enum { MHGU_CNMT_VERSION = 262144u };  // 1.4.0 (v262144)
enum { NEED_FREE_BYTES = 20ull * 1024 * 1024 * 1024 };

typedef struct {
    NspImportRequest *req;
    char hactool[1100];
    char work[1100];
    char home[1100];
    char keys_file[1100];
    char base_nsp_dir[1100];
    char upd_nsp_dir[1100];
    char stage[1100];
    char base_nca[1100];
    char upd_nca[1100];
    char base_titlekey_enc[33];  // encrypted titlekey from base .tik (for --titlekey=)
    char upd_titlekey_enc[33];
    SDL_Mutex *child_lock;
    SDL_Process *child;
} ImportCtx;

typedef struct {
    char path[1100];
    int64_t size;
    char content_type[64];
    uint64_t title_id;
} NcaInfo;

bool nsp_import_available(void) {
#if defined(__ANDROID__)
    return false;
#else
    return true;
#endif
}

static bool cancelled(ImportCtx *ctx) { return ctx->req->cancel && *ctx->req->cancel; }

static void progress(ImportCtx *ctx, const char *stage, float frac) {
    if (ctx->req->progress) ctx->req->progress(ctx->req->progress_user, stage, frac);
}

static void set_err(ImportCtx *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->req->error, sizeof ctx->req->error, fmt, ap);
    va_end(ap);
}

static bool is_dir(const char *p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

static bool is_file(const char *p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE;
}

static bool ensure_dir(const char *p) { return is_dir(p) || SDL_CreateDirectory(p); }

static bool join2(char *out, size_t n, const char *a, const char *b) {
    return snprintf(out, n, "%s%c%s", a, PATH_SEP, b) < (int)n;
}

static SDL_EnumerationResult SDLCALL rm_entry(void *arg, const char *dir, const char *name) {
    char p[2048];
    snprintf(p, sizeof p, "%s%s", dir, name);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p, rm_entry, arg);
    if (!SDL_RemovePath(p)) *(bool *)arg = false;
    return SDL_ENUM_CONTINUE;
}

static bool rm_tree(const char *p) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(p, &info)) return true;
    bool ok = true;
    if (info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p, rm_entry, &ok);
    return SDL_RemovePath(p) && ok;
}

static bool copy_file(const char *src, const char *dst) {
    size_t n = 0;
    void *data = SDL_LoadFile(src, &n);
    if (!data) return false;
    bool ok = SDL_SaveFile(dst, data, n);
    SDL_free(data);
    return ok;
}

bool nsp_import_find_hactool(char *out, size_t size) {
    char exe[1024];
    if (plat_exe_path(exe, sizeof exe)) {
        char *s = strrchr(exe, '\\'), *s2 = strrchr(exe, '/');
        if (s2 > s) s = s2;
        if (s) {
            *s = 0;
            if (snprintf(out, size, "%s%c%s", exe, PATH_SEP, HACTOOL_NAME) < (int)size && is_file(out)) return true;
        }
    }
    const char *base = SDL_GetBasePath();
    if (base && snprintf(out, size, "%s%s", base, HACTOOL_NAME) < (int)size && is_file(out)) return true;
    if (snprintf(out, size, "%s", HACTOOL_NAME) < (int)size && is_file(out)) return true;
    return false;
}

static const char *find_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return p;
    }
    return NULL;
}

static bool parse_u64_hex(const char *s, uint64_t *out) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 16);
    if (end == s) return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_u32_decimal(const char *s, uint32_t *out) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

static bool extract_field(const char *text, const char *label, char *val, size_t val_size) {
    const char *p = find_ci(text, label);
    if (!p) return false;
    p += strlen(label);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r') p++;
    size_t i = 0;
    while (*p && *p != '\n' && *p != '\r' && i + 1 < val_size) val[i++] = *p++;
    val[i] = 0;
    while (i && (val[i - 1] == ' ' || val[i - 1] == '\t')) val[--i] = 0;
    return i > 0;
}

static bool ends_with_ci(const char *name, const char *suf) {
    size_t n = strlen(name), m = strlen(suf);
    if (n < m) return false;
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)name[n - m + i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}

// Run hactool with cancel support. stdout+stderr captured into out when non-NULL.
static int run_hactool(ImportCtx *ctx, char *out, size_t out_size, const char *const *argv) {
    if (cancelled(ctx)) return -1;

    SDL_Environment *env = SDL_CreateEnvironment(true);
    if (env) {
        SDL_SetEnvironmentVariable(env, "HOME", ctx->home, true);
#ifdef _WIN32
        SDL_SetEnvironmentVariable(env, "USERPROFILE", ctx->home, true);
#endif
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        if (env) SDL_DestroyEnvironment(env);
        return -1;
    }
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
    if (env) SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);

    SDL_Process *proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (env) SDL_DestroyEnvironment(env);
    if (!proc) {
        set_err(ctx, "Could not start hactool (%s).", SDL_GetError());
        return -1;
    }
    SDL_LockMutex(ctx->child_lock);
    ctx->child = proc;
    SDL_UnlockMutex(ctx->child_lock);

    if (out && out_size) out[0] = 0;
    size_t got = 0;
    SDL_IOStream *reader = SDL_GetProcessOutput(proc);
    for (;;) {
        if (cancelled(ctx)) {
            SDL_KillProcess(proc, true);
            SDL_WaitProcess(proc, true, NULL);
            SDL_LockMutex(ctx->child_lock);
            ctx->child = NULL;
            SDL_UnlockMutex(ctx->child_lock);
            SDL_DestroyProcess(proc);
            return -1;
        }
        if (reader) {
            char buf[4096];
            size_t n = SDL_ReadIO(reader, buf, sizeof buf);
            if (n > 0 && out && out_size && got + 1 < out_size) {
                size_t take = n;
                if (got + take >= out_size) take = out_size - 1 - got;
                memcpy(out + got, buf, take);
                got += take;
                out[got] = 0;
            }
        }
        int code = 0;
        if (SDL_WaitProcess(proc, false, &code)) {
            if (reader && out && out_size) {
                char buf[4096];
                for (;;) {
                    size_t n = SDL_ReadIO(reader, buf, sizeof buf);
                    if (!n) break;
                    if (got + 1 < out_size) {
                        size_t take = n;
                        if (got + take >= out_size) take = out_size - 1 - got;
                        memcpy(out + got, buf, take);
                        got += take;
                        out[got] = 0;
                    }
                }
            }
            SDL_LockMutex(ctx->child_lock);
            ctx->child = NULL;
            SDL_UnlockMutex(ctx->child_lock);
            SDL_DestroyProcess(proc);
            return code;
        }
        SDL_Delay(20);
    }
}

static int hactool_v(ImportCtx *ctx, char *out, size_t out_size, const char **extra, int nextra) {
    const char *argv[24];
    int n = 0;
    argv[n++] = ctx->hactool;
    argv[n++] = "-k";
    argv[n++] = ctx->keys_file;
    argv[n++] = "--disablekeywarns";
    for (int i = 0; i < nextra && n < 23; i++) argv[n++] = extra[i];
    argv[n] = NULL;
    return run_hactool(ctx, out, out_size, argv);
}

static bool setup_keys(ImportCtx *ctx) {
    if (!is_file(ctx->req->prod_keys)) {
        set_err(ctx, "Decryption keys were not found. Select your prod.keys file (from your own Switch).");
        return false;
    }
    char switch_dir[1100];
    join2(switch_dir, sizeof switch_dir, ctx->home, ".switch");
    if (!ensure_dir(ctx->home) || !ensure_dir(switch_dir)) {
        set_err(ctx, "Could not create a temporary key folder.");
        return false;
    }
    join2(ctx->keys_file, sizeof ctx->keys_file, switch_dir, "prod.keys");
    if (!copy_file(ctx->req->prod_keys, ctx->keys_file)) {
        set_err(ctx, "Could not read prod.keys.");
        return false;
    }
    if (ctx->req->title_keys && ctx->req->title_keys[0] && is_file(ctx->req->title_keys)) {
        char dest[1100];
        join2(dest, sizeof dest, switch_dir, "title.keys");
        if (!copy_file(ctx->req->title_keys, dest)) {
            set_err(ctx, "Could not read title.keys.");
            return false;
        }
    }
    return true;
}

static bool check_space(ImportCtx *ctx) {
    uint64_t free_b = 0;
    if (!plat_disk_free(ctx->req->data_dir, &free_b) && !plat_disk_free(".", &free_b)) {
        LOG("[nsp] free-space check unavailable; continuing\n");
        return true;
    }
    if (free_b < NEED_FREE_BYTES) {
        set_err(ctx,
                "Not enough free storage: need about 20 GB free to extract and install the game files (have %.1f GB).",
                free_b / (1024.0 * 1024.0 * 1024.0));
        return false;
    }
    return true;
}

// hactool 1.4 has no -t tik: read common-ticket fields and let hactool decrypt via --titlekey=.
static bool parse_ticket_file(const char *path, char rights_hex[33], char enc_hex[33]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0x2B0) { fclose(f); return false; }
    uint8_t enc[16], rights[16];
    if (fseek(f, 0x180, SEEK_SET) != 0 || fread(enc, 1, 16, f) != 16 ||
        fseek(f, 0x2A0, SEEK_SET) != 0 || fread(rights, 1, 16, f) != 16) {
        fclose(f);
        return false;
    }
    fclose(f);
    static const char *hexd = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        enc_hex[i * 2] = hexd[enc[i] >> 4];
        enc_hex[i * 2 + 1] = hexd[enc[i] & 0xf];
        rights_hex[i * 2] = hexd[rights[i] >> 4];
        rights_hex[i * 2 + 1] = hexd[rights[i] & 0xf];
    }
    enc_hex[32] = rights_hex[32] = 0;
    return true;
}

typedef struct {
    ImportCtx *ctx;
    FILE *tk;
    char sample_nca[1100];
    char *store_enc;  // optional 33-byte buffer for encrypted titlekey
} TikScan;

static SDL_EnumerationResult SDLCALL pick_sample_nca(void *userdata, const char *dirname, const char *name) {
    TikScan *t = userdata;
    if (!ends_with_ci(name, ".nca") || find_ci(name, "cnmt")) return SDL_ENUM_CONTINUE;
    char path[1200];
    snprintf(path, sizeof path, "%s%s", dirname, name);
    SDL_PathInfo pi, cur;
    if (!SDL_GetPathInfo(path, &pi) || pi.type != SDL_PATHTYPE_FILE) return SDL_ENUM_CONTINUE;
    if (!t->sample_nca[0] || (SDL_GetPathInfo(t->sample_nca, &cur) && pi.size > cur.size))
        snprintf(t->sample_nca, sizeof t->sample_nca, "%s", path);
    return SDL_ENUM_CONTINUE;
}

static SDL_EnumerationResult SDLCALL on_tik(void *userdata, const char *dirname, const char *name) {
    TikScan *t = userdata;
    if (!ends_with_ci(name, ".tik") || !t->sample_nca[0]) return SDL_ENUM_CONTINUE;
    char path[1200], rights[33], enc[33], out[8192];
    snprintf(path, sizeof path, "%s%s", dirname, name);
    if (!parse_ticket_file(path, rights, enc)) return SDL_ENUM_CONTINUE;
    if (t->store_enc && !t->store_enc[0]) snprintf(t->store_enc, 33, "%s", enc);
    // Probe that prod.keys can unwrap this ticket; title.keys stores the *encrypted*
    // titlekey (hactool applies titlekek). Decrypted values break --basenca merges.
    char titlekey_arg[80];
    snprintf(titlekey_arg, sizeof titlekey_arg, "--titlekey=%s", enc);
    const char *args[] = {titlekey_arg, "-t", "nca", t->sample_nca};
    hactool_v(t->ctx, out, sizeof out, args, 4);
    char key[80];
    if (!extract_field(out, "Titlekey (Decrypted)", key, sizeof key) &&
        !extract_field(out, "Titlekey (decrypted)", key, sizeof key))
        return SDL_ENUM_CONTINUE;
    fprintf(t->tk, "%s=%s\n", rights, enc);
    return SDL_ENUM_CONTINUE;
}

static void ingest_tickets(ImportCtx *ctx, const char *nsp_dir, char *store_enc) {
    char title_keys[1100], switch_dir[1100];
    join2(switch_dir, sizeof switch_dir, ctx->home, ".switch");
    join2(title_keys, sizeof title_keys, switch_dir, "title.keys");
    FILE *tk = fopen(title_keys, "a");
    if (!tk) return;
    TikScan scan = {ctx, tk, {0}, store_enc};
    SDL_EnumerateDirectory(nsp_dir, pick_sample_nca, &scan);
    SDL_EnumerateDirectory(nsp_dir, on_tik, &scan);
    fclose(tk);
}

static bool unpack_nsp(ImportCtx *ctx, const char *nsp, const char *outdir, const char *label, float frac,
                       char *store_enc) {
    progress(ctx, label, frac);
    if (cancelled(ctx)) return false;
    rm_tree(outdir);
    if (!ensure_dir(outdir)) {
        set_err(ctx, "Could not create temporary folder %s.", outdir);
        return false;
    }
    if (!is_file(nsp)) {
        set_err(ctx, "NSP file not found: %s", nsp);
        return false;
    }
    {
        SDL_IOStream *io = SDL_IOFromFile(nsp, "rb");
        char magic[4] = {0};
        bool ok = io && SDL_ReadIO(io, magic, 4) == 4;
        if (io) SDL_CloseIO(io);
        if (!ok || memcmp(magic, "PFS0", 4) != 0) {
            set_err(ctx, "Not a valid NSP (missing PFS0 header): %s", nsp);
            return false;
        }
    }
    char out[4096];
    const char *args[] = {"-t", "pfs0", "--outdir", outdir, nsp};
    int code = hactool_v(ctx, out, sizeof out, args, 5);
    if (cancelled(ctx)) return false;
    if (code != 0) {
        if (find_ci(out, "key") || find_ci(out, "Unable to decrypt") || find_ci(out, "missing"))
            set_err(ctx, "Could not unpack the NSP. Check that prod.keys is complete and matches this dump.");
        else
            set_err(ctx, "hactool failed while unpacking %s (exit %d).", label, code);
        return false;
    }
    ingest_tickets(ctx, outdir, store_enc);
    return true;
}

static bool nca_info(ImportCtx *ctx, const char *path, NcaInfo *info) {
    memset(info, 0, sizeof *info);
    snprintf(info->path, sizeof info->path, "%s", path);
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(path, &pi)) info->size = (int64_t)pi.size;
    char out[16384];
    const char *args[] = {"-t", "nca", path};
    if (hactool_v(ctx, out, sizeof out, args, 3) != 0) return false;
    extract_field(out, "Content Type", info->content_type, sizeof info->content_type);
    char tid[64];
    if (extract_field(out, "Title ID", tid, sizeof tid)) parse_u64_hex(tid, &info->title_id);
    return true;
}

typedef struct {
    ImportCtx *ctx;
    NcaInfo *program;
    NcaInfo *meta;
} ScanNca;

static SDL_EnumerationResult SDLCALL scan_nca_entry(void *userdata, const char *dirname, const char *name) {
    ScanNca *s = userdata;
    if (!ends_with_ci(name, ".nca")) return SDL_ENUM_CONTINUE;
    char path[1200];
    snprintf(path, sizeof path, "%s%s", dirname, name);
    if (!is_file(path)) return SDL_ENUM_CONTINUE;
    NcaInfo info;
    if (!nca_info(s->ctx, path, &info)) return SDL_ENUM_CONTINUE;
    if (find_ci(info.content_type, "Meta") || find_ci(name, "cnmt")) {
        if (!s->meta->path[0] || info.size <= s->meta->size) *s->meta = info;
    }
    if (find_ci(info.content_type, "Program")) {
        if (!s->program->path[0] || info.size > s->program->size) *s->program = info;
    }
    return SDL_ENUM_CONTINUE;
}

static bool find_ncas(ImportCtx *ctx, const char *dir, NcaInfo *program, NcaInfo *meta) {
    memset(program, 0, sizeof *program);
    memset(meta, 0, sizeof *meta);
    ScanNca s = {ctx, program, meta};
    if (!SDL_EnumerateDirectory(dir, scan_nca_entry, &s)) {
        set_err(ctx, "Could not list extracted NSP contents (%s).", dir);
        return false;
    }
    if (!program->path[0]) {
        // Fallback: largest .nca that is not Meta
        set_err(ctx, "No Program NCA found in the NSP. Is this a valid game or update package?");
        return false;
    }
    return true;
}

typedef struct {
    char path[1100];
} FoundFile;

static SDL_EnumerationResult SDLCALL first_file(void *u, const char *dir, const char *name) {
    FoundFile *ff = u;
    if (name[0] == '.') return SDL_ENUM_CONTINUE;
    char path[1100];
    snprintf(path, sizeof path, "%s%s", dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_EnumerateDirectory(path, first_file, ff);
        return ff->path[0] ? SDL_ENUM_SUCCESS : SDL_ENUM_CONTINUE;
    }
    if (info.type == SDL_PATHTYPE_FILE) {
        snprintf(ff->path, sizeof ff->path, "%s", path);
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

static bool read_cnmt_version(ImportCtx *ctx, const NcaInfo *meta, uint64_t *title_id, uint32_t *version) {
    *title_id = meta->title_id;
    *version = 0;
    if (!meta->path[0]) return *title_id != 0;

    char secdir[1100];
    join2(secdir, sizeof secdir, ctx->work, "cnmt_sec");
    rm_tree(secdir);
    ensure_dir(secdir);
    const char *args[] = {"-t", "nca", "--section0dir", secdir, meta->path};
    if (hactool_v(ctx, NULL, 0, args, 5) == 0) {
        FoundFile f = {{0}};
        SDL_EnumerateDirectory(secdir, first_file, &f);
        if (f.path[0] && is_file(f.path)) {
            FILE *fp = fopen(f.path, "rb");
            if (fp) {
                uint8_t hdr[16];
                if (fread(hdr, 1, 16, fp) == 16) {
                    // CNMT: title_id u64 LE @0, title_version u32 LE @8
                    uint64_t tid_bin = (uint64_t)hdr[0] | ((uint64_t)hdr[1] << 8) | ((uint64_t)hdr[2] << 16) |
                                       ((uint64_t)hdr[3] << 24) | ((uint64_t)hdr[4] << 32) | ((uint64_t)hdr[5] << 40) |
                                       ((uint64_t)hdr[6] << 48) | ((uint64_t)hdr[7] << 56);
                    uint32_t ver_bin = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) |
                                       ((uint32_t)hdr[11] << 24);
                    if (tid_bin) *title_id = tid_bin;
                    if (ver_bin) *version = ver_bin;
                }
                fclose(fp);
            }
        }
    }
    return *title_id != 0;
}

static bool rename_swap(const char *from, const char *to) {
    rm_tree(to);
    return SDL_RenamePath(from, to);
}

static bool install_staging(ImportCtx *ctx) {
    progress(ctx, "Validating extracted files…", 0.85f);
    char msg[900];
    if (game_files_problem(ctx->stage, msg, sizeof msg)) {
        set_err(ctx, "%s", msg);
        return false;
    }
    progress(ctx, "Installing into data…", 0.92f);
    char dst_exefs[1100], dst_romfs[1100], src_exefs[1100], src_romfs[1100];
    char bak_exefs[1100], bak_romfs[1100];
    join2(dst_exefs, sizeof dst_exefs, ctx->req->data_dir, "exefs");
    join2(dst_romfs, sizeof dst_romfs, ctx->req->data_dir, "romfs");
    join2(src_exefs, sizeof src_exefs, ctx->stage, "exefs");
    join2(src_romfs, sizeof src_romfs, ctx->stage, "romfs");
    join2(bak_exefs, sizeof bak_exefs, ctx->req->data_dir, ".import_old_exefs");
    join2(bak_romfs, sizeof bak_romfs, ctx->req->data_dir, ".import_old_romfs");
    ensure_dir(ctx->req->data_dir);
    rm_tree(bak_exefs);
    rm_tree(bak_romfs);
    if (is_dir(dst_exefs) && !rename_swap(dst_exefs, bak_exefs)) {
        set_err(ctx, "Could not move the old exefs aside.");
        return false;
    }
    if (is_dir(dst_romfs) && !rename_swap(dst_romfs, bak_romfs)) {
        if (is_dir(bak_exefs)) rename_swap(bak_exefs, dst_exefs);
        set_err(ctx, "Could not move the old romfs aside.");
        return false;
    }
    if (!rename_swap(src_exefs, dst_exefs) || !rename_swap(src_romfs, dst_romfs)) {
        if (is_dir(bak_exefs)) {
            rm_tree(dst_exefs);
            rename_swap(bak_exefs, dst_exefs);
        }
        if (is_dir(bak_romfs)) {
            rm_tree(dst_romfs);
            rename_swap(bak_romfs, dst_romfs);
        }
        set_err(ctx, "Could not install the new game files into data/.");
        return false;
    }
    rm_tree(bak_exefs);
    rm_tree(bak_romfs);
    return true;
}

static void cleanup(ImportCtx *ctx) {
    if (ctx->child_lock) {
        SDL_LockMutex(ctx->child_lock);
        if (ctx->child) {
            SDL_KillProcess(ctx->child, true);
            SDL_WaitProcess(ctx->child, true, NULL);
            SDL_DestroyProcess(ctx->child);
            ctx->child = NULL;
        }
        SDL_UnlockMutex(ctx->child_lock);
        SDL_DestroyMutex(ctx->child_lock);
        ctx->child_lock = NULL;
    }
    if (ctx->work[0]) rm_tree(ctx->work);
    if (ctx->stage[0]) rm_tree(ctx->stage);
    if (ctx->req && ctx->req->data_dir) {
        char p[1100];
        join2(p, sizeof p, ctx->req->data_dir, ".import_old_exefs");
        rm_tree(p);
        join2(p, sizeof p, ctx->req->data_dir, ".import_old_romfs");
        rm_tree(p);
    }
}

int nsp_import_run(NspImportRequest *req) {
    ImportCtx ctx = {0};
    ctx.req = req;
    req->error[0] = 0;
    if (!nsp_import_available()) {
        snprintf(req->error, sizeof req->error, "NSP import is not available on this platform.");
        return NSP_IMPORT_ERR;
    }
    if (!req->data_dir || !req->base_nsp || !req->update_nsp || !req->prod_keys) {
        snprintf(req->error, sizeof req->error, "Missing import paths.");
        return NSP_IMPORT_ERR;
    }

    ctx.child_lock = SDL_CreateMutex();
    if (!ctx.child_lock) {
        snprintf(req->error, sizeof req->error, "Out of memory.");
        return NSP_IMPORT_ERR;
    }

    if (req->hactool && req->hactool[0])
        snprintf(ctx.hactool, sizeof ctx.hactool, "%s", req->hactool);
    else if (!nsp_import_find_hactool(ctx.hactool, sizeof ctx.hactool)) {
        snprintf(req->error, sizeof req->error,
                 "hactool was not found next to mhgurecomp. Reinstall the release or build hactool (make hactool).");
        cleanup(&ctx);
        return NSP_IMPORT_ERR;
    }

    {
        // Prefer TEMP/TMP; SDL_GetTempDir is not in all SDL3 builds we link.
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !tmp[0]) tmp = getenv("TEMP");
        if (!tmp || !tmp[0]) tmp = getenv("TMP");
        if (!tmp || !tmp[0]) tmp = ".";
        size_t n = strlen(tmp);
        if (n > 0 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\'))
            snprintf(ctx.work, sizeof ctx.work, "%smhgu_nsp_%lu", tmp, (unsigned long)plat_process_id());
        else
            snprintf(ctx.work, sizeof ctx.work, "%s%cmhgu_nsp_%lu", tmp, PATH_SEP,
                     (unsigned long)plat_process_id());
    }
    join2(ctx.home, sizeof ctx.home, ctx.work, "home");
    join2(ctx.base_nsp_dir, sizeof ctx.base_nsp_dir, ctx.work, "base_nsp");
    join2(ctx.upd_nsp_dir, sizeof ctx.upd_nsp_dir, ctx.work, "upd_nsp");
    join2(ctx.stage, sizeof ctx.stage, req->data_dir, ".import_staging");

    rm_tree(ctx.work);
    rm_tree(ctx.stage);
    if (!ensure_dir(ctx.work) || !ensure_dir(ctx.home)) {
        set_err(&ctx, "Could not create temporary folders.");
        cleanup(&ctx);
        return NSP_IMPORT_ERR;
    }

    if (!setup_keys(&ctx) || !check_space(&ctx)) {
        cleanup(&ctx);
        return NSP_IMPORT_ERR;
    }
    if (cancelled(&ctx)) {
        cleanup(&ctx);
        return NSP_IMPORT_CANCELLED;
    }

    if (!unpack_nsp(&ctx, req->base_nsp, ctx.base_nsp_dir, "Unpacking base game NSP…", 0.08f,
                    ctx.base_titlekey_enc)) {
        int r = cancelled(&ctx) ? NSP_IMPORT_CANCELLED : NSP_IMPORT_ERR;
        cleanup(&ctx);
        return r;
    }
    if (!unpack_nsp(&ctx, req->update_nsp, ctx.upd_nsp_dir, "Unpacking update NSP…", 0.22f,
                    ctx.upd_titlekey_enc)) {
        int r = cancelled(&ctx) ? NSP_IMPORT_CANCELLED : NSP_IMPORT_ERR;
        cleanup(&ctx);
        return r;
    }

    progress(&ctx, "Identifying game packages…", 0.35f);
    NcaInfo base_prog = {0}, base_meta = {0}, upd_prog = {0}, upd_meta = {0};
    if (!find_ncas(&ctx, ctx.base_nsp_dir, &base_prog, &base_meta) ||
        !find_ncas(&ctx, ctx.upd_nsp_dir, &upd_prog, &upd_meta)) {
        cleanup(&ctx);
        return cancelled(&ctx) ? NSP_IMPORT_CANCELLED : NSP_IMPORT_ERR;
    }
    snprintf(ctx.base_nca, sizeof ctx.base_nca, "%s", base_prog.path);
    snprintf(ctx.upd_nca, sizeof ctx.upd_nca, "%s", upd_prog.path);

    uint64_t title_id = 0;
    uint32_t version = 0;
    progress(&ctx, "Checking title ID and version…", 0.45f);
    if (!read_cnmt_version(&ctx, &upd_meta, &title_id, &version) && upd_prog.title_id) title_id = upd_prog.title_id;
    if (!title_id && base_prog.title_id) title_id = base_prog.title_id;
    // Update CNMTs use the patch title ID (base | 0x800 → …8800); base NSPs use …8000.
    if (title_id && title_id != MHGU_TITLE_ID && title_id != MHGU_PATCH_TITLE_ID) {
        set_err(&ctx,
                "Wrong game: title ID %016llX, expected 0100770008DD8000 (or update 0100770008DD8800).",
                (unsigned long long)title_id);
        cleanup(&ctx);
        return NSP_IMPORT_ERR;
    }
    if (version && version != MHGU_CNMT_VERSION) {
        set_err(&ctx, "Wrong update version (CNMT v%u). Need Monster Hunter Generations Ultimate update 1.4.0 (v%u).",
                version, MHGU_CNMT_VERSION);
        cleanup(&ctx);
        return NSP_IMPORT_ERR;
    }

    progress(&ctx, "Extracting update ExeFS and merged RomFS…", 0.55f);
    {
        char exefs[1100], romfs[1100];
        join2(exefs, sizeof exefs, ctx.stage, "exefs");
        join2(romfs, sizeof romfs, ctx.stage, "romfs");
        rm_tree(exefs);
        rm_tree(romfs);
        ensure_dir(ctx.stage);
        ensure_dir(exefs);
        ensure_dir(romfs);
        char basenca_arg[1200], upd_tk[80], base_tk[80], exefs_arg[1200], romfs_arg[1200];
        snprintf(basenca_arg, sizeof basenca_arg, "--basenca=%s", ctx.base_nca);
        snprintf(exefs_arg, sizeof exefs_arg, "--exefsdir=%s", exefs);
        snprintf(romfs_arg, sizeof romfs_arg, "--romfsdir=%s", romfs);
        snprintf(upd_tk, sizeof upd_tk, "--titlekey=%s",
                 ctx.upd_titlekey_enc[0] ? ctx.upd_titlekey_enc : "0");
        snprintf(base_tk, sizeof base_tk, "--titlekey=%s",
                 ctx.base_titlekey_enc[0] ? ctx.base_titlekey_enc : "0");
        char out[8192];
        // ExeFS: CLI --titlekey= for the update NCA only (no basenca).
        const char *args_exefs[] = {upd_tk, "-t", "nca", exefs_arg, ctx.upd_nca};
        int code = hactool_v(&ctx, out, sizeof out, args_exefs, 5);
        char mainp_early[1200];
        join2(mainp_early, sizeof mainp_early, exefs, "main");
        if (cancelled(&ctx)) {
            cleanup(&ctx);
            return NSP_IMPORT_CANCELLED;
        }
        if (code != 0 || !is_file(mainp_early)) {
            set_err(&ctx, "Could not extract update ExeFS (exefs/main missing). Check the 1.4.0 update NSP and keys.");
            cleanup(&ctx);
            return NSP_IMPORT_ERR;
        }
        // RomFS merge: never pass --titlekey= here — it would also be used for --basenca and
        // corrupt patched files. title.keys holds each NCA's encrypted titlekey by rights id.
        progress(&ctx, "Merging base + update RomFS…", 0.62f);
        const char *args_romfs[] = {"-t", "nca", basenca_arg, romfs_arg, ctx.upd_nca};
        code = hactool_v(&ctx, out, sizeof out, args_romfs, 5);
        if (cancelled(&ctx)) {
            cleanup(&ctx);
            return NSP_IMPORT_CANCELLED;
        }
        if (code != 0) {
            progress(&ctx, "Retrying extraction (base RomFS, then update)…", 0.58f);
            rm_tree(romfs);
            ensure_dir(romfs);
            const char *args2[] = {base_tk, "-t", "nca", romfs_arg, ctx.base_nca};
            code = hactool_v(&ctx, out, sizeof out, args2, 5);
            if (code != 0) {
                set_err(&ctx, "Could not extract the base game RomFS. Check keys and that the base NSP is MHGU.");
                cleanup(&ctx);
                return NSP_IMPORT_ERR;
            }
            if (cancelled(&ctx)) {
                cleanup(&ctx);
                return NSP_IMPORT_CANCELLED;
            }
            code = hactool_v(&ctx, out, sizeof out, args_romfs, 5);
            if (code != 0) {
                set_err(&ctx,
                        "Could not apply the 1.4.0 update onto the base RomFS. The update NSP may be wrong or keys may "
                        "be incomplete.");
                cleanup(&ctx);
                return NSP_IMPORT_ERR;
            }
        }
        char mainp[1200];
        join2(mainp, sizeof mainp, exefs, "main");
        if (!is_file(mainp)) {
            set_err(&ctx, "Extraction finished but exefs/main is missing.");
            cleanup(&ctx);
            return NSP_IMPORT_ERR;
        }
    }

    if (!install_staging(&ctx)) {
        int r = cancelled(&ctx) ? NSP_IMPORT_CANCELLED : NSP_IMPORT_ERR;
        cleanup(&ctx);
        return r;
    }

    progress(&ctx, "Cleaning up…", 0.98f);
    rm_tree(ctx.work);
    rm_tree(ctx.stage);
    ctx.work[0] = 0;
    ctx.stage[0] = 0;
    if (ctx.child_lock) {
        SDL_DestroyMutex(ctx.child_lock);
        ctx.child_lock = NULL;
    }
    progress(&ctx, "Done.", 1.0f);
    return NSP_IMPORT_OK;
}
