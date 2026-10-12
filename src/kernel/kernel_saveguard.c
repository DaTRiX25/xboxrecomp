/*
 * kernel_saveguard.c - a title's save files are replaced, not overwritten.
 * See kernel_saveguard.h.
 */
#include "kernel.h"
#include "kernel_saveguard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define LOGP "[SAVE] "

/* ── host paths: bytes off Windows, wide there ─────────────────────── */

#if defined(_WIN32)
#define HC(s)          L##s
#define hlen           wcslen
#define hcpy(d, n, s)  _snwprintf((d), (n), L"%ls", (s))
#define hcmp           _wcsicmp
#define hncmp          _wcsnicmp
#define HSEP(c)        ((c) == L'\\' || (c) == L'/')
#else
#define HC(s)          s
#define hlen           strlen
#define hcpy(d, n, s)  snprintf((d), (n), "%s", (s))
#define hcmp           strcmp
#define hncmp          strncmp
#define HSEP(c)        ((c) == '/')
#endif

static const xbox_host_char k_suffix[] = HC(".~new");
#define SUFFIX_LEN 5

/* ── state ─────────────────────────────────────────────────────────── */

#define ROOTS    4
#define ENTRIES  16
#define HANDLES  4

typedef enum { E_FREE, E_OPEN, E_QUEUED, E_COMMITTING } entry_state;

typedef struct {
    entry_state    state;
    xbox_host_char real[MAX_PATH];
    xbox_host_char shadow[MAX_PATH];
    HANDLE         h[HANDLES];
    int            n_handles;
    int            dirty;
    int            deleted;           /* the title deleted it by handle */
    unsigned       writes;            /* the title's calls, for the log */
    unsigned long long bytes;
#if defined(_WIN32)
    HANDLE         flush;             /* a duplicate, to flush and close */
#else
    int            flush;
#endif
} guard_entry;

static xbox_host_char s_root[ROOTS][MAX_PATH];
static size_t         s_root_len[ROOTS];
static int            s_n_roots;
static guard_entry    s_entry[ENTRIES];
static void         (*s_hook)(void);
static volatile int   s_fault_at;

#if defined(_WIN32)
static CRITICAL_SECTION   s_mu;
static CONDITION_VARIABLE s_cv;
#define LOCK()        EnterCriticalSection(&s_mu)
#define UNLOCK()      LeaveCriticalSection(&s_mu)
#define SIGNAL()      WakeAllConditionVariable(&s_cv)
#define WAIT(ms)      SleepConditionVariableCS(&s_cv, &s_mu, (ms))
#else
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
#define LOCK()        pthread_mutex_lock(&s_mu)
#define UNLOCK()      pthread_mutex_unlock(&s_mu)
#define SIGNAL()      pthread_cond_broadcast(&s_cv)
static void WAIT(unsigned ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&s_cv, &s_mu, &ts);
}
#endif
static int s_ready, s_worker;

/* The process ends here when a test asked for it (kernel_saveguard.h). */
static void fault(int step)
{
    if (s_fault_at == step) {
        fflush(NULL);
        _exit(90 + step);
    }
}

void xbox_SaveGuardFaultAt(int step)
{
    s_fault_at = step;
}

