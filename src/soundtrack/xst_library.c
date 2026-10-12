/*
 * xst_library.c - a music folder as soundtracks, and the ST.DB image.
 * See xbox_soundtrack.h.
 */
#include "xbox_soundtrack.h"
#include "xst_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ── names ─────────────────────────────────────────────────────────── */

int xst_natural_cmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;

    while (*p && *q) {
        if (isdigit(*p) && isdigit(*q)) {
            const unsigned char *ps, *qs;
            size_t pn, qn;
            while (*p == '0') p++;
            while (*q == '0') q++;
            ps = p; qs = q;
            while (isdigit(*p)) p++;
            while (isdigit(*q)) q++;
            pn = (size_t)(p - ps); qn = (size_t)(q - qs);
            if (pn != qn)
                return pn < qn ? -1 : 1;
            if (pn) {
                int c = memcmp(ps, qs, pn);
                if (c)
                    return c < 0 ? -1 : 1;
            }
            continue;
        }
        {
            int x = tolower(*p), y = tolower(*q);
            if (x != y)
                return x < y ? -1 : 1;
        }
        p++; q++;
    }
    if (*p || *q)
        return *p ? 1 : -1;
    return strcmp(a, b);              /* a stable order for "A" and "a" */
}

/* A character as the one unit a title's menu font is likely to have: such
 * fonts stop at Latin-1 (Burnout 3's draws a box for anything above it).
 * Latin Extended-A goes to its nearest Latin-1 or plain letter, so a
 * Hungarian "ő" reads "ö" and a Czech "ř" reads "r"; typographic quotes and
 * dashes go to the plain ones; other scripts have no stand-in. */
static uint16_t fold_latin1(uint32_t c)
{
    static const char ext_a[] =
        "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
        "lLlNnNnNnnNnOoOo" "\xD6\xF6OoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu"
        "\xDC\xFCUuWwYyYZzZzZzs";

    if (c < 0x20)
        return c == '\t' ? ' ' : '?';
    if (c < 0x100)
        return (uint16_t)c;
    if (c < 0x180)
        return (uint8_t)ext_a[c - 0x100];
    switch (c) {
    case 0x2018: case 0x2019: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';
    default: return '?';
    }
}

/* A combining mark after `base` (a file name from macOS is decomposed:
 * "a" + U+0301 for "á"): the composed Latin-1 letter, or the base alone. */
static uint16_t compose_latin1(uint16_t base, uint32_t mark)
{
    static const struct { uint16_t mark; const char *from, *to; } k[] = {
        { 0x0300, "AEIOUaeiou",   "\xC0\xC8\xCC\xD2\xD9\xE0\xE8\xEC\xF2\xF9" },
        { 0x0301, "AEIOUYaeiouy", "\xC1\xC9\xCD\xD3\xDA\xDD\xE1\xE9\xED\xF3\xFA\xFD" },
        { 0x0302, "AEIOUaeiou",   "\xC2\xCA\xCE\xD4\xDB\xE2\xEA\xEE\xF4\xFB" },
        { 0x0303, "ANOano",       "\xC3\xD1\xD5\xE3\xF1\xF5" },
        { 0x0308, "AEIOUaeiouy",  "\xC4\xCB\xCF\xD6\xDC\xE4\xEB\xEF\xF6\xFC\xFF" },
        { 0x030A, "Aa",           "\xC5\xE5" },
        { 0x030B, "OUou",         "\xD6\xDC\xF6\xFC" },
        { 0x0327, "Cc",           "\xC7\xE7" },
    };
    size_t i;

    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].mark == mark && base < 0x80) {
            const char *at = base ? strchr(k[i].from, (int)base) : NULL;
            if (at)
                return (uint8_t)k[i].to[at - k[i].from];
        }
    return base;
}

