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
#include "../forge.h"
#include "../ipc.h"
#include "../kernel.h"
#include "../mods.h"
#include "../romfs.h"

#define FS_PATH_NOT_FOUND MAKE_RESULT(2, 1)
#define FS_PATH_EXISTS MAKE_RESULT(2, 2)
#define FS_NOT_EMPTY MAKE_RESULT(2, 8)

const char *g_save_dir = "save";
const char *g_backup_dir;  // ([Save Backups] BackupLocation, ui.c; MHGU_BACKUP_DIR overrides)

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
typedef struct HostFile {
    int fd;
    bool writes;
    char path[1100];
    struct HostFile *wnext;  // (g_wfiles: open for writing)
    time_t opened;
    uint64_t thread;         // the guest thread that opened it
    char callers[160];       // the game's calls that opened it (kernel_game_callers)
    char plugins[200];       // and Forge plugins' (forge_stack_plugins)
} HostFile;

// Writing now (Pause/Suspend waits for it: runtime/pause.h): files open for writing, save mounts with changes not
// committed yet (a save deletes, creates and writes its files, then commits), a commit with its backup
static int g_writing;
static void writing(int d) { __atomic_add_fetch(&g_writing, d, __ATOMIC_ACQ_REL); }
bool fs_writing(void) { return __atomic_load_n(&g_writing, __ATOMIC_ACQUIRE) > 0; }

// The files open for writing now, for the abort log (fs_log_open_writes, runtime/hle/abort.c): nn::fs aborts a save's
// commit while one is still open (2002-6457, WriteModeFileNotClosed, checked in the game's own SDK before the commit
// reaches here), and its own message names no file. Each one keeps who opened it and when.
static pthread_mutex_t g_wfiles_lock = PTHREAD_MUTEX_INITIALIZER;
static HostFile *g_wfiles;

static void wfile_add(HostFile *f) {
    Cpu *c = kernel_current_cpu();
    f->opened = time(NULL);
    KThread *t = kernel_current_thread();
    f->thread = t ? t->id : 0;
    if (c) {
        kernel_game_callers(c->r[13], f->callers, sizeof f->callers);
        forge_stack_plugins(c->r[13], f->plugins, sizeof f->plugins);
    }
    pthread_mutex_lock(&g_wfiles_lock);
    f->wnext = g_wfiles;
    g_wfiles = f;
    pthread_mutex_unlock(&g_wfiles_lock);
}

static void wfile_remove(HostFile *f) {
    pthread_mutex_lock(&g_wfiles_lock);
    for (HostFile **p = &g_wfiles; *p; p = &(*p)->wnext)
        if (*p == f) { *p = f->wnext; break; }
    pthread_mutex_unlock(&g_wfiles_lock);
}

int fs_log_open_writes(const char *tag) {
    int n = 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_wfiles_lock);  // (an abort: the lock is never held long, and never across a guest call)
    for (HostFile *f = g_wfiles; f; f = f->wnext, n++)
        LOG("%s: %s, opened %llds ago by guest thread %llu, the game's calls:%s%s%s\n", tag, f->path,
            (long long)(now - f->opened), (unsigned long long)f->thread, f->callers[0] ? f->callers : " (none found)",
            f->plugins[0] ? ", Forge plugins' calls:" : "", f->plugins);
    pthread_mutex_unlock(&g_wfiles_lock);
    return n;
}
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
    if (!fs->npending) writing(1);
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
    if (fs->npending) writing(-1);
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
    if (fs->npending) writing(-1);
    fs->npending = 0;
}

// ------------------------------------------------------------------ save backups
// [Save Backups] Backups: after each commit of save_0, its save files (system, system_backup) are copied to
// <backups>/<YYYY-MM-DD_HH-MM-SS>/, <backups> being BackupLocation or <save folder>/backups. A copy is made in
// <name>.partial and renamed when complete, so a backup's folder always holds a whole save. The game saves in two
// commits a second or so apart (system, then system_backup): the first one's backup is half a save, and the second
// one's replaces it. Past BackupsKept, the oldest go. Only a folder named like a backup that holds nothing but save
// files is ever counted or deleted, and it is deleted file by file, never recursively: other folders and files in
// BackupLocation (which can be any folder) are not touched. A failure only logs: the game's save has already succeeded.
#define BACKUP_EXT ".partial"
#define MAX_BACKUPS 512
#define SAVE_STEPS_SECONDS 5  // (a commit this soon after the last backup was made: the same save's next step)
static void mkdirs(const char *path);
static const char *const save_files[] = {"system", "system_backup"};
#define NSAVE_FILES (int)(sizeof save_files / sizeof *save_files)

static void backup_root(char *out, size_t n) {
    if (getenv("MHGU_BACKUP_DIR")) snprintf(out, n, "%s", getenv("MHGU_BACKUP_DIR"));
    else if (g_backup_dir) snprintf(out, n, "%s", g_backup_dir);
    else snprintf(out, n, "%s/backups", g_save_dir);
}

// A backup's folder name: YYYY-MM-DD_HH-MM-SS (the game can't save twice in one second), with BACKUP_EXT while made.
static bool is_backup_name(const char *s, bool partial) {
    static const char form[] = "dddd-dd-dd_dd-dd-dd";
    for (int i = 0; form[i]; i++)
        if (form[i] == 'd' ? (s[i] < '0' || s[i] > '9') : s[i] != form[i]) return false;
    return !strcmp(s + sizeof form - 1, partial ? BACKUP_EXT : "");
}

