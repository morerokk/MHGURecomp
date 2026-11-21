// fsp-srv: the RomFS as a (virtual) storage image, save data and SD card backed by host directories.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../config.h"
#include "../hle.h"
#include "../ipc.h"
#include "../mods.h"
#include "../romfs.h"

#define FS_PATH_NOT_FOUND MAKE_RESULT(2, 1)
#define FS_PATH_EXISTS MAKE_RESULT(2, 2)
#define FS_NOT_EMPTY MAKE_RESULT(2, 8)

const char *g_save_dir = "save";
const char *g_backup_dir;  // ([Application] BackupLocation, ui.c; MHGU_BACKUP_DIR overrides)

// ------------------------------------------------------------------ IStorage (the RomFS image)
static void storage(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // Read(offset, size) -> B buffer
        uint64_t off = IN(uint64_t, 0), size = IN(uint64_t, 8);
        IpcBuf *b = out_buf(req, 0);
        if (!b) { resp->result = MAKE_RESULT(2, 6001); break; }
        if (size > b->size) size = b->size;
        romfs_read(BUFP(b), off, size);
        if (ENV("MHGU_LOG_FILES")) {
            static const char *last;
            const char *path = romfs_file_at(off);
            if (path && path != last) { last = path; LOG("[fs] read %s\n", path); }
        }
        break;
    }
    case 4: OUT(uint64_t, 0) = romfs_size(); resp_size(resp, 8); break;  // GetSize
    default: LOG("[fs] IStorage cmd %u (ok)\n", req->cmd);
    }
}

static Service *open_romfs_storage(void) {
    static bool opened;
    if (!opened) {
        char base[1024];
        snprintf(base, sizeof base, "%s/romfs", g_game_dir);
        const char *layers[64];
        int n = mods_romfs_layers(layers, 64);
        if (!romfs_open(base, layers, n)) FATAL("cannot read the game's romfs folder (%s)\n", base);
        opened = true;
    }
    return service_new("IStorage(romfs)", storage, NULL);
}

// ------------------------------------------------------------------ host-directory filesystem
// Save data is transactional on the Switch: changes become real at Commit, and an unmount without one discards them.
// The game saves by deleting `system`, creating it again empty and writing ~5 MB into it, then Commit. Done directly on
// the host files, a failure or a closed game in between left no save at all (seen with a save folder on a NAS), so a
// save mount keeps its changes aside: a created file is written as <name>.mhgu-tmp beside it, a deleted one only
// marked, and Commit applies them. The commit point is the rename to <name>.mhgu-commit (complete data); its content
// is then copied over the real file in place (which keeps a symlinked file a symlink; a rename would replace it) and
// it is deleted. At the first mount of a folder, a leftover .mhgu-commit is applied (the copy was cut short) and a
// leftover .mhgu-tmp is deleted (never committed). The SD card file system has no commit and writes directly.
#define TMP_EXT    ".mhgu-tmp"
#define COMMIT_EXT ".mhgu-commit"
#define MAX_PENDING 16
typedef struct { char path[1100]; bool deleted, created; } Pending;  // created: the new content is in path + TMP_EXT
typedef struct { char root[1024]; bool journal; int npending; Pending pending[MAX_PENDING]; } HostFs;
typedef struct { int fd; char path[1100]; } HostFile;
typedef struct { DIR *d; char path[1024]; int mode; } HostDir;

static const char *in_path(IpcReq *req, int i) {
    if (i < req->nx) return gstr((uint32_t)req->x[i].addr);
    if (i < req->na) return gstr((uint32_t)req->a[i].addr);
    return "";
}

static void host_path(HostFs *fs, const char *p, char *out, size_t n) {
    snprintf(out, n, "%s%s%s", fs->root, p[0] == '/' ? "" : "/", p);
}

static uint32_t errno_result(void) {
    if (errno == ENOENT || errno == ENOTDIR) return FS_PATH_NOT_FOUND;
    if (errno == EEXIST) return FS_PATH_EXISTS;
    if (errno == ENOTEMPTY) return FS_NOT_EMPTY;
    return MAKE_RESULT(2, 4000);
}

