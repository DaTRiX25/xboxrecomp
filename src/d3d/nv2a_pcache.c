/*
 * nv2a_pcache.c - the on-disk pipeline-key list; see nv2a_pcache.h.
 */
#include "nv2a_pcache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define pc_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#define pc_mkdir(p) mkdir((p), 0755)
#endif

#define MAGIC "XRPCACHE"           /* 8 bytes */
#define FORMAT 1u
#define VERSION_MAX 256u

struct rec {
    uint32_t type, len;
    uint64_t sum;
    const unsigned char *data;      /* into pc->buf */
};

struct nv2a_pcache {
    FILE *f;                        /* open for append */
    unsigned char *buf;             /* the file as loaded */
    struct rec *recs;
    size_t nrecs;
    /* Sums of every record in the file, loaded and put: open addressing,
     * 0 is empty (a sum of 0 is stored as 1). */
    uint64_t *set;
    size_t set_cap, set_n;
};

uint64_t nv2a_fnv64(const void *p, size_t n, uint64_t h)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) {
        h ^= *b++;
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t rec_sum(uint32_t type, const void *data, uint32_t len)
{
    uint64_t h = nv2a_fnv64(&type, 4, NV2A_FNV64_INIT);
    h = nv2a_fnv64(&len, 4, h);
    h = nv2a_fnv64(data, len, h);
    return h ? h : 1;
}

/* 1 if added, 0 if present, -1 out of memory. */
static int set_add(nv2a_pcache *pc, uint64_t h)
{
    size_t i;
    if ((pc->set_n + 1) * 2 > pc->set_cap) {
        size_t cap = pc->set_cap ? pc->set_cap * 2 : 256, j;
        uint64_t *s = (uint64_t *)calloc(cap, sizeof *s);
        if (!s)
            return -1;
        for (j = 0; j < pc->set_cap; j++) {
            uint64_t v = pc->set[j];
            if (!v)
                continue;
            for (i = (size_t)(v % cap); s[i]; i = (i + 1) % cap)
                ;
            s[i] = v;
        }
        free(pc->set);
        pc->set = s;
        pc->set_cap = cap;
    }
    for (i = (size_t)(h % pc->set_cap); pc->set[i]; i = (i + 1) % pc->set_cap)
        if (pc->set[i] == h)
            return 0;
    pc->set[i] = h;
    pc->set_n++;
    return 1;
}

static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* Header: MAGIC, u32 FORMAT, u32 n, version[n] (no NUL). */
static size_t header(unsigned char *out, const char *version)
{
    size_t n = strlen(version);
    if (n > VERSION_MAX)
        n = VERSION_MAX;
    memcpy(out, MAGIC, 8);
    put32(out + 8, FORMAT);
    put32(out + 12, (uint32_t)n);
    memcpy(out + 16, version, n);
    return 16 + n;
}

static unsigned char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b = NULL;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        b = (unsigned char *)malloc((size_t)n + 1);
        if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
            free(b);
            b = NULL;
        }
        *size = (size_t)n;
    }
    fclose(f);
    return b;
}