static bool is_save_file_name(const char *name) {
    for (int i = 0; i < NSAVE_FILES; i++) if (!strcmp(name, save_files[i])) return true;
    return false;
}

// Whether the folder holds nothing but save files (regular files named as one), at least min of them. The folder and
// its files must be themselves, not links: a link named like a backup can lead to a real save (a symlink, a junction).
static bool only_save_files(const char *dir, int min) {
    if (!plat_plain_entry(dir, true)) return false;
    DIR *d = opendir(dir);
    if (!d) return false;
    int n = 0;
    bool only = true;
    struct dirent *e;
    while (only && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        only = is_save_file_name(e->d_name) && plat_plain_entry(p, false);
        n++;
    }
    closedir(d);
    return only && n >= min;
}

// Deletes a backup's folder: only if it holds nothing but save files, and only those files and the folder itself.
static bool delete_backup(const char *dir, int min) {
    if (!only_save_files(dir, min)) {
        LOG("[fs] %s is not only a save backup (other files, a link, or not readable): left as it is\n", dir);
        return false;
    }
    for (int i = 0; i < NSAVE_FILES; i++) {
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, save_files[i]);
        if (unlink(p) && errno != ENOENT) { LOG("[fs] deleting %s: %s\n", p, strerror(errno)); return false; }
    }
    if (rmdir(dir)) { LOG("[fs] deleting %s: %s\n", dir, strerror(errno)); return false; }
    return true;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// The backups' folder names, oldest first (the names sort by time). Partial ones a stop left are deleted.
static int list_backups(const char *root, char **names, int max) {
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        bool partial = is_backup_name(e->d_name, true);
        if (!partial && !is_backup_name(e->d_name, false)) continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        if (partial) delete_backup(p, 0);
        else if (n < max && only_save_files(p, 1)) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_names);
    return n;
}

static void save_backup(const char *save) {
    int keep = config_live_save_backups();
    char path[1300];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", save, save_files[0]);
    if (keep <= 0 || stat(path, &st)) return;  // (off, or no save yet)
    char root[1100];
    backup_root(root, sizeof root);
    mkdirs(root);
    char *names[MAX_BACKUPS + 1];
    int n = list_backups(root, names, MAX_BACKUPS);

    time_t now = time(NULL);
    char name[64];
    strftime(name, sizeof name, "%Y-%m-%d_%H-%M-%S", localtime(&now));
    snprintf(path, sizeof path, "%s/%s", root, name);
    char part[1400];
    snprintf(part, sizeof part, "%s" BACKUP_EXT, path);
    mkdirs(part);
    bool ok = true;
    for (int i = 0; ok && i < NSAVE_FILES; i++) {
        char src[2048], dst[2048];
        snprintf(src, sizeof src, "%s/%s", save, save_files[i]);
        snprintf(dst, sizeof dst, "%s/%s", part, save_files[i]);
        if (stat(src, &st)) continue;
        if (!(ok = copy_over(src, dst))) LOG("[fs] save backup: copying %s: %s\n", src, strerror(errno));
    }
    // (the same save's first step: this run's last backup, made a moment ago, and still the newest)
    static char step[64];
    static time_t step_at;
    bool replace = ok && step[0] && n && !strcmp(names[n - 1], step) && time(NULL) - step_at <= SAVE_STEPS_SECONDS;
    char old[1300] = "";
    if (replace) snprintf(old, sizeof old, "%s/%s", root, names[n - 1]);
    if (replace && !strcmp(name, names[n - 1]) && !delete_backup(old, 1)) ok = false;  // (in its second: its name)
    if (ok && rename(part, path)) {
        LOG("[fs] save backup: %s: %s\n", path, strerror(errno));
        ok = false;
    }
    if (!ok) {
        delete_backup(part, 0);
        step[0] = 0;
    } else {
        LOG("[fs] save backup: %s\n", path);
        if (replace) {
            if (strcmp(name, names[n - 1])) delete_backup(old, 1);
            LOG("[fs] (it replaces %s, the same save's first step)\n", names[n - 1]);
            free(names[--n]);
        }
        names[n++] = NULL;  // (the new one: counts toward the kept ones, never deleted)
        snprintf(step, sizeof step, "%s", replace ? "" : name);  // (a save is two steps: a whole one starts the next)
        step_at = time(NULL);  // (after the copy: on a slow drive, it took a while)
    }
    for (int i = 0; i < n; i++) {
        if (names[i] && i < n - keep) {
            snprintf(path, sizeof path, "%s/%s", root, names[i]);
            if (delete_backup(path, 1)) LOG("[fs] old save backup deleted: %s\n", path);
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
    if (f->writes) writing(-1), wfile_remove(f);
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
        if ((f->writes = (mode & 6) != 0)) writing(1), wfile_add(f);  // (Write, Append)
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
            writing(1);
            if (!fs_commit(fs)) resp->result = MAKE_RESULT(2, 4000);
            else if (changed && is_main_save(fs->root)) save_backup(fs->root);
            writing(-1);
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
