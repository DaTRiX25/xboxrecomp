/*
 * xst_browse.c - a folder browser's model for choosing the music folder.
 * See xbox_soundtrack.h.
 *
 * The places are those of the setup window's disc browser of the Burnout 3
 * port (home, Downloads, /run/media/<user>/<label> and /run/media/<label>,
 * /media, /mnt), with Music first and the other hosts' mounts.
 */
#include "xbox_soundtrack.h"
#include "xst_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION xsb_mutex;
#define LOCK()    EnterCriticalSection(&s_mu)
#define UNLOCK()  LeaveCriticalSection(&s_mu)
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
typedef pthread_mutex_t xsb_mutex;
#define LOCK()    pthread_mutex_lock(&s_mu)
#define UNLOCK()  pthread_mutex_unlock(&s_mu)
#endif

static xsb_mutex s_mu;
static int       s_mu_ready;

static xsb_row  *s_rows;                  /* XSB_MAX_ROWS */
static int       s_n;
static char      s_dir[1024];             /* "" on the places */
static char      s_root[1024];            /* the place that was entered */
static char      s_current[1024];
static int       s_open;
/* Bumped at every new list: the counting thread drops what it was doing. */
static volatile int s_gen;
static volatile int s_stop;
#if defined(_WIN32)
static HANDLE    s_thread;
#else
static pthread_t s_thread;
#endif
static int       s_thread_on;

/* What this opening of the browser is for (xsb_open_with). */
static char      s_use_label[80] = "Use this folder";
static char      s_off_label[80] = "Turn custom music off";
static char      s_top_label[80], s_top_path[1024];
static int     (*s_mark)(const char *dir);

static const char *s_test_home;
static const char *const *s_test_media;
static int       s_test_roots;

/* ── small things ──────────────────────────────────────────────────── */

static int is_dir(const char *path)
{
#if defined(_WIN32)
    wchar_t w[1024];
    DWORD a;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) <= 0)
        return 0;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

static const char *leaf(const char *path)
{
    const char *s = path + strlen(path);
    while (s > path && is_sep(s[-1]))
        s--;
    while (s > path && !is_sep(s[-1]))
        s--;
    return s;
}

/* The folder above `path`, 0 when there is none ("/", "C:\"). */
static int parent_of(const char *path, char *out, size_t cap)
{
    size_t n = strlen(path);

    while (n > 1 && is_sep(path[n - 1]))
        n--;
    while (n > 0 && !is_sep(path[n - 1]))
        n--;
    if (n == 0)
        return 0;
    /* Keep a root's separator: "/" and "C:\". */
    if (n > 1 && !(n == 3 && path[1] == ':'))
        n--;
    if (n >= cap || n == strlen(path))
        return 0;
    memcpy(out, path, n);
    out[n] = 0;
    return 1;
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    snprintf(out, cap, "%s%s%s", dir, n && is_sep(dir[n - 1]) ? "" : "/", name);
}

static int same_path(const char *a, const char *b)
{
    size_t x = strlen(a), y = strlen(b);
    while (x > 1 && is_sep(a[x - 1])) x--;
    while (y > 1 && is_sep(b[y - 1])) y--;
    return x == y && !strncmp(a, b, x);
}

/* ── counting ──────────────────────────────────────────────────────── */

/* Playable files in and below `dir`; *budget entries may be looked at. */
static int count_tree(const char *dir, int depth, int *budget, int gen)
{
    int n = 0;
#if defined(_WIN32)
    wchar_t pat[1024];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, pat, 1000) <= 0)
        return 0;
    wcscat(pat, L"\\*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char name[780], path[1024];
        if (*budget <= 0 || gen != s_gen || s_stop)
            break;
        (*budget)--;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL))
            continue;
        if (name[0] == '.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < XST_MAX_DEPTH) {
                join(path, sizeof path, dir, name);
                n += count_tree(path, depth + 1, budget, gen);
            }
        } else if (xst_is_playable_name(name)) {
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char path[1024];
        struct stat st;
        int dirent_is_dir;
        if (*budget <= 0 || gen != s_gen || s_stop)
            break;
        (*budget)--;
        if (e->d_name[0] == '.')
            continue;
#if defined(DT_DIR)
        if (e->d_type == DT_REG) {
            n += xst_is_playable_name(e->d_name);
            continue;
        }
        dirent_is_dir = e->d_type == DT_DIR;
        if (e->d_type != DT_DIR && e->d_type != DT_LNK && e->d_type != DT_UNKNOWN)
            continue;
#else
        dirent_is_dir = 0;
#endif
        join(path, sizeof path, dir, e->d_name);
        if (!dirent_is_dir) {
            if (stat(path, &st) != 0)
                continue;
            if (S_ISREG(st.st_mode)) {
                n += xst_is_playable_name(e->d_name);
                continue;
            }
            if (!S_ISDIR(st.st_mode))
                continue;
        }
        if (depth < XST_MAX_DEPTH)
            n += count_tree(path, depth + 1, budget, gen);
    }
    closedir(d);
