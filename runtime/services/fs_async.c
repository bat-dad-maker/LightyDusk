// Smooth loading (experimental): read-ahead of the game's RomFS image and save writes off the game thread.
// See fs_async.h. Nothing here runs unless fs_async_on() (the setting, off by default) is true.
#include "fs_async.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../rt.h"
#include "../settings.h"

#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#include <sys/stat.h>

#define RA_WINDOW (4u << 20)    // read-ahead: how far past a read is asked for
#define RA_SLOTS 16             // read-ahead requests waiting at most (more are dropped: a hint, never a queue)
#define SHADOW_MAX (64u << 20)  // a save file larger than this stays on the upstream (direct) path
#define PATH_N 1100

bool fs_async_on(void) {
    static int on = -1;
    int v = __atomic_load_n(&on, __ATOMIC_ACQUIRE);
    if (v < 0) {
        const char *e = getenv("LARECOMP_SMOOTH_LOADING");
        if (e && *e) v = atoi(e) != 0;
#ifdef __ANDROID__
        else v = g_settings.smooth_loading ? 1 : 0;
#else
        else v = 0;
#endif
        __atomic_store_n(&on, v, __ATOMIC_RELEASE);
        if (v) LOG("[fs] smooth loading (experimental): read-ahead and background save writes on\n");
    }
    return v != 0;
}

// ------------------------------------------------------------------ the worker
typedef struct Job { struct Job *next; char path[PATH_N]; uint8_t *data; size_t len; } Job;
typedef struct { int fd; uint64_t off, n; } Hint;

static pthread_mutex_t q_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;     // work arrived
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;  // a save job finished
static Job *q_head, *q_tail;                               // (q_m)
static char q_busy[PATH_N];                                // the path being written now, "" none (q_m)
static Hint ra[RA_SLOTS];                                  // (q_m) ring
static unsigned ra_r, ra_w;
static bool worker_up;

static void fsync_dir_of(const char *path) {
    char d[PATH_N];
    snprintf(d, sizeof d, "%s", path);
    char *slash = strrchr(d, '/');
    if (!slash) return;
    *slash = 0;
    int fd = open(d[0] ? d : "/", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) fsync(fd), close(fd);
}

static bool write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w, n -= (size_t)w;
    }
    return true;
}

// <path>.smooth-tmp written and synced, then renamed over path: the file is either the old one or the new one whole
static void commit_file(const Job *j) {
    char tmp[PATH_N + 16];
    snprintf(tmp, sizeof tmp, "%s.smooth-tmp", j->path);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    bool ok = fd >= 0 && write_all(fd, j->data, j->len) && fsync(fd) == 0;
    if (fd >= 0 && close(fd)) ok = false;
    if (ok && rename(tmp, j->path) == 0) { fsync_dir_of(j->path); return; }
    // (the temporary file couldn't be made or renamed - a full disk, a folder gone: written in place, as upstream)
    LOG("[fs] smooth loading: atomic save of %s failed (errno %d), writing it in place\n", j->path, errno);
    unlink(tmp);
    fd = open(j->path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { LOG("[fs] smooth loading: save %s lost: errno %d\n", j->path, errno); return; }
    if (!write_all(fd, j->data, j->len) || ftruncate(fd, (off_t)j->len)) LOG("[fs] smooth loading: save %s write errno %d\n", j->path, errno);
    fsync(fd);
    close(fd);
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&q_m);
    for (;;) {
        while (!q_head && ra_r == ra_w) pthread_cond_wait(&q_cv, &q_m);
        if (q_head) {  // (saves first: a hint can wait)
            Job *j = q_head;
            q_head = j->next;
            if (!q_head) q_tail = NULL;
            snprintf(q_busy, sizeof q_busy, "%s", j->path);
            pthread_mutex_unlock(&q_m);
            commit_file(j);
            free(j->data);
            free(j);
            pthread_mutex_lock(&q_m);
            q_busy[0] = 0;
            pthread_cond_broadcast(&done_cv);
            continue;
        }
        Hint h = ra[ra_r % RA_SLOTS];
        ra_r++;
        pthread_mutex_unlock(&q_m);
#ifdef POSIX_FADV_WILLNEED
        posix_fadvise(h.fd, (off_t)h.off, (off_t)h.n, POSIX_FADV_WILLNEED);
#endif
        pthread_mutex_lock(&q_m);
    }
    return NULL;
}