void xst_name_from_utf8(uint16_t out[XST_NAME_UNITS], const char *utf8)
{
    const unsigned char *s = (const unsigned char *)(utf8 ? utf8 : "");
    int n = 0;

    while (*s && n < XST_NAME_UNITS - 1) {
        uint32_t c = *s++;
        int more = 0;
        if (c >= 0xF0)      { c &= 0x07; more = 3; }
        else if (c >= 0xE0) { c &= 0x0F; more = 2; }
        else if (c >= 0xC0) { c &= 0x1F; more = 1; }
        else if (c >= 0x80) { c = 0xFFFD; }
        while (more-- > 0) {
            if ((*s & 0xC0) != 0x80) { c = 0xFFFD; break; }
            c = (c << 6) | (*s++ & 0x3F);
        }
        if (c >= 0x0300 && c < 0x0370) {
            if (n > 0)
                out[n - 1] = compose_latin1(out[n - 1], c);
            continue;
        }
        /* One unit a character: the menus index the name by unit. */
        out[n++] = fold_latin1(c);
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    memset(out + n, 0, (size_t)(XST_NAME_UNITS - n) * sizeof out[0]);
}

int xst_is_playable_name(const char *name)
{
    static const char *const ext[] = { ".mp3", ".flac", ".ogg", ".oga", ".wav",
#if defined(__APPLE__)
        /* The system's decoder (xst_decode.c): macOS only. */
        ".m4a", ".aac", ".aif", ".aiff", ".caf",
#endif
        NULL };
    const char *dot = strrchr(name, '.');
    int i;

    if (!dot || name[0] == '.')
        return 0;
    for (i = 0; ext[i]; i++) {
        const char *e = ext[i], *d = dot;
        while (*e && *d && tolower((unsigned char)*d) == *e) { e++; d++; }
        if (!*e && !*d)
            return 1;
    }
    return 0;
}

/* ── one folder's entries ──────────────────────────────────────────── */


int xst_names_add(xst_names *l, const char *s)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        char **v = (char **)realloc(l->v, (size_t)cap * sizeof *v);
        if (!v)
            return 0;
        l->v = v; l->cap = cap;
    }
    l->v[l->n] = (char *)malloc(strlen(s) + 1);
    if (!l->v[l->n])
        return 0;
    strcpy(l->v[l->n++], s);
    return 1;
}

void xst_names_free(xst_names *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    l->v = NULL; l->n = l->cap = 0;
}

static int names_cmp(const void *a, const void *b)
{
    return xst_natural_cmp(*(char *const *)a, *(char *const *)b);
}