#endif
    return n;
}

static void nap(void)
{
#if defined(_WIN32)
    Sleep(15);
#else
    usleep(15000);
#endif
}

#if defined(_WIN32)
static DWORD WINAPI count_thread(LPVOID arg)
#else
static void *count_thread(void *arg)
#endif
{
    (void)arg;
    while (!s_stop) {
        char path[1024];
        int i, gen, found = -1, budget = XSB_COUNT_BUDGET, n;

        LOCK();
        gen = s_gen;
        for (i = 0; i < s_n; i++)
            if (s_rows[i].songs < 0 && s_rows[i].path[0]
                && (s_rows[i].kind == XSB_USE || s_rows[i].kind == XSB_DIR
                    || s_rows[i].kind == XSB_PLACE)) {
                found = i;
                snprintf(path, sizeof path, "%s", s_rows[i].path);
                break;
            }
        UNLOCK();
        if (found < 0) {
            nap();
            continue;
        }
        /* Music: the playable files in and below it. Another purpose: its
         * own yes or no for the folder itself. */
        n = s_mark ? (s_mark(path) ? 1 : 0) : count_tree(path, 0, &budget, gen);
        LOCK();
        if (gen == s_gen && found < s_n) {
            s_rows[found].songs = n;
            s_rows[found].more = budget <= 0;
        }
        UNLOCK();
    }
    return 0;
}

/* ── the lists ─────────────────────────────────────────────────────── */