static void worker_start(void) {  // (q_m held)
    if (worker_up) return;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, worker, NULL) == 0) worker_up = true;
    pthread_attr_destroy(&a);
}

// A copy of data queued for path; one already queued (not yet being written) for the same path takes the newer
// content instead, in its place in the queue. false: no worker (the caller writes it itself)
static bool enqueue(const char *path, uint8_t *data, size_t len) {
    pthread_mutex_lock(&q_m);
    worker_start();
    if (!worker_up) { pthread_mutex_unlock(&q_m); return false; }
    for (Job *j = q_head; j; j = j->next)
        if (!strcmp(j->path, path)) {
            free(j->data);
            j->data = data, j->len = len;
            pthread_mutex_unlock(&q_m);
            return true;
        }
    Job *j = calloc(1, sizeof *j);
    if (!j) { pthread_mutex_unlock(&q_m); return false; }
    snprintf(j->path, sizeof j->path, "%s", path);
    j->data = data, j->len = len;
    if (q_tail) q_tail->next = j; else q_head = j;
    q_tail = j;
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_m);
    return true;
}

static bool path_under(const char *p, const char *root) {  // p is root or inside it
    size_t n = strlen(root);
    return !strncmp(p, root, n) && (p[n] == 0 || p[n] == '/');
}

void fs_async_wait(const char *path) {
    pthread_mutex_lock(&q_m);
    for (;;) {
        bool busy = path ? (q_busy[0] && path_under(q_busy, path)) : q_busy[0] != 0;
        for (Job *j = q_head; j && !busy; j = j->next) busy = !path || path_under(j->path, path);
        if (!busy) break;
        pthread_cond_wait(&done_cv, &q_m);
    }
    pthread_mutex_unlock(&q_m);
}

bool fs_async_pending(void) {
    if (!fs_async_on()) return false;
    pthread_mutex_lock(&q_m);
    bool p = q_head || q_busy[0];
    pthread_mutex_unlock(&q_m);
    return p;
}

// ------------------------------------------------------------------ read-ahead
void fs_async_readahead(int fd, uint64_t off, uint64_t n) {
    if (fd < 0 || !fs_async_on()) return;
    static uint64_t last_end;  // (q_m) the end of the last window asked for: reads inside it ask nothing new
    static int last_fd = -1;
    uint64_t from = off + n;
    pthread_mutex_lock(&q_m);
    if (fd == last_fd && from + RA_WINDOW / 2 <= last_end && from >= (last_end > 2 * RA_WINDOW ? last_end - 2 * RA_WINDOW : 0)) {
        pthread_mutex_unlock(&q_m);
        return;
    }
    if (ra_w - ra_r < RA_SLOTS) {
        worker_start();
        ra[ra_w % RA_SLOTS] = (Hint){fd, from, RA_WINDOW};
        ra_w++;
        last_fd = fd, last_end = from + RA_WINDOW;
        pthread_cond_signal(&q_cv);
    }
    pthread_mutex_unlock(&q_m);
}

// ------------------------------------------------------------------ save shadows
struct SaveShadow {
    SaveShadow *next;
    char path[PATH_N];
    uint8_t *buf;
    size_t len, cap;
    int refs;
    bool dirty, dead;
};
static pthread_mutex_t sh_m = PTHREAD_MUTEX_INITIALIZER;
static SaveShadow *shadows;  // (sh_m)