static double now_ms(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static const xbox_host_char *leaf(const xbox_host_char *p)
{
    const xbox_host_char *l = p;
    for (; *p; p++)
        if (HSEP(*p))
            l = p + 1;
    return l;
}

#if defined(_WIN32)
#define LEAF_FMT "%ls"
#else
#define LEAF_FMT "%s"
#endif

/* ── files ─────────────────────────────────────────────────────────── */

static int h_exists(const xbox_host_char *p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static void h_unlink(const xbox_host_char *p)
{
#if defined(_WIN32)
    SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(p);
#else
    unlink(p);
#endif
}

static FILE *h_fopen(const xbox_host_char *p, int write)
{
#if defined(_WIN32)
    return _wfopen(p, write ? L"wb" : L"rb");
#else
    return fopen(p, write ? "wb" : "rb");
#endif
}

/* A whole copy; 0 and no file at `to` when it fails. */
static int h_copy(const xbox_host_char *from, const xbox_host_char *to)
{
    FILE *in = h_fopen(from, 0), *out;
    char buf[65536];
    size_t n;
    int ok = 1;

    if (!in)
        return 0;
    out = h_fopen(to, 1);
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (ferror(in))
        ok = 0;
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;
    if (!ok)
        h_unlink(to);
    return ok;
}

/* 1 when both are there with the same bytes. */
static int h_same(const xbox_host_char *a, const xbox_host_char *b)
{
    FILE *fa = h_fopen(a, 0), *fb = h_fopen(b, 0);
    char ba[32768], bb[32768];
    int same = fa && fb;

    while (same) {
        size_t na = fread(ba, 1, sizeof ba, fa), nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na))
            same = 0;
        if (na < sizeof ba)
            break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

static int h_replace(const xbox_host_char *from, const xbox_host_char *to)
{
#if defined(_WIN32)
    int tries;
    for (tries = 0; tries < 25; tries++) {
        if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return 1;
        Sleep(20);                    /* a reader has the old one open */
    }
    return 0;
#else
    char dir[MAX_PATH];
    char *slash;
    int fd;

    if (rename(from, to) != 0)
        return 0;
    /* The name's change is in the folder: that goes to the disk too. */
    snprintf(dir, sizeof dir, "%s", to);
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        fd = open(dir, O_RDONLY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    return 1;
#endif
}

/* ── which paths ───────────────────────────────────────────────────── */

static int guarded(const xbox_host_char *p)
{
    int i;
    for (i = 0; i < s_n_roots; i++)
        if (s_root_len[i] && !hncmp(p, s_root[i], s_root_len[i]) && HSEP(p[s_root_len[i]]))
            return 1;
    return 0;
}

int xbox_saveguard_hidden_name(const xbox_host_char *name)
{
    size_t n = hlen(name);
    return n > SUFFIX_LEN && !hcmp(name + n - SUFFIX_LEN, k_suffix);
}

static guard_entry *entry_of_path(const xbox_host_char *real)
{
    int i;
    for (i = 0; i < ENTRIES; i++)
        if (s_entry[i].state != E_FREE && !hcmp(s_entry[i].real, real))
            return &s_entry[i];
    return NULL;
}

static guard_entry *entry_of_handle(HANDLE h, int *slot)
{
    int i, k;
    for (i = 0; i < ENTRIES; i++)
        if (s_entry[i].state == E_OPEN)
            for (k = 0; k < s_entry[i].n_handles; k++)
                if (s_entry[i].h[k] == h) {
                    if (slot) *slot = k;
                    return &s_entry[i];
                }
    return NULL;
}

/* ── the commit, on a thread of its own ────────────────────────────── */

/* Flush what the title wrote, then put it in the real file's place: the
 * rename is the one moment the save changes, and it changes whole. */
static void commit(guard_entry *e)
{
    double t0 = now_ms(), t_flush;
    int changed;

    fault(XBOX_SAVE_FAULT_BEFORE_FLUSH);
#if defined(_WIN32)
    if (e->flush && e->flush != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(e->flush);
        CloseHandle(e->flush);
    }
#else
    if (e->flush >= 0) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
        /* fsync on macOS stops at the drive's cache; this goes through it. */
        if (fcntl(e->flush, F_FULLFSYNC) != 0)
#endif
            fsync(e->flush);
        close(e->flush);
    }
#endif
    t_flush = now_ms() - t0;
    fault(XBOX_SAVE_FAULT_BEFORE_REPLACE);
    if (!h_exists(e->shadow)) {
        /* The title removed it through its handle (delete on close), or
         * renamed it away. */
        if (e->deleted) {
            h_unlink(e->real);
            fprintf(stderr, LOGP LEAF_FMT ": deleted by the title\n", leaf(e->real));
        }
        return;
    }
    changed = !h_exists(e->real) || !h_same(e->shadow, e->real);
    if (!changed) {
        /* The same bytes again (a title rewrites its meta file at every
         * save): nothing to replace, nothing to keep a copy of. */
        h_unlink(e->shadow);
        fprintf(stderr, LOGP LEAF_FMT ": written with the same content (%u writes, %llu bytes), left as it is\n",
                leaf(e->real), e->writes, e->bytes);
        return;
    }
    /* The port's moment to keep the save as it still is (the history). */
    if (s_hook)
        s_hook();
    if (!h_replace(e->shadow, e->real)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "save: the new file could not replace the old one; "
                 "the old one stays");
        fprintf(stderr, LOGP LEAF_FMT ": the new file could not be put in place; the previous one stays\n",
                leaf(e->real));
        h_unlink(e->shadow);
        return;
    }
    fprintf(stderr, LOGP LEAF_FMT ": replaced (%u writes, %llu bytes; flush %.1f ms, all %.1f ms)\n",
            leaf(e->real), e->writes, e->bytes, t_flush, now_ms() - t0);
    fault(XBOX_SAVE_FAULT_AFTER_REPLACE);
}

#if defined(_WIN32)
static unsigned __stdcall worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
    (void)arg;
    LOCK();
    for (;;) {
        guard_entry *e = NULL;
        int i;
        for (i = 0; i < ENTRIES; i++)
            if (s_entry[i].state == E_QUEUED) { e = &s_entry[i]; break; }
        if (!e) {
            WAIT(1000);
            continue;
        }
        e->state = E_COMMITTING;
        UNLOCK();
        commit(e);
        LOCK();
        e->state = E_FREE;
        SIGNAL();
    }
    return 0;
}