// Whole reads / writes: a network share (or Wine over one) may move fewer bytes per call than asked.
static ssize_t pread_full(int fd, void *buf, size_t n, int64_t off) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, (char *)buf + done, n - done, off + (int64_t)done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return done ? (ssize_t)done : -1;
        if (r == 0) break;  // end of file
        done += (size_t)r;
    }
    return (ssize_t)done;
}
static bool pwrite_full(int fd, const void *buf, size_t n, int64_t off) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = pwrite(fd, (const char *)buf + done, n - done, off + (int64_t)done);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) errno = EIO;
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcmp(name + n - e, ext);
}

// Copies src over dst in place (dst opened for writing, so a symlink is followed) and flushes it.
static bool copy_over(const char *src, const char *dst) {
    int in = open(src, O_RDONLY | O_BINARY);
    if (in < 0) return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (out < 0) { close(in); return false; }
    static char buf[1 << 20];
    bool ok = true;
    for (int64_t off = 0;;) {
        ssize_t n = pread_full(in, buf, sizeof buf, off);
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (!pwrite_full(out, buf, (size_t)n, off)) { ok = false; break; }
        off += n;
    }
    if (fsync(out)) ok = false;
    close(in);
    if (close(out)) ok = false;
    return ok;
}

static Pending *pending(HostFs *fs, const char *p) {
    for (int i = 0; i < fs->npending; i++)
        if (!strcmp(fs->pending[i].path, p)) return &fs->pending[i];
    return NULL;
}
static Pending *pending_add(HostFs *fs, const char *p) {
    Pending *q = pending(fs, p);
    if (q || fs->npending == MAX_PENDING) return q;
    q = &fs->pending[fs->npending++];
    memset(q, 0, sizeof *q);
    snprintf(q->path, sizeof q->path, "%s", p);
    return q;
}
static void tmp_path(const char *p, const char *ext, char *out, size_t n) { snprintf(out, n, "%s%s", p, ext); }

// Commit: the pending changes become the files' content.
static bool fs_commit(HostFs *fs) {
    bool ok = true;
    for (int i = 0; i < fs->npending; i++) {
        Pending *q = &fs->pending[i];
        char tmp[1200], com[1200];
        tmp_path(q->path, TMP_EXT, tmp, sizeof tmp);
        tmp_path(q->path, COMMIT_EXT, com, sizeof com);
        if (q->created) {
            unlink(com);
            if (rename(tmp, com)) { LOG("[fs] commit %s: %s\n", q->path, strerror(errno)); ok = false; continue; }
            if (!copy_over(com, q->path)) {  // (the complete data stays in .mhgu-commit: applied at the next start)
                LOG("[fs] commit %s: writing failed (%s); kept in %s\n", q->path, strerror(errno), com);
                ok = false;
                continue;
            }
            unlink(com);
        } else if (q->deleted && unlink(q->path)) {
            LOG("[fs] commit: deleting %s: %s\n", q->path, strerror(errno));
        }
    }
    fs->npending = 0;
    return ok;
}

// Unmounted without a commit (or a commit that stopped): what was never committed goes away.
static void fs_discard(HostFs *fs) {
    for (int i = 0; i < fs->npending; i++) {
        char tmp[1200];
        tmp_path(fs->pending[i].path, TMP_EXT, tmp, sizeof tmp);
        if (fs->pending[i].created) unlink(tmp);
    }
    fs->npending = 0;
}

// ------------------------------------------------------------------ save backups
// [Application] SaveBackups: after each commit of save_0, and at its first mount in a run (before this run changes it),
// its files are copied to <backups>/<date>_<time>/, <backups> being BackupLocation or <save folder>/backups. A copy is
// made in <name>.partial and renamed when complete, so a folder without the extension always holds a whole save. One
// that would be the same as the newest is not made (a start right after a save); past SaveBackups, the oldest go.
// A failure only logs: the game's save itself has already succeeded.
#define BACKUP_EXT ".partial"
#define MAX_BACKUPS 512
static int rm_rf(const char *p, bool keep_root);
static void mkdirs(const char *path);

static void backup_root(char *out, size_t n) {
    if (getenv("MHGU_BACKUP_DIR")) snprintf(out, n, "%s", getenv("MHGU_BACKUP_DIR"));
    else if (g_backup_dir) snprintf(out, n, "%s", g_backup_dir);
    else snprintf(out, n, "%s/backups", g_save_dir);
}