static SaveShadow *find_locked(const char *path) {
    for (SaveShadow *s = shadows; s; s = s->next)
        if (!s->dead && !strcmp(s->path, path)) return s;
    return NULL;
}

SaveShadow *fs_shadow_find(const char *path) {
    pthread_mutex_lock(&sh_m);
    SaveShadow *s = find_locked(path);
    if (s) s->refs++;
    pthread_mutex_unlock(&sh_m);
    return s;
}

SaveShadow *fs_shadow_open(const char *path, int fd) {
    SaveShadow *s = fs_shadow_find(path);
    if (s) return s;
    struct stat sb;
    if (fstat(fd, &sb) || (uint64_t)sb.st_size > SHADOW_MAX) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cap = sb.st_size ? (size_t)sb.st_size : 4096;
    s->buf = malloc(s->cap);
    if (!s->buf) { free(s); return NULL; }
    size_t got = 0;
    while (got < (size_t)sb.st_size) {
        ssize_t r = pread(fd, s->buf + got, (size_t)sb.st_size - got, (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got != (size_t)sb.st_size) { free(s->buf); free(s); return NULL; }  // (unreadable: the upstream path)
    s->len = got;
    snprintf(s->path, sizeof s->path, "%s", path);
    s->refs = 1;
    pthread_mutex_lock(&sh_m);
    SaveShadow *other = find_locked(path);  // (another thread opened it meanwhile: share that one)
    if (other) {
        other->refs++;
        pthread_mutex_unlock(&sh_m);
        free(s->buf);
        free(s);
        return other;
    }
    s->next = shadows;
    shadows = s;
    pthread_mutex_unlock(&sh_m);
    return s;
}

static bool reserve(SaveShadow *s, uint64_t end) {  // (sh_m held)
    if (end > SHADOW_MAX) return false;
    if (end <= s->cap) return true;
    size_t c = s->cap ? s->cap : 4096;
    while (c < end) c *= 2;
    if (c > SHADOW_MAX) c = SHADOW_MAX;
    uint8_t *b = realloc(s->buf, c);
    if (!b) return false;
    s->buf = b, s->cap = c;
    return true;
}

int64_t fs_shadow_read(SaveShadow *s, void *dst, uint64_t n, uint64_t off) {
    pthread_mutex_lock(&sh_m);
    uint64_t k = off >= s->len ? 0 : (s->len - off < n ? s->len - off : n);
    if (k) memcpy(dst, s->buf + off, (size_t)k);
    pthread_mutex_unlock(&sh_m);
    return (int64_t)k;
}

bool fs_shadow_write(SaveShadow *s, const void *src, uint64_t n, uint64_t off) {
    pthread_mutex_lock(&sh_m);
    bool ok = reserve(s, off + n);
    if (ok) {
        if (off > s->len) memset(s->buf + s->len, 0, (size_t)(off - s->len));  // (a hole reads as zeros, as in a file)
        memcpy(s->buf + off, src, (size_t)n);
        if (off + n > s->len) s->len = (size_t)(off + n);
        s->dirty = true;
    }
    pthread_mutex_unlock(&sh_m);
    return ok;
}

bool fs_shadow_set_size(SaveShadow *s, uint64_t size) {
    pthread_mutex_lock(&sh_m);
    bool ok = reserve(s, size);
    if (ok) {
        if (size > s->len) memset(s->buf + s->len, 0, (size_t)(size - s->len));
        s->len = (size_t)size;
        s->dirty = true;
    }
    pthread_mutex_unlock(&sh_m);
    return ok;
}

uint64_t fs_shadow_size(SaveShadow *s) {
    pthread_mutex_lock(&sh_m);
    uint64_t n = s->len;
    pthread_mutex_unlock(&sh_m);
    return n;
}

static void flush_locked(SaveShadow *s) {  // (sh_m held) its content copied and queued
    if (!s->dirty || s->dead) return;
    uint8_t *copy = malloc(s->len ? s->len : 1);
    if (!copy) return;  // (stays dirty: tried again at the next flush)
    memcpy(copy, s->buf, s->len);
    if (enqueue(s->path, copy, s->len)) { s->dirty = false; return; }
    // (no worker thread: written here, as a last resort)
    Job j = {0};
    snprintf(j.path, sizeof j.path, "%s", s->path);
    j.data = copy, j.len = s->len;
    commit_file(&j);
    free(copy);
    s->dirty = false;
}

void fs_shadow_flush(SaveShadow *s) {
    pthread_mutex_lock(&sh_m);
    flush_locked(s);
    pthread_mutex_unlock(&sh_m);
}

void fs_shadow_close(SaveShadow *s) {
    pthread_mutex_lock(&sh_m);
    flush_locked(s);
    if (--s->refs == 0) {
        for (SaveShadow **p = &shadows; *p; p = &(*p)->next)
            if (*p == s) { *p = s->next; break; }
        free(s->buf);
        free(s);
    }
    pthread_mutex_unlock(&sh_m);
}

void fs_shadow_commit(const char *root) {
    pthread_mutex_lock(&sh_m);
    for (SaveShadow *s = shadows; s; s = s->next)
        if (path_under(s->path, root)) flush_locked(s);
    pthread_mutex_unlock(&sh_m);
}

void fs_async_before_unlink(const char *path) {
    if (!fs_async_on()) return;
    fs_async_wait(NULL);
    pthread_mutex_lock(&sh_m);
    for (SaveShadow *s = shadows; s; s = s->next)
        if (path_under(s->path, path)) s->dead = true, s->dirty = false;
    pthread_mutex_unlock(&sh_m);
}

void fs_async_before_rename(const char *from, const char *to) {
    if (!fs_async_on()) return;
    fs_async_wait(NULL);
    size_t n = strlen(from);
    pthread_mutex_lock(&sh_m);
    for (SaveShadow *s = shadows; s; s = s->next)
        if (!s->dead && path_under(s->path, from)) {
            char moved[PATH_N];
            snprintf(moved, sizeof moved, "%s%s", to, s->path + n);
            snprintf(s->path, sizeof s->path, "%s", moved);
        }
    pthread_mutex_unlock(&sh_m);
}

void fs_async_flush(void) {
    if (!fs_async_on()) return;
    pthread_mutex_lock(&sh_m);
    for (SaveShadow *s = shadows; s; s = s->next) flush_locked(s);
    pthread_mutex_unlock(&sh_m);
    fs_async_wait(NULL);
}

#else  // (other hosts: never on)
bool fs_async_on(void) { return false; }
void fs_async_readahead(int fd, uint64_t off, uint64_t n) { (void)fd, (void)off, (void)n; }
SaveShadow *fs_shadow_open(const char *path, int fd) { (void)path, (void)fd; return NULL; }
SaveShadow *fs_shadow_find(const char *path) { (void)path; return NULL; }
int64_t fs_shadow_read(SaveShadow *s, void *dst, uint64_t n, uint64_t off) { (void)s, (void)dst, (void)n, (void)off; return -1; }
bool fs_shadow_write(SaveShadow *s, const void *src, uint64_t n, uint64_t off) { (void)s, (void)src, (void)n, (void)off; return false; }
bool fs_shadow_set_size(SaveShadow *s, uint64_t size) { (void)s, (void)size; return false; }
uint64_t fs_shadow_size(SaveShadow *s) { (void)s; return 0; }
void fs_shadow_flush(SaveShadow *s) { (void)s; }
void fs_shadow_close(SaveShadow *s) { (void)s; }
void fs_shadow_commit(const char *root) { (void)root; }
void fs_async_before_unlink(const char *p) { (void)p; }
void fs_async_before_rename(const char *a, const char *b) { (void)a, (void)b; }
void fs_async_wait(const char *path) { (void)path; }
void fs_async_flush(void) {}
bool fs_async_pending(void) { return false; }
#endif