static void start_worker(void)
{
    if (s_worker)
        return;
    s_worker = 1;
#if defined(_WIN32)
    {
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t); else s_worker = 0;
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t); else s_worker = 0;
    }
#endif
}

/* ── the file layer's hooks ────────────────────────────────────────── */

/* Until nothing of `real` is waiting to be put in place (a read right after
 * a save must see the save). Called with the lock held. */
static void wait_committed(const xbox_host_char *real)
{
    int tries = 0;
    for (;;) {
        guard_entry *e = entry_of_path(real);
        if (!e || e->state == E_OPEN || ++tries > 50)
            return;
        WAIT(100);
    }
}

void *xbox_saveguard_open(xbox_host_char *host_path, unsigned n, int write_access, int directory)
{
    guard_entry *e;
    int i;

    if (!s_ready || directory || !guarded(host_path) || xbox_saveguard_hidden_name(host_path))
        return NULL;
    LOCK();
    wait_committed(host_path);
    if (!write_access) {
        UNLOCK();
        return NULL;
    }
    e = entry_of_path(host_path);
    if (e && e->state == E_OPEN && e->n_handles < HANDLES) {
        /* A second handle on a file being written: the same new file. */
        e->n_handles++;
        e->h[e->n_handles - 1] = NULL;
        hcpy(host_path, n, e->shadow);
        UNLOCK();
        return e;
    }
    if (e) {                          /* no room to share it: in place, as before */
        UNLOCK();
        return NULL;
    }
    for (i = 0; i < ENTRIES && s_entry[i].state != E_FREE; i++)
        ;
    if (i == ENTRIES || hlen(host_path) + SUFFIX_LEN + 1 > MAX_PATH || hlen(host_path) + SUFFIX_LEN + 1 > n) {
        UNLOCK();
        return NULL;
    }
    e = &s_entry[i];
    memset(e, 0, sizeof *e);
    hcpy(e->real, MAX_PATH, host_path);
    hcpy(e->shadow, MAX_PATH, host_path);
    memcpy(e->shadow + hlen(e->shadow), k_suffix, sizeof k_suffix);
    /* The new file starts as the old one, so an open that keeps what is
     * there, and a write of a part, see the bytes they expect. */
    h_unlink(e->shadow);
    if (h_exists(e->real) && !h_copy(e->real, e->shadow)) {
        UNLOCK();
        return NULL;                  /* no room for a second copy: in place */
    }
    e->state = E_OPEN;
    e->n_handles = 1;
    e->h[0] = NULL;
    hcpy(host_path, n, e->shadow);
    UNLOCK();
    fault(XBOX_SAVE_FAULT_AFTER_OPEN);
    return e;
}

void xbox_saveguard_opened(void *token, HANDLE h, int ok)
{
    guard_entry *e = (guard_entry *)token;
    int k;

    if (!e)
        return;
    LOCK();
    for (k = 0; k < e->n_handles && e->h[k]; k++)
        ;
    if (ok && k < e->n_handles) {
        e->h[k] = h;
    } else {
        /* It did not open (no such file, a name collision): as if never asked. */
        if (k < e->n_handles)
            e->h[k] = e->h[e->n_handles - 1];
        e->n_handles--;
        if (e->n_handles <= 0) {
            h_unlink(e->shadow);
            e->state = E_FREE;
            SIGNAL();
        }
    }
    UNLOCK();
}

void xbox_saveguard_wrote(HANDLE h, unsigned bytes)
{
    guard_entry *e;
    int i, any = 0;

    if (!s_ready)
        return;
    for (i = 0; i < ENTRIES; i++)       /* unlocked glance: usually nothing is open */
        any |= s_entry[i].state == E_OPEN;
    if (!any)
        return;
    LOCK();
    e = entry_of_handle(h, NULL);
    if (e) {
        e->dirty = 1;
        e->writes++;
        e->bytes += bytes;
    }
    UNLOCK();
    if (e)
        fault(XBOX_SAVE_FAULT_AFTER_WRITE);
}

void xbox_saveguard_deleted(HANDLE h)
{
    guard_entry *e;

    if (!s_ready)
        return;
    LOCK();
    e = entry_of_handle(h, NULL);
    if (e)
        e->deleted = 1;
    UNLOCK();
}