// A backup's folder: <YYYY-MM-DD_HH-MM-SS>, or with _<nn> after it (two in one second: save_backup).
static bool is_backup_name(const char *s) {
    static const char form[] = "dddd-dd-dd_dd-dd-dd";
    for (int i = 0; form[i]; i++)
        if (form[i] == 'd' ? (s[i] < '0' || s[i] > '9') : s[i] != form[i]) return false;
    const char *t = s + sizeof form - 1;
    if (!*t) return true;
    if (*t++ != '_' || !*t) return false;
    for (; *t; t++) if (*t < '0' || *t > '9') return false;
    return true;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// The backups' folder names, oldest first (the names sort by time). Partial ones left by a stop are deleted.
static int list_backups(const char *root, char **names, int max) {
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (has_ext(e->d_name, BACKUP_EXT)) {
            char p[2048];
            snprintf(p, sizeof p, "%s/%s", root, e->d_name);
            rm_rf(p, false);
        } else if (is_backup_name(e->d_name) && n < max)
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_names);
    return n;
}

static bool is_save_file(const char *dir, const char *name, struct stat *st) {
    if (name[0] == '.' || has_ext(name, TMP_EXT) || has_ext(name, COMMIT_EXT)) return false;
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return !stat(p, st) && S_ISREG(st->st_mode);
}

static bool same_content(const char *a, const char *b) {
    int fa = open(a, O_RDONLY | O_BINARY), fb = open(b, O_RDONLY | O_BINARY);
    bool same = fa >= 0 && fb >= 0;
    static char ba[1 << 16], bb[1 << 16];
    for (int64_t off = 0; same;) {
        ssize_t na = pread_full(fa, ba, sizeof ba, off), nb = pread_full(fb, bb, sizeof bb, off);
        if (na < 0 || na != nb || memcmp(ba, bb, (size_t)na)) same = false;
        if (na <= 0) break;
        off += na;
    }
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return same;
}

static int count_save_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct stat st;
    struct dirent *e;
    while ((e = readdir(d))) if (is_save_file(dir, e->d_name, &st)) n++;
    closedir(d);
    return n;
}

// Whether the backup folder holds exactly the save folder's files, with the same content.
static bool same_as(const char *save, const char *backup) {
    int nsave = 0, nbackup = count_save_files(backup);
    struct stat st, bt;
    struct dirent *e;
    DIR *d = opendir(save);
    if (!d) return false;
    bool same = true;
    while (same && (e = readdir(d))) {
        if (!is_save_file(save, e->d_name, &st)) continue;
        nsave++;
        char a[2048], b[2048];
        snprintf(a, sizeof a, "%s/%s", save, e->d_name);
        snprintf(b, sizeof b, "%s/%s", backup, e->d_name);
        same = !stat(b, &bt) && bt.st_size == st.st_size && same_content(a, b);
    }
    closedir(d);
    return same && nsave == nbackup;
}

static void save_backup(const char *save, const char *why) {
    int keep = config_live_save_backups();
    if (keep <= 0 || !count_save_files(save)) return;  // (off, or no save yet)
    char root[1100];
    backup_root(root, sizeof root);
    mkdirs(root);
    char *names[MAX_BACKUPS];
    int n = list_backups(root, names, MAX_BACKUPS);
    char path[1300];
    bool skip = false;
    if (n) {
        snprintf(path, sizeof path, "%s/%s", root, names[n - 1]);
        skip = same_as(save, path);
    }
    if (!skip) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char name[64];
        strftime(name, sizeof name, "%Y-%m-%d_%H-%M-%S", t);
        // (a name sorting after the newest: the oldest is the first by name. In the same second, or with the clock
        // set back, as when summer time ends, it is the newest's time with _02, _03... after it)
        if (n && strcmp(name, names[n - 1]) <= 0) {
            const char *last = names[n - 1];
            int next = last[19] == '_' ? atoi(last + 20) + 1 : 2;
            snprintf(name, sizeof name, "%.19s_%02d", last, next);
        }
        snprintf(path, sizeof path, "%s/%s", root, name);
        struct stat st;
        char part[1400];
        snprintf(part, sizeof part, "%s" BACKUP_EXT, path);
        mkdirs(part);
        bool ok = true;
        DIR *d = opendir(save);
        struct dirent *e;
        while (ok && d && (e = readdir(d))) {
            if (!is_save_file(save, e->d_name, &st)) continue;
            char src[2048], dst[2048];
            snprintf(src, sizeof src, "%s/%s", save, e->d_name);
            snprintf(dst, sizeof dst, "%s/%s", part, e->d_name);
            ok = copy_over(src, dst);
        }
        if (d) closedir(d);
        if (ok && d && !rename(part, path)) {
            LOG("[fs] save backup (%s): %s\n", why, path);
            if (n == MAX_BACKUPS) free(names[--n]);
            names[n++] = NULL;  // (the new one: counts toward the kept ones, never deleted)
        } else {
            LOG("[fs] save backup (%s) to %s failed: %s\n", why, path, strerror(errno));
            rm_rf(part, false);
        }
    }
    for (int i = 0; i < n; i++) {
        if (names[i] && i < n - keep) {
            snprintf(path, sizeof path, "%s/%s", root, names[i]);
            if (!rm_rf(path, false)) LOG("[fs] old save backup deleted: %s\n", path);
        }
        free(names[i]);
    }
}