/* The playable files and the folders of `dir`, each sorted. 0: unreadable. */
int xst_list_dir(const char *dir, xst_names *files, xst_names *dirs)
{
#if defined(_WIN32)
    wchar_t pat[1024];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, pat, 1000);
    if (n <= 0)
        return 0;
    wcscat(pat, L"\\*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char name[780];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL))
            continue;
        if (name[0] == '.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            xst_names_add(dirs, name);
        else if (files && xst_is_playable_name(name))
            xst_names_add(files, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char path[2048];
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            xst_names_add(dirs, e->d_name);
        else if (files && S_ISREG(st.st_mode) && xst_is_playable_name(e->d_name))
            xst_names_add(files, e->d_name);
    }
    closedir(d);
#endif
    if (files && files->n)
        qsort(files->v, (size_t)files->n, sizeof files->v[0], names_cmp);
    if (dirs->n)
        qsort(dirs->v, (size_t)dirs->n, sizeof dirs->v[0], names_cmp);
    return 1;
}

static const char *leaf_of(const char *path)
{
    const char *s = path + strlen(path);
    while (s > path && (s[-1] == '/' || s[-1] == '\\'))
        s--;                               /* trailing separators */
    while (s > path && s[-1] != '/' && s[-1] != '\\')
        s--;
    return s;
}

/* ── the scan ──────────────────────────────────────────────────────── */

static int add_song(xst_library *lib, const char *path, const char *title, uint32_t ms)
{
    xst_song *s;

    if (lib->n_songs == lib->cap_songs) {
        int cap = lib->cap_songs ? lib->cap_songs * 2 : 256;
        xst_song *v = (xst_song *)realloc(lib->songs, (size_t)cap * sizeof *v);
        if (!v)
            return 0;
        lib->songs = v; lib->cap_songs = cap;
    }
    s = &lib->songs[lib->n_songs];
    memset(s, 0, sizeof *s);
    s->path = (char *)malloc(strlen(path) + 1);
    if (!s->path)
        return 0;
    strcpy(s->path, path);
    xst_name_from_utf8(s->name, title);
    s->ms = ms;
    lib->n_songs++;
    return 1;
}

static void scan_dir(xst_library *lib, const char *dir, int depth, xst_progress *pr)
{
    xst_names files = { 0 }, dirs = { 0 };
    char path[2048];
    int i;

    if ((pr && pr->cancel) || !xst_list_dir(dir, &files, &dirs))
        return;
    if (pr)
        pr->folders++;

    if (files.n && lib->n_st == XST_MAX_SOUNDTRACKS) {
        lib->dropped_folders++;
    } else if (files.n) {
        xst_soundtrack *st = &lib->st[lib->n_st];
        char leaf[512];

        memset(st, 0, sizeof *st);
        st->id = (uint32_t)lib->n_st + 1;
        st->first = lib->n_songs;
        for (i = 0; i < files.n; i++) {
            char title[256], stem[512];
            uint32_t ms = 0;
            char *dot;

            if (st->count == XST_MAX_SONGS) {
                lib->dropped_songs += files.n - i;
                break;
            }
            if (pr) {
                if (pr->cancel)
                    break;
                pr->files++;
            }
            snprintf(path, sizeof path, "%s/%s", dir, files.v[i]);
            title[0] = 0;
            if (!xst_probe(path, &ms, title, sizeof title)) {
                lib->bad_files++;
                continue;
            }
            if (!title[0]) {
                snprintf(stem, sizeof stem, "%s", files.v[i]);
                dot = strrchr(stem, '.');
                if (dot)
                    *dot = 0;
                snprintf(title, sizeof title, "%s", stem);
            }
            if (!add_song(lib, path, title, ms))
                break;
            lib->songs[lib->n_songs - 1].id = (st->id << 16) | (uint32_t)st->count;
            st->count++;
            if (pr)
                pr->songs++;
            st->ms += ms;
        }
        if (st->count) {
            const char *l = leaf_of(dir);
            size_t n = strlen(l);
            snprintf(leaf, sizeof leaf, "%s", l);
            while (n < sizeof leaf && n > 0 && (leaf[n - 1] == '/' || leaf[n - 1] == '\\'))
                leaf[--n] = 0;
            xst_name_from_utf8(st->name, leaf[0] ? leaf : "Music");
            lib->n_st++;
        }
    }

    if (depth < XST_MAX_DEPTH)
        for (i = 0; i < dirs.n; i++) {
            snprintf(path, sizeof path, "%s/%s", dir, dirs.v[i]);
            scan_dir(lib, path, depth + 1, pr);
        }
    xst_names_free(&files);
    xst_names_free(&dirs);
}

xst_library *xst_scan(const char *dir)
{
    return xst_scan_progress(dir, NULL);
}

xst_library *xst_scan_progress(const char *dir, xst_progress *pr)
{
    xst_library *lib;
    xst_names files = { 0 }, dirs = { 0 };
    char root[2048];
    size_t n;

    if (!dir || !*dir)
        return NULL;
    snprintf(root, sizeof root, "%s", dir);
    n = strlen(root);
    while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\'))
        root[--n] = 0;
    if (!xst_list_dir(root, &files, &dirs))
        return NULL;
    xst_names_free(&files);
    xst_names_free(&dirs);

    lib = (xst_library *)calloc(1, sizeof *lib);
    if (!lib)
        return NULL;
    scan_dir(lib, root, 0, pr);
    if (pr && pr->cancel) {
        xst_free(lib);
        return NULL;
    }
    return lib;
}

void xst_free(xst_library *lib)
{
    int i;
    if (!lib)
        return;
    for (i = 0; i < lib->n_songs; i++)
        free(lib->songs[i].path);
    free(lib->songs);
    free(lib);
}

const xst_song *xst_song_by_id(const xst_library *lib, uint32_t id)
{
    uint32_t st = id >> 16, idx = id & 0xFFFF;

    if (!lib || st < 1 || st > (uint32_t)lib->n_st)
        return NULL;
    if (idx >= (uint32_t)lib->st[st - 1].count)
        return NULL;
    return &lib->songs[lib->st[st - 1].first + (int)idx];
}

/* ── ST.DB ─────────────────────────────────────────────────────────── */

/*
 * 512-byte blocks, little-endian, as the XDK library reads them:
 *
 *   block 0            header: magic 1, soundtrack count, next soundtrack
 *                      id, the ids of the 100 soundtrack blocks, next song id
 *   blocks 1..100      soundtrack i: magic 0x00021371, id, song count,
 *                      84 song-group ids, total ms, name[32]
 *   blocks 101..       song group g: magic 0x00031073, soundtrack id, group
 *                      id, pad, 6 song ids, 6 lengths in ms, 6 name[32]
 *
 * A song's file is MUSIC\<id >> 16, %04x>\<id, %08x>.WMA, so the high half of
 * a song id is its soundtrack. Soundtrack ids start at 1: the library keeps
 * the last soundtrack it read in a zero-initialised cache keyed by id, and
 * an id of 0 would match the empty cache.
 */

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_name(uint8_t *p, const uint16_t *name)
{
    int i;
    for (i = 0; i < XST_NAME_UNITS; i++) {
        p[i * 2] = (uint8_t)name[i];
        p[i * 2 + 1] = (uint8_t)(name[i] >> 8);
    }
}

static int groups_of(int songs)
{
    return (songs + XST_SONGS_PER_GROUP - 1) / XST_SONGS_PER_GROUP;
}

size_t xst_db_size(const xst_library *lib)
{
    int i, groups = 0;
    for (i = 0; i < lib->n_st; i++)
        groups += groups_of(lib->st[i].count);
    return (size_t)XST_BLOCK * (size_t)(1 + XST_MAX_SOUNDTRACKS + groups);
}

size_t xst_db_build(const xst_library *lib, uint8_t *out, size_t cap)
{
    size_t size = xst_db_size(lib);
    uint32_t gid = 0;
    int i, g, k;

    if (cap < size)
        return 0;
    memset(out, 0, size);

    le32(out + 0, 1);
    le32(out + 4, (uint32_t)lib->n_st);
    le32(out + 8, (uint32_t)lib->n_st + 1);
    for (i = 0; i < lib->n_st; i++)
        le32(out + 12 + i * 4, lib->st[i].id);
    le32(out + 12 + XST_MAX_SOUNDTRACKS * 4, 0x00010000u * ((uint32_t)lib->n_st + 1));

    for (i = 0; i < lib->n_st; i++) {
        const xst_soundtrack *st = &lib->st[i];
        uint8_t *b = out + (size_t)XST_BLOCK * (size_t)(1 + i);
        int ng = groups_of(st->count);

        le32(b + 0, 0x00021371u);
        le32(b + 4, st->id);
        le32(b + 8, (uint32_t)st->count);
        le32(b + 12 + XST_MAX_GROUPS * 4, st->ms);
        put_name(b + 16 + XST_MAX_GROUPS * 4, st->name);
        for (g = 0; g < ng; g++, gid++) {
            uint8_t *gb = out + (size_t)XST_BLOCK * (size_t)(1 + XST_MAX_SOUNDTRACKS + gid);
            le32(b + 12 + g * 4, gid);
            le32(gb + 0, 0x00031073u);
            le32(gb + 4, st->id);
            le32(gb + 8, gid);
            for (k = 0; k < XST_SONGS_PER_GROUP; k++) {
                int idx = g * XST_SONGS_PER_GROUP + k;
                const xst_song *s;
                if (idx >= st->count)
                    break;
                s = &lib->songs[st->first + idx];
                le32(gb + 16 + k * 4, s->id);
                le32(gb + 40 + k * 4, s->ms);
                put_name(gb + 64 + k * XST_NAME_UNITS * 2, s->name);
            }
        }
    }
    return size;
}