void xbox_saveguard_settle(const xbox_host_char *host_path)
{
    int i, tries = 0, busy;

    if (!s_ready)
        return;
    for (i = 0, busy = 0; i < ENTRIES; i++)   /* unlocked glance, as above */
        busy |= s_entry[i].state == E_QUEUED || s_entry[i].state == E_COMMITTING;
    if (!busy || !host_path)
        return;
    LOCK();
    for (;;) {
        for (i = 0, busy = 0; i < ENTRIES; i++)
            busy |= s_entry[i].state == E_QUEUED || s_entry[i].state == E_COMMITTING;
        if (!busy || ++tries > 50)
            break;
        WAIT(100);
    }
    UNLOCK();
}

void xbox_saveguard_close(HANDLE h)
{
    guard_entry *e;
    int slot = 0;

    if (!s_ready)
        return;
    LOCK();
    e = entry_of_handle(h, &slot);
    if (!e) {
        UNLOCK();
        return;
    }
    e->h[slot] = e->h[e->n_handles - 1];
    e->n_handles--;
    if (e->n_handles > 0) {
        UNLOCK();
        return;
    }
    /* The last handle: a duplicate stays with the commit, to flush. The
     * caller closes the title's handle as it always did. */
#if defined(_WIN32)
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &e->flush, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        e->flush = NULL;
#else
    e->flush = dup(w32_handle_fd(h));
#endif
    e->state = E_QUEUED;
    start_worker();
    if (!s_worker) {                  /* no thread: here and now */
        e->state = E_COMMITTING;
        UNLOCK();
        commit(e);
        LOCK();
        e->state = E_FREE;
    }
    SIGNAL();
    UNLOCK();
}

/* ── the port's side ───────────────────────────────────────────────── */

int xbox_SaveGuardBusy(void)
{
    int i, n = 0;
    if (!s_ready)
        return 0;
    LOCK();
    for (i = 0; i < ENTRIES; i++)
        if (s_entry[i].state == E_QUEUED || s_entry[i].state == E_COMMITTING
            || (s_entry[i].state == E_OPEN && s_entry[i].dirty))
            n++;
    UNLOCK();
    return n;
}

int xbox_SaveGuardWait(unsigned ms)
{
    unsigned waited = 0;
    while (xbox_SaveGuardBusy() && waited < ms) {
        LOCK();
        WAIT(20);
        UNLOCK();
        waited += 20;
    }
    return !xbox_SaveGuardBusy();
}

void xbox_SaveGuardSetHook(void (*before_replace)(void))
{
    s_hook = before_replace;
}

/* A new file that never took its place (the process ended first) is not a
 * save: the old file beside it is. */
static void sweep(const xbox_host_char *dir, int depth)
{
#if defined(_WIN32)
    WCHAR pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (_snwprintf(pat, MAX_PATH, L"%ls\\*", dir) < 0)
        return;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (_snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) < 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 6 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                sweep(path, depth + 1);
        } else if (xbox_saveguard_hidden_name(fd.cFileName)) {
            fprintf(stderr, LOGP "an unfinished save write was found and removed; the save before it stands\n");
            h_unlink(path);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[MAX_PATH];
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path
            || lstat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 6)
                sweep(path, depth + 1);
        } else if (S_ISREG(st.st_mode) && xbox_saveguard_hidden_name(e->d_name)) {
            fprintf(stderr, LOGP "an unfinished save write was found and removed; the save before it stands\n");
            h_unlink(path);
        }
    }
    closedir(d);
#endif
}

void xbox_SaveGuardEnable(const char *title_id)
{
    static const char *const part[] = { "UDATA", "TDATA" };
    char guest[160];
    int i;

    if (!title_id || !*title_id)
        return;
    if (!s_ready) {
#if defined(_WIN32)
        InitializeCriticalSection(&s_mu);
        InitializeConditionVariable(&s_cv);
#endif
    }
    for (i = 0; i < 2; i++) {
        xbox_host_char *r = s_root[s_n_roots < ROOTS ? s_n_roots : ROOTS - 1];
        size_t n;
        snprintf(guest, sizeof guest, "\\Device\\Harddisk0\\Partition1\\%s\\%s", part[i], title_id);
        if (!xbox_translate_path(guest, r, MAX_PATH))
            continue;
        int k, again = 0;
        n = hlen(r);
        while (n > 1 && HSEP(r[n - 1]))
            r[--n] = 0;
        for (k = 0; k < s_n_roots; k++)       /* enabled again: the sweep only */
            again |= s_root_len[k] == n && !hcmp(s_root[k], r);
        sweep(r, 0);
        if (!again && s_n_roots + 1 < ROOTS) {
            s_root_len[s_n_roots] = n;
            s_n_roots++;
        }
    }
    s_ready = s_n_roots > 0;
}