static bool is_main_save(const char *root) {
    size_t n = strlen(root);
    return n >= 7 && !strcmp(root + n - 7, "/save_0");
}

// First mount of a save folder this run: finish an interrupted commit, drop uncommitted files.
static void fs_recover(const char *root) {
    static char done[8][1024];
    static int ndone;
    for (int i = 0; i < ndone; i++) if (!strcmp(done[i], root)) return;
    if (ndone < 8) snprintf(done[ndone++], sizeof done[0], "%s", root);
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[2048], real[2048];
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        if (has_ext(e->d_name, COMMIT_EXT)) {
            snprintf(real, sizeof real, "%.*s", (int)(strlen(p) - strlen(COMMIT_EXT)), p);
            bool ok = copy_over(p, real);
            if (ok) unlink(p);
            LOG("[fs] %s: finished an interrupted save%s\n", real, ok ? "" : " - FAILED, the data stays in .mhgu-commit");
        } else if (has_ext(e->d_name, TMP_EXT)) {
            unlink(p);
            LOG("[fs] %s: removed an uncommitted save file\n", p);
        }
    }
    closedir(d);
    if (is_main_save(root)) save_backup(root, "start");
}

static void file(Service *s, IpcReq *req, IpcResp *resp) {
    HostFile *f = s->state;
    switch (req->cmd) {
    case 0: {  // Read(option, offset, size) -> B
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *b = out_buf(req, 0);
        if ((uint64_t)size > b->size) size = (int64_t)b->size;
        ssize_t n = pread_full(f->fd, BUFP(b), (size_t)size, off);
        if (n < 0) {
            LOG("[fs] reading %s: %s\n", f->path, strerror(errno));
            resp->result = errno_result();
        }
        OUT(int64_t, 0) = n < 0 ? 0 : n;
        if (ENV("MHGU_LOG_FILES"))
            LOG("[fs] read %s +%llx %llx -> guest %08x\n", f->path, (unsigned long long)off, (unsigned long long)size,
                (uint32_t)b->addr);
        resp_size(resp, 8);
        break;
    }
    case 1: {  // Write(option, offset, size) <- A
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *a = in_buf(req, 0);
        if (!pwrite_full(f->fd, BUFP(a), (size_t)size, off)) {
            LOG("[fs] writing %s: %s\n", f->path, strerror(errno));
            resp->result = errno_result();
        }
        if (ENV("MHGU_LOG_FILES"))
            LOG("[fs] write %s +%llx %llx <- guest %08x\n", f->path, (unsigned long long)off, (unsigned long long)size,
                (uint32_t)a->addr);
        break;
    }
    case 2: fsync(f->fd); break;
    case 3: if (ftruncate(f->fd, IN(int64_t, 0))) resp->result = errno_result(); break;
    case 4: {
        struct stat sb;
        fstat(f->fd, &sb);
        OUT(int64_t, 0) = sb.st_size;
        resp_size(resp, 8);
        break;
    }
    default: LOG("[fs] IFile cmd %u (ok)\n", req->cmd);
    }
}

static void file_close(Service *s) {
    HostFile *f = s->state;
    if (f->fd >= 0) close(f->fd);
    free(f);
}

