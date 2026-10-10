/*
 * recomp_save_dir.c - the save root by platform, the one-time move from the
 * working directory's saves/, and the backups taken at start.
 * See recomp_save_dir.h.
 *
 * The directories are each platform's place for per-user app data. Windows
 * and macOS as UnleashedRecomp (hedge-dev/UnleashedRecomp
 * UnleashedRecomp/user/paths.cpp, BuildUserPath: FOLDERID_RoamingAppData,
 * ~/Library/Application Support); on Linux the XDG data directory, which
 * the kernel's own default (kernel_path.c) already uses, rather than its
 * ~/.config, since saves are data and not configuration.
 */
#include "recomp_save_dir.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define rs_mkdir(p) _mkdir(p)
#define rs_rmdir(p) _rmdir(p)
#define rs_unlink(p) _unlink(p)
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#define rs_mkdir(p) mkdir(p, 0755)
#define rs_rmdir(p) rmdir(p)
#define rs_unlink(p) unlink(p)
#endif

#define RS_PATH 1024

static void mkdir_p(const char *path)
{
    char tmp[RS_PATH];
    size_t i, n = strlen(path);

    if (!n || n >= sizeof tmp)
        return;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if ((tmp[i] == '/' || tmp[i] == '\\') && !(i == 2 && tmp[1] == ':')) {
            char c = tmp[i];
            tmp[i] = '\0';
            rs_mkdir(tmp);
            tmp[i] = c;
        }
    }
    rs_mkdir(tmp);
}