static void add_row(xsb_kind kind, const char *label, const char *path)
{
    xsb_row *r;
    int i;

    if (s_n == XSB_MAX_ROWS)
        return;
    if (kind == XSB_PLACE) {
        if (!is_dir(path))
            return;
        for (i = 0; i < s_n; i++)
            if (s_rows[i].kind == XSB_PLACE && same_path(s_rows[i].path, path))
                return;
    }
    r = &s_rows[s_n++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    /* The label is for a screen: composed. The path stays as the disk has it. */
    xst_utf8_compose(r->label, sizeof r->label, label);
    snprintf(r->path, sizeof r->path, "%s", path ? path : "");
    r->songs = (kind == XSB_OFF || kind == XSB_UP) ? 0 : -1;
}

/* Every folder of `root` as a drive; with `user`, <root>/<user>/<label>
 * (how udisks mounts a card) rather than the user's folder itself. */
static void add_mounts(const char *root, const char *user)
{
    xst_names dirs = { 0 }, sub = { 0 };
    char path[1024], label[200];
    int i, k;

    if (!xst_list_dir(root, NULL, &dirs))
        return;
    for (i = 0; i < dirs.n; i++) {
        join(path, sizeof path, root, dirs.v[i]);
        if (user && *user && !strcmp(dirs.v[i], user)) {
            if (xst_list_dir(path, NULL, &sub)) {
                for (k = 0; k < sub.n; k++) {
                    char p2[1024];
                    join(p2, sizeof p2, path, sub.v[k]);
                    snprintf(label, sizeof label, "Drive: %s", sub.v[k]);
                    add_row(XSB_PLACE, label, p2);
                }
                xst_names_free(&sub);
            }
            continue;
        }
        snprintf(label, sizeof label, "Drive: %s", dirs.v[i]);
        add_row(XSB_PLACE, label, path);
    }
    xst_names_free(&dirs);
}

static void list_places(void)
{
    char path[1024], label[1100];
    const char *home;

    s_n = 0;
    s_dir[0] = s_root[0] = 0;
    if (s_current[0]) {
        if (s_off_label[0])
            add_row(XSB_OFF, s_off_label, "");
        snprintf(label, sizeof label, "Current folder: %s", leaf(s_current));
        add_row(XSB_PLACE, label, s_current);
    }
    if (s_top_path[0])
        add_row(XSB_PLACE, s_top_label, s_top_path);
    if (s_test_roots) {
        home = s_test_home;
    } else {
#if defined(_WIN32)
        /* Wide, then UTF-8: the narrow environment is in the ANSI code page. */
        static char home_utf8[1024];
        const wchar_t *w = _wgetenv(L"USERPROFILE");
        home = w && WideCharToMultiByte(CP_UTF8, 0, w, -1, home_utf8, sizeof home_utf8, NULL, NULL) > 0
             ? home_utf8 : NULL;
#elif defined(__ANDROID__)
        home = NULL;
#else
        home = getenv("HOME");
#endif
    }
    if (home && *home) {
        join(path, sizeof path, home, "Music");
        add_row(XSB_PLACE, "Music", path);
        add_row(XSB_PLACE, "Home", home);
        join(path, sizeof path, home, "Downloads");
        add_row(XSB_PLACE, "Downloads", path);
    }
    if (s_test_roots) {
        int i;
        for (i = 0; s_test_media && s_test_media[i]; i++)
            add_mounts(s_test_media[i], home && *home ? leaf(home) : NULL);
        return;
    }
#if defined(_WIN32)
    {
        DWORD mask = GetLogicalDrives();
        int d;
        for (d = 0; d < 26; d++)
            if (mask & (1u << d)) {
                snprintf(path, sizeof path, "%c:\\", 'A' + d);
                snprintf(label, sizeof label, "Drive %c:", 'A' + d);
                add_row(XSB_PLACE, label, path);
            }
    }
#elif defined(__APPLE__)
    add_mounts("/Volumes", NULL);
#elif defined(__ANDROID__)
    /* The app's own files folder first: the one place it can read without
     * a permission (it has none for the shared Music folder yet), and one a
     * PC reaches over USB: <files>/Music. */
    {
        const char *own = getenv("RECOMP_ANDROID_DIR");
        if (own && *own) {
            join(path, sizeof path, own, "Music");
            mkdir(path, 0775);
            add_row(XSB_PLACE, "This app's Music folder", path);
        }
    }
    /* Shared storage, readable as files with the audio permission; a card
     * is /storage/<id>. */
    add_row(XSB_PLACE, "Music", "/storage/emulated/0/Music");
    add_row(XSB_PLACE, "Downloads", "/storage/emulated/0/Download");
    add_row(XSB_PLACE, "Internal storage", "/storage/emulated/0");
    {
        xst_names dirs = { 0 };
        int i;
        if (xst_list_dir("/storage", NULL, &dirs)) {
            for (i = 0; i < dirs.n; i++) {
                if (!strcmp(dirs.v[i], "emulated") || !strcmp(dirs.v[i], "self"))
                    continue;
                join(path, sizeof path, "/storage", dirs.v[i]);
                snprintf(label, sizeof label, "Card: %s", dirs.v[i]);
                add_row(XSB_PLACE, label, path);
            }
            xst_names_free(&dirs);
        }
    }
#else
    {
        const char *user = home && *home ? leaf(home) : NULL;
        add_mounts("/run/media", user);
        add_mounts("/media", user);
        add_mounts("/mnt", NULL);
    }
#endif
#if !defined(_WIN32) && !defined(__ANDROID__)
    add_row(XSB_PLACE, "Computer: /", "/");
#endif
}

/* The rows of `dir`; 0 (and nothing changed) when it cannot be read. */
static int list_folder(const char *dir)
{
    xst_names dirs = { 0 };
    char path[1024];
    int i;

    if (!xst_list_dir(dir, NULL, &dirs))
        return 0;
    s_n = 0;
    snprintf(path, sizeof path, "%s", dir);
    snprintf(s_dir, sizeof s_dir, "%s", path);
    add_row(XSB_USE, s_use_label, s_dir);
    add_row(XSB_UP, "..", "");
    for (i = 0; i < dirs.n; i++) {
        join(path, sizeof path, s_dir, dirs.v[i]);
        add_row(XSB_DIR, dirs.v[i], path);
    }
    xst_names_free(&dirs);
    return 1;
}

/* ── the interface ─────────────────────────────────────────────────── */

static void xsb_open_places(const char *current);

void xsb_set_roots(const char *home, const char *const *media_roots)
{
    s_test_home = home;
    s_test_media = media_roots;
    s_test_roots = home || media_roots;
}

void xsb_open_with(const xsb_options *o)
{
    xsb_close();
    snprintf(s_use_label, sizeof s_use_label, "%s", o && o->use_label ? o->use_label : "Use this folder");
    snprintf(s_off_label, sizeof s_off_label, "%s", o && o->off_label ? o->off_label : "");
    snprintf(s_top_label, sizeof s_top_label, "%s", o && o->top_label ? o->top_label : "");
    snprintf(s_top_path, sizeof s_top_path, "%s", o && o->top_path ? o->top_path : "");
    s_mark = o ? o->mark : NULL;
    xsb_open_places(o ? o->current : NULL);
}

void xsb_open(const char *current)
{
    xsb_options o;
    memset(&o, 0, sizeof o);
    o.current = current;
    o.off_label = "Turn custom music off";
    xsb_open_with(&o);
}

static void xsb_open_places(const char *current)
{
    if (!s_mu_ready) {
#if defined(_WIN32)
        InitializeCriticalSection(&s_mu);
#else
        pthread_mutex_init(&s_mu, NULL);
#endif
        s_mu_ready = 1;
    }
    if (!s_rows)
        s_rows = (xsb_row *)calloc(XSB_MAX_ROWS, sizeof *s_rows);
    if (!s_rows)
        return;
    LOCK();
    snprintf(s_current, sizeof s_current, "%s", current ? current : "");
    s_gen++;
    list_places();
    s_open = 1;
    UNLOCK();
    if (!s_thread_on) {
        s_stop = 0;
#if defined(_WIN32)
        s_thread = CreateThread(NULL, 0, count_thread, NULL, 0, NULL);
        s_thread_on = s_thread != NULL;
#else
        s_thread_on = pthread_create(&s_thread, NULL, count_thread, NULL) == 0;
#endif
    }
}

void xsb_close(void)
{
    if (!s_open)
        return;
    s_stop = 1;
    if (s_thread_on) {
#if defined(_WIN32)
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
#else
        pthread_join(s_thread, NULL);
#endif
        s_thread_on = 0;
    }
    LOCK();
    s_open = 0;
    s_n = 0;
    s_gen++;
    UNLOCK();
}

int xsb_is_open(void) { return s_open; }

int xsb_rows(void)
{
    return s_open ? s_n : 0;
}

int xsb_row_at(int i, xsb_row *out)
{
    int ok = 0;
    if (!s_open)
        return 0;
    LOCK();
    if (i >= 0 && i < s_n) {
        *out = s_rows[i];
        ok = 1;
    }
    UNLOCK();
    return ok;
}

void xsb_where(char *out, size_t cap)
{
    if (!cap)
        return;
    if (!s_open) {
        out[0] = 0;
        return;
    }
    LOCK();
    snprintf(out, cap, "%s", s_dir);
    UNLOCK();
}

/* The row whose path is `path`, else the first folder row, else 0. */
static int row_of(const char *path)
{
    int i;
    for (i = 0; i < s_n; i++)
        if (s_rows[i].path[0] && s_rows[i].kind != XSB_USE && same_path(s_rows[i].path, path))
            return i;
    return 0;
}

xsb_result xsb_back(int *select)
{
    char from[1024], up[1024];
    xsb_result res = XSB_MOVED;

    if (select)
        *select = 0;
    if (!s_open)
        return XSB_CLOSED;
    LOCK();
    if (!s_dir[0]) {
        UNLOCK();
        return XSB_CLOSED;
    }
    snprintf(from, sizeof from, "%s", s_dir);
    s_gen++;
    if (same_path(s_dir, s_root) || !parent_of(s_dir, up, sizeof up) || !list_folder(up)) {
        /* Out of the place that was entered: the places again. */
        char root[1024];
        snprintf(root, sizeof root, "%s", s_root);
        list_places();
        if (select)
            *select = row_of(root);
    } else if (select) {
        *select = row_of(from);
    }
    UNLOCK();
    return res;
}

xsb_result xsb_activate(int i, char *chosen, size_t cap, int *select)
{
    xsb_row r;
    xsb_result res = XSB_NOTHING;

    if (select)
        *select = 0;
    if (!xsb_row_at(i, &r))
        return XSB_NOTHING;
    switch (r.kind) {
    case XSB_OFF:
        return XSB_TURNED_OFF;
    case XSB_USE:
        if (chosen && cap)
            snprintf(chosen, cap, "%s", r.path);
        return XSB_CHOSEN;
    case XSB_UP:
        return xsb_back(select);
    case XSB_PLACE:
    case XSB_DIR:
        LOCK();
        s_gen++;
        if (list_folder(r.path)) {
            if (r.kind == XSB_PLACE)
                snprintf(s_root, sizeof s_root, "%s", r.path);
            res = XSB_MOVED;
            /* On the first folder when there is one, else on "use". */
            if (select)
                *select = s_n > 2 ? 2 : 0;
        }
        UNLOCK();
        return res;
    }
    return res;
}