/* Writes `len` bytes as the whole file (through a temporary and a rename). */
static int rewrite(const char *path, const unsigned char *b, size_t len)
{
    char tmp[1024];
    FILE *f;
    int ok;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return 0;
    if (!(f = fopen(tmp, "wb")))
        return 0;
    ok = fwrite(b, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
#if defined(_WIN32)
    remove(path);
#endif
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

/* The header length of file image `b` when it reads as `version`, else 0.
 * A version of ours followed by "; toolkit <rev>" is ours too: the key lists
 * up to BO-93 named the toolkit commit, so every rebuild dropped them; the
 * keys under that suffix are the same. */
static size_t header_len(const unsigned char *b, size_t size, const char *version)
{
    static const char old[] = "; toolkit ";
    size_t vl = strlen(version), n;
    if (vl > VERSION_MAX)
        vl = VERSION_MAX;
    if (size < 16 || memcmp(b, MAGIC, 8) != 0 || get32(b + 8) != FORMAT)
        return 0;
    n = get32(b + 12);
    if (n > VERSION_MAX || size - 16 < n || n < vl || memcmp(b + 16, version, vl) != 0)
        return 0;
    if (n > vl && (n - vl < sizeof old - 1 || memcmp(b + 16 + vl, old, sizeof old - 1) != 0))
        return 0;
    return 16 + n;
}

nv2a_pcache *nv2a_pcache_open(const char *path, const char *version,
                              char *msg, size_t msg_len)
{
    unsigned char hdr[16 + VERSION_MAX];
    size_t hlen = header(hdr, version), size = 0, off, cap = 0;
    nv2a_pcache *pc = (nv2a_pcache *)calloc(1, sizeof *pc);
    unsigned char *b;
    const char *what = "new";
    char cut[96] = "";

    if (!pc)
        return NULL;
    b = slurp(path, &size);
    if (b && !(hlen = header_len(b, size, version))) {
        /* Another format, version or no header at all: start afresh. */
        what = size >= 16 && !memcmp(b, MAGIC, 8) ? "another version, started afresh"
                                                   : "not a cache file, started afresh";
        free(b);
        b = NULL;
        remove(path);
        hlen = header(hdr, version);
    }
    if (!b) {
        if (!rewrite(path, hdr, hlen)) {
            if (msg && msg_len)
                snprintf(msg, msg_len, "cannot create %s (%s)", path, strerror(errno));
            free(pc);
            return NULL;
        }
    } else {
        for (off = hlen; off < size; ) {
            uint32_t type, len;
            uint64_t sum;
            if (size - off < 16)
                break;
            type = get32(b + off);
            len = get32(b + off + 4);
            sum = get64(b + off + 8);
            if (len > NV2A_PCACHE_MAX_LEN || size - off - 16 < len
                    || rec_sum(type, b + off + 16, len) != sum)
                break;
            if (pc->nrecs == cap) {
                struct rec *r;
                cap = cap ? cap * 2 : 64;
                if (!(r = (struct rec *)realloc(pc->recs, cap * sizeof *r)))
                    break;
                pc->recs = r;
            }
            pc->recs[pc->nrecs].type = type;
            pc->recs[pc->nrecs].len = len;
            pc->recs[pc->nrecs].sum = sum;
            pc->recs[pc->nrecs].data = b + off + 16;
            if (set_add(pc, sum) > 0)
                pc->nrecs++;
            off += 16 + (size_t)len;
        }
        if (off < size) {
            /* A torn or damaged record: keep what came before it. */
            snprintf(cut, sizeof cut, ", %zu bytes cut at offset %zu", size - off, off);
            rewrite(path, b, off);
        }
        what = "loaded";
        pc->buf = b;
    }
    pc->f = fopen(path, "ab");
    /* A buffer larger than any record, so a record leaves in one write. */
    if (pc->f)
        setvbuf(pc->f, NULL, _IOFBF, 16 + NV2A_PCACHE_MAX_LEN);
    if (!pc->f) {
        if (msg && msg_len)
            snprintf(msg, msg_len, "cannot append to %s (%s)", path, strerror(errno));
        nv2a_pcache_close(pc);
        return NULL;
    }
    if (msg && msg_len)
        snprintf(msg, msg_len, "%s: %zu records%s", what, pc->nrecs, cut);
    return pc;
}

size_t nv2a_pcache_count(const nv2a_pcache *pc)
{
    return pc ? pc->nrecs : 0;
}

int nv2a_pcache_get(const nv2a_pcache *pc, size_t i, uint32_t *type,
                    const void **data, uint32_t *len)
{
    if (!pc || i >= pc->nrecs)
        return 0;
    *type = pc->recs[i].type;
    *data = pc->recs[i].data;
    *len = pc->recs[i].len;
    return 1;
}

int nv2a_pcache_put(nv2a_pcache *pc, uint32_t type, const void *data, uint32_t len)
{
    unsigned char stack[16 + 4096], *r = stack;
    uint64_t sum;
    int added, ok;

    if (!pc || !pc->f || len > NV2A_PCACHE_MAX_LEN)
        return -1;
    sum = rec_sum(type, data, len);
    if ((added = set_add(pc, sum)) <= 0)
        return added;
    if (16 + (size_t)len > sizeof stack && !(r = (unsigned char *)malloc(16 + (size_t)len)))
        return -1;
    put32(r, type);
    put32(r + 4, len);
    put64(r + 8, sum);
    memcpy(r + 16, data, len);
    /* One write of the whole record (O_APPEND under "ab"), so two processes
     * appending at once interleave whole records. */
    ok = fwrite(r, 1, 16 + (size_t)len, pc->f) == 16 + (size_t)len && fflush(pc->f) == 0;
    if (r != stack)
        free(r);
    return ok ? 1 : -1;
}

void nv2a_pcache_close(nv2a_pcache *pc)
{
    if (!pc)
        return;
    if (pc->f)
        fclose(pc->f);
    free(pc->buf);
    free(pc->recs);
    free(pc->set);
    free(pc);
}

/* mkdir -p */
static int make_dirs(char *p)
{
    char *s;
    struct stat st;
    for (s = p + 1; *s; s++) {
        if (*s != '/' && *s != '\\')
            continue;
        *s = 0;
        if (stat(p, &st) != 0 && pc_mkdir(p) != 0 && errno != EEXIST) {
            *s = '/';
            return 0;
        }
        *s = '/';
    }
    return (stat(p, &st) == 0 && (st.st_mode & S_IFDIR)) || pc_mkdir(p) == 0
        || errno == EEXIST;
}

#if defined(__linux__) && !defined(__ANDROID__)
extern char *program_invocation_short_name;
#endif

const char *nv2a_pcache_app(void)
{
#if defined(__APPLE__)
    return getprogname();
#elif defined(__linux__) && !defined(__ANDROID__)
    return program_invocation_short_name;
#else
    return "xboxrecomp";
#endif
}

int nv2a_pcache_dir(const char *dir, const char *app, char *out, size_t n)
{
    const char *home = getenv("HOME");
    int w;

    if (dir && (!*dir || !strcmp(dir, "none") || !strcmp(dir, "off") || !strcmp(dir, "0")))
        return 0;
    if (!app || !*app)
        app = "xboxrecomp";
    if (dir) {
        if (dir[0] == '~' && (dir[1] == '/' || dir[1] == '\\' || !dir[1]) && home)
            w = snprintf(out, n, "%s%s", home, dir + 1);
        else
            w = snprintf(out, n, "%s", dir);
    } else {
#if defined(__APPLE__)
        if (!home)
            return 0;
        w = snprintf(out, n, "%s/Library/Caches/%s", home, app);
#elif defined(_WIN32)
        const char *la = getenv("LOCALAPPDATA");
        if (!la)
            return 0;
        w = snprintf(out, n, "%s\\%s\\cache", la, app);
#else
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg)
            w = snprintf(out, n, "%s/%s", xdg, app);
        else if (home)
            w = snprintf(out, n, "%s/.cache/%s", home, app);
        else
            return 0;
#endif
    }
    if (w <= 0 || (size_t)w >= n)
        return 0;
    return make_dirs(out);
}

/* ---- the running backend's numbers ------------------------------------------- */

static nv2a_pcache_stat s_stat;
static void (*s_rescan)(void);

void nv2a_pcache_stat_get(nv2a_pcache_stat *out)
{
    *out = s_stat;
}

void nv2a_pcache_stat_open(const char *dir, const uint32_t listed[3])
{
    snprintf(s_stat.dir, sizeof s_stat.dir, "%s", dir ? dir : "");
    memcpy(s_stat.listed, listed, sizeof s_stat.listed);
    s_stat.active = dir && *dir;
}

void nv2a_pcache_stat_warming(int on)
{
    s_stat.warming = on;
}

void nv2a_pcache_stat_waiting(int on)
{
    s_stat.waiting = on;
}

void nv2a_pcache_stat_built(int prewarm, int kind, uint64_t ns)
{
    if (kind < 0 || kind > 2)
        return;
    if (prewarm) {
        s_stat.warmed[kind]++;
    } else {
        uint64_t us = ns / 1000u;
        s_stat.ack[kind]++;
        s_stat.ack_ns += ns;
        if (us > s_stat.ack_max_us)
            s_stat.ack_max_us = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)us;
    }
}