static int is_dir(const char *p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Each entry of `dir` but . and .., in no particular order. Stops early,
 * returning 0, when fn does. */
typedef int (*entry_fn)(const char *dir, const char *name, int dir_entry, void *ctx);

static int for_each(const char *dir, entry_fn fn, void *ctx)
{
#if defined(_WIN32)
    char pat[RS_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int ok = 1;

    snprintf(pat, sizeof pat, "%s/*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        if (!fn(dir, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx)) {
            ok = 0;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
#else
    DIR *d = opendir(dir);
    struct dirent *de;
    int ok = 1;

    if (!d)
        return 1;
    while ((de = readdir(d)) != NULL) {
        char p[RS_PATH];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (!fn(dir, de->d_name, is_dir(p), ctx)) {
            ok = 0;
            break;
        }
    }
    closedir(d);
    return ok;
#endif
}

static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out;
    char buf[65536];
    size_t n;
    int ok = 1;

    if (!in)
        return 0;
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;
    return ok;
}

typedef struct {
    const char *to;
    int skip_images;
    int ok;
} CopyCtx;

static int copy_tree(const char *from, const char *to, int skip_images);

static int copy_entry(const char *dir, const char *name, int dir_entry, void *vctx)
{
    CopyCtx *c = (CopyCtx *)vctx;
    char src[RS_PATH], dst[RS_PATH];
    size_t n = strlen(name);

    snprintf(src, sizeof src, "%s/%s", dir, name);
    snprintf(dst, sizeof dst, "%s/%s", c->to, name);
    if (dir_entry) {
        if (!copy_tree(src, dst, c->skip_images))
            c->ok = 0;
    } else if (!(c->skip_images && n > 4 && !strcmp(name + n - 4, ".img"))) {
        if (!copy_file(src, dst))
            c->ok = 0;
    }
    return 1;
}

static int copy_tree(const char *from, const char *to, int skip_images)
{
    CopyCtx c;
    c.to = to;
    c.skip_images = skip_images;
    c.ok = 1;
    mkdir_p(to);
    for_each(from, copy_entry, &c);
    return c.ok;
}

static int rm_entry(const char *dir, const char *name, int dir_entry, void *ctx);

static void rm_tree(const char *path)
{
    for_each(path, rm_entry, NULL);
    rs_rmdir(path);
}

static int rm_entry(const char *dir, const char *name, int dir_entry, void *ctx)
{
    char p[RS_PATH];
    (void)ctx;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (dir_entry)
        rm_tree(p);
    else
        rs_unlink(p);
    return 1;
}

/* ── same contents ─────────────────────────────────────────────────── */

static int same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[8192], bb[8192];
    int same = fa && fb;

    while (same) {
        size_t na = fread(ba, 1, sizeof ba, fa), nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na))
            same = 0;
        else if (!na)
            break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

static int count_entry(const char *dir, const char *name, int dir_entry, void *ctx)
{
    (void)dir; (void)name; (void)dir_entry;
    (*(int *)ctx)++;
    return 1;
}

static int same_tree(const char *a, const char *b);

typedef struct { const char *other; } SameCtx;

static int same_entry(const char *dir, const char *name, int dir_entry, void *vctx)
{
    SameCtx *c = (SameCtx *)vctx;
    char pa[RS_PATH], pb[RS_PATH];
    snprintf(pa, sizeof pa, "%s/%s", dir, name);
    snprintf(pb, sizeof pb, "%s/%s", c->other, name);
    if (dir_entry)
        return is_dir(pb) && same_tree(pa, pb);
    return !is_dir(pb) && same_file(pa, pb);
}

/* Both missing counts as the same. */
static int same_tree(const char *a, const char *b)
{
    int na = 0, nb = 0;
    SameCtx c;

    if (is_dir(a) != is_dir(b))
        return 0;
    if (!is_dir(a))
        return 1;
    for_each(a, count_entry, &na);
    for_each(b, count_entry, &nb);
    if (na != nb)
        return 0;
    c.other = b;
    return for_each(a, same_entry, &c);
}

/* ── the API ───────────────────────────────────────────────────────── */

const char *recomp_save_dir(const char *name)
{
    static char dir[RS_PATH];
    const char *ov;

    if (dir[0])
        return dir;
    ov = recomp_env(RENV_SAVE_DIR);
    if (ov && *ov) {
        snprintf(dir, sizeof dir, "%s", ov);
    } else {
#if defined(_WIN32)
        const char *base = getenv("APPDATA");
        snprintf(dir, sizeof dir, "%s/%s", base && *base ? base : ".", name);
#elif defined(__APPLE__)
        const char *home = getenv("HOME");
        snprintf(dir, sizeof dir, "%s/Library/Application Support/%s",
                 home && *home ? home : ".", name);
#else
        char lower[128];
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        size_t i;
        for (i = 0; name[i] && i + 1 < sizeof lower; i++)
            lower[i] = (char)tolower((unsigned char)name[i]);
        lower[i] = '\0';
        if (xdg && *xdg)
            snprintf(dir, sizeof dir, "%s/%s", xdg, lower);
        else
            snprintf(dir, sizeof dir, "%s/.local/share/%s", home && *home ? home : ".", lower);
#endif
    }
    {
        size_t n = strlen(dir);
        while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
            dir[--n] = '\0';
    }
    mkdir_p(dir);
    return dir;
}

int recomp_save_dir_migrate(const char *legacy, const char *dir)
{
    char from[RS_PATH], to[RS_PATH];
    FILE *note;

    snprintf(from, sizeof from, "%s/UDATA", legacy);
    snprintf(to, sizeof to, "%s/UDATA", dir);
    /* Also stops a second copy, and RECOMP_SAVE_DIR=saves (one folder). */
    if (!is_dir(from) || is_dir(to))
        return 0;
    /* Copied before: a UDATA deleted since, for a first boot, stays so. */
    snprintf(from, sizeof from, "%s/MOVED.txt", legacy);
    note = fopen(from, "r");
    if (note) {
        fclose(note);
        return 0;
    }
    if (!copy_tree(legacy, dir, 1)) {
        char part[RS_PATH];
        fprintf(stderr, "[SAVE] copying %s to %s failed part way; the old folder "
                        "is untouched and is copied again next start\n", legacy, dir);
        rm_tree(to);
        snprintf(part, sizeof part, "%s/TDATA", dir);
        rm_tree(part);
        return 0;
    }
    note = fopen(from, "w");
    if (note) {
        fprintf(note, "The saves in this folder were copied to\n  %s\n"
                      "and are read from there now. This folder is no longer "
                      "used and can be deleted.\n", dir);
        fclose(note);
    }
    return 1;
}

static int collect_entry(const char *dir, const char *name, int dir_entry, void *vctx)
{
    char (*names)[32] = (char (*)[32])((void **)vctx)[0];
    int *n = (int *)((void **)vctx)[1];
    (void)dir;
    if (dir_entry && strlen(name) < 32 && *n < 256)
        snprintf(names[(*n)++], 32, "%s", name);
    return 1;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int recomp_save_backup(const char *dir, int keep, char *out, unsigned out_size)
{
    static char names[256][32];
    static const char *const parts[] = { "UDATA", "TDATA" };
    char root[RS_PATH], path[RS_PATH], src[RS_PATH], stamp[32];
    void *ctx[2];
    int n = 0, i, k, same = 0, have = 0;

    if (out && out_size)
        out[0] = '\0';
    for (k = 0; k < 2; k++) {
        snprintf(src, sizeof src, "%s/%s", dir, parts[k]);
        have |= is_dir(src);
    }
    if (!have)
        return 0;           /* nothing saved yet */
    snprintf(root, sizeof root, "%s/backup", dir);
    mkdir_p(root);
    ctx[0] = names;
    ctx[1] = &n;
    for_each(root, collect_entry, ctx);
    qsort(names, (size_t)n, sizeof names[0], cmp_name);

    if (n) {
        same = 1;
        for (k = 0; k < 2 && same; k++) {
            snprintf(src, sizeof src, "%s/%s", dir, parts[k]);
            snprintf(path, sizeof path, "%s/%s/%s", root, names[n - 1], parts[k]);
            same = same_tree(src, path);
        }
    }
    if (!same) {
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        int ok = 1;
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", lt);
        snprintf(path, sizeof path, "%s/%s", root, stamp);
        if (is_dir(path))
            rm_tree(path);  /* two starts in one second: the later wins */
        for (k = 0; k < 2; k++) {
            char dst[RS_PATH];
            snprintf(src, sizeof src, "%s/%s", dir, parts[k]);
            snprintf(dst, sizeof dst, "%s/%s", path, parts[k]);
            if (is_dir(src) && !copy_tree(src, dst, 0))
                ok = 0;
        }
        if (!ok) {
            fprintf(stderr, "[SAVE] backup to %s failed; removed\n", path);
            rm_tree(path);
            return 0;
        }
        if (out && out_size)
            snprintf(out, out_size, "%s", path);
        /* Listed again below with the new one. */
        n = 0;
        for_each(root, collect_entry, ctx);
        qsort(names, (size_t)n, sizeof names[0], cmp_name);
    }
    for (i = 0; keep > 0 && i < n - keep; i++) {
        snprintf(path, sizeof path, "%s/%s", root, names[i]);
        rm_tree(path);
    }
    return !same;
}