static void directory(Service *s, IpcReq *req, IpcResp *resp) {
    HostDir *d = s->state;
    switch (req->cmd) {
    case 0: {  // Read -> B buffer of 0x310-byte entries
        IpcBuf *b = out_buf(req, 0);
        uint8_t *out = BUFP(b);
        int64_t max = (int64_t)(b->size / 0x310), n = 0;
        struct dirent *e;
        while (n < max && (e = readdir(d->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (has_ext(e->d_name, TMP_EXT) || has_ext(e->d_name, COMMIT_EXT)) continue;  // (save transactions)
            char full[2048];
            struct stat es;
            snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
            bool isdir = !stat(full, &es) && S_ISDIR(es.st_mode);
            if ((isdir && !(d->mode & 1)) || (!isdir && !(d->mode & 2))) continue;
            uint8_t *ent = out + n * 0x310;
            memset(ent, 0, 0x310);
            strncpy((char *)ent, e->d_name, 0x300);
            ent[0x304] = isdir ? 0 : 1;
            if (!isdir) { int64_t sz = es.st_size; memcpy(ent + 0x308, &sz, 8); }
            n++;
        }
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // GetEntryCount
        long pos = telldir(d->d);
        rewinddir(d->d);
        int64_t n = 0;
        struct dirent *e;
        while ((e = readdir(d->d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && !has_ext(e->d_name, TMP_EXT) &&
                !has_ext(e->d_name, COMMIT_EXT))
                n++;
        seekdir(d->d, pos);
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    }
}

static int rm_rf(const char *p, bool keep_root) {
    DIR *d = opendir(p);
    if (!d) return unlink(p);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[2048];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        rm_rf(c, false);
    }
    closedir(d);
    return keep_root ? 0 : rmdir(p);
}

static void filesystem(Service *s, IpcReq *req, IpcResp *resp) {
    HostFs *fs = s->state;
    char p[2048], p2[2048];
    host_path(fs, in_path(req, 0), p, sizeof p);
    if (ENV("MHGU_LOG_FILES")) LOG("[fs] IFileSystem cmd %u %s\n", req->cmd, p);
    Pending *q = fs->journal ? pending(fs, p) : NULL;  // (save mounts: changes not committed yet)
    struct stat sb;
    char tmp[1200];
    tmp_path(p, TMP_EXT, tmp, sizeof tmp);
    switch (req->cmd) {
    case 0: {  // CreateFile(option, size)
        if (fs->journal) {
            if (q ? q->created : !stat(p, &sb)) { resp->result = FS_PATH_EXISTS; break; }
            if (!(q = pending_add(fs, p))) { LOG("[fs] %s: too many changes before a commit\n", p); resp->result = MAKE_RESULT(2, 4000); break; }
        }
        const char *at = fs->journal ? tmp : p;
        int fd = open(at, O_CREAT | (fs->journal ? O_TRUNC : O_EXCL) | O_RDWR | O_BINARY, 0644);
        if (fd < 0) {
            if (fs->journal) LOG("[fs] creating %s: %s\n", at, strerror(errno));
            resp->result = errno_result();
            break;
        }
        if (ftruncate(fd, IN(int64_t, 8))) {
            LOG("[fs] creating %s: %s\n", at, strerror(errno));
            resp->result = errno_result();
        }
        close(fd);
        if (q) q->created = true;
        break;
    }
    case 1:  // DeleteFile
        if (fs->journal) {
            bool real = !stat(p, &sb) && !S_ISDIR(sb.st_mode);
            if (!(q ? q->created : real)) { resp->result = FS_PATH_NOT_FOUND; break; }
            if (!(q = pending_add(fs, p))) { LOG("[fs] %s: too many changes before a commit\n", p); resp->result = MAKE_RESULT(2, 4000); break; }
            if (q->created) unlink(tmp);
            q->created = false;
            q->deleted = real;  // (the file itself goes at the commit)
            break;
        }
        if (unlink(p)) resp->result = errno_result();
        break;
    case 2: if (mkdir(p, 0755)) resp->result = errno_result(); break;
    case 3: if (rmdir(p)) resp->result = errno_result(); break;
    case 4: if (rm_rf(p, false)) resp->result = errno_result(); break;
    case 5: case 6:
        host_path(fs, in_path(req, 1), p2, sizeof p2);
        if (rename(p, p2)) resp->result = errno_result();
        break;
    case 7: {  // GetEntryType
        if (q && (q->created || q->deleted)) {
            if (!q->created) { resp->result = FS_PATH_NOT_FOUND; break; }
            OUT(uint32_t, 0) = 1;
            resp_size(resp, 4);
            break;
        }
        if (stat(p, &sb)) { resp->result = FS_PATH_NOT_FOUND; break; }
        OUT(uint32_t, 0) = S_ISDIR(sb.st_mode) ? 0 : 1;
        resp_size(resp, 4);
        break;
    }
    case 8: {  // OpenFile(mode)
        uint32_t mode = IN(uint32_t, 0);
        if (q && !q->created && q->deleted) { resp->result = FS_PATH_NOT_FOUND; break; }
        bool fresh = q && q->created;  // (opens the uncommitted new content)
        if (!fresh && strstr(fs->root, "/save_")) { void bonus_fill(const char *); bonus_fill(p); }  // (bonus.c)
        int fd = open(fresh ? tmp : p, ((mode & 2) ? O_RDWR : O_RDONLY) | O_BINARY);
        if (fd < 0) { resp->result = errno_result(); break; }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        strlcpy(f->path, p, sizeof f->path);
        Service *fsvc = service_new("IFile", file, f);
        fsvc->on_close = file_close;
        ipc_return_object(resp, fsvc);
        break;
    }
    case 9: {  // OpenDirectory(mode)
        DIR *d = opendir(p);
        if (!d) { resp->result = errno_result(); break; }
        HostDir *hd = calloc(1, sizeof *hd);
        hd->d = d;
        hd->mode = (int)IN(uint32_t, 0);
        snprintf(hd->path, sizeof hd->path, "%s", p);
        ipc_return_object(resp, service_new("IDirectory", directory, hd));
        break;
    }
    case 10:  // Commit
        if (fs->journal) {
            bool changed = fs->npending > 0;
            if (!fs_commit(fs)) resp->result = MAKE_RESULT(2, 4000);
            else if (changed && is_main_save(fs->root)) save_backup(fs->root, "save");
        }
        break;
    case 11: case 12: OUT(int64_t, 0) = 0x40000000; resp_size(resp, 8); break;  // free / total space
    case 13: rm_rf(p, true); break;
    case 14: memset(resp->data, 0, 0x20); resp_size(resp, 0x20); break;  // GetFileTimeStampRaw
    default: LOG("[fs] IFileSystem cmd %u (ok)\n", req->cmd);
    }
}

static void mkdirs(const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    mkdir(tmp, 0755);
}

static void filesystem_close(Service *s) {
    HostFs *fs = s->state;
    if (fs->npending) LOG("[fs] %s unmounted without a commit: %d change(s) dropped\n", fs->root, fs->npending);
    fs_discard(fs);
    free(fs);
}

static Service *open_host_fs(const char *sub, bool journal) {
    HostFs *fs = calloc(1, sizeof *fs);
    if (getenv("MHGU_SAVE_DIR")) g_save_dir = getenv("MHGU_SAVE_DIR");
    snprintf(fs->root, sizeof fs->root, "%s/%s", g_save_dir, sub);
    mkdirs(fs->root);
    fs->journal = journal;
    if (journal) fs_recover(fs->root);
    LOG("[fs] mounted host directory %s\n", fs->root);
    Service *svc = service_new("IFileSystem", filesystem, fs);
    svc->on_close = filesystem_close;
    return svc;
}

// ------------------------------------------------------------------ fsp-srv
SERVICE("fsp-srv", fsp_srv) {
    (void)s;
    switch (req->cmd) {
    case 1: break;  // SetCurrentProcess
    case 18: ipc_return_object(resp, open_host_fs("sdcard", false)); break;  // OpenSdCardFileSystem
    case 22: break;  // CreateSaveDataFileSystem
    case 51: case 53: {  // OpenSaveDataFileSystem(space, attribute)
        uint8_t type = IN(uint8_t, 0x8 + 0x18);  // attribute.type
        char sub[64];
        snprintf(sub, sizeof sub, "save_%u", type);
        ipc_return_object(resp, open_host_fs(sub, true));
        break;
    }
    case 200: ipc_return_object(resp, open_romfs_storage()); break;  // OpenDataStorageByCurrentProcess
    case 202:  // OpenDataStorageByDataId (system data archives)
        LOG("[fs] OpenDataStorageByDataId %llx (not available)\n", (unsigned long long)IN(uint64_t, 8));
        resp->result = MAKE_RESULT(2, 1001);
        break;
    case 203: ipc_return_object(resp, open_romfs_storage()); break;  // OpenPatchDataStorageByCurrentProcess
    case 1005: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetGlobalAccessLogMode
    case 1003: case 1004: case 1006: break;
    case 1011: memset(resp->data, 0, 0x10); resp_size(resp, 0x10); break;
    default: LOG("[fs] fsp-srv cmd %u (ok)\n", req->cmd);
    }
}