static void (*s_seed)(uint32_t, const void *, uint32_t);

void nv2a_pcache_set_seed(void (*fn)(uint32_t type, const void *data, uint32_t len))
{
    s_seed = fn;
}

int nv2a_pcache_seed(uint32_t type, const void *data, uint32_t len)
{
    if (!s_seed || !s_stat.active)
        return 0;
    s_seed(type, data, len);
    return 1;
}

void nv2a_pcache_set_rescan(void (*fn)(void))
{
    s_rescan = fn;
}

int nv2a_pcache_rescan(void)
{
    if (!s_rescan || !s_stat.active)
        return 0;
    s_rescan();
    return 1;
}

/* ---- vertex programs in an executable ------------------------------------------ */

size_t nv2a_pcache_scan_vsh(const void *image, size_t size,
                            void (*fn)(void *ctx, const uint32_t *slots, uint32_t n),
                            void *ctx)
{
    const unsigned char *b = (const unsigned char *)image;
    uint32_t slots[136 * 4];
    size_t i, found = 0;

    for (i = 0; i + 4 + 16 <= size; i++) {
        uint32_t n, k;
        if (b[i] != 0x78 || b[i + 1] != 0x20)
            continue;
        n = (uint32_t)b[i + 2] | ((uint32_t)b[i + 3] << 8);
        if (!n || n > 136 || size - i - 4 < (size_t)n * 16u)
            continue;
        memcpy(slots, b + i + 4, (size_t)n * 16u);
        for (k = 0; k < n; k++)
            if (slots[k * 4] != 0 || ((slots[k * 4 + 3] & 1u) != 0) != (k == n - 1))
                break;
        if (k < n)
            continue;
        if (fn)
            fn(ctx, slots, n);
        found++;
        i += 3 + (size_t)n * 16u;
    }
    return found;
}
