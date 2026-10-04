/*
 * recomp_cfg.c - a small TOML-subset config reader; see recomp_cfg.h.
 *
 * One pass over the whole text with a cursor and a line counter. Entries are
 * kept flat, by full dotted key, in file order; lookups are linear (a config
 * file has tens of keys and is read at startup).
 */
#include "recomp_cfg.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    recomp_cfg_type type;
    union { char *s; long long i; double f; int b; } v;
} Val;

typedef struct {
    char *key;
    int line;
    Val val;          /* type RCFG_ARRAY: items below */
    Val *items;
    size_t n_items;
} Entry;

struct recomp_cfg {
    Entry *e;
    size_t n, cap;
    char **tables;    /* [headers] seen, to refuse a second [render] */
    size_t n_tables, cap_tables;
    char *name;
};

typedef struct {
    const char *p, *end;
    int line;
    recomp_cfg *cfg;
    char *err;
    size_t errlen;
    int failed;
} P;

/* ── small helpers ─────────────────────────────────────────────────── */

static char *dup_n(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static int fail(P *ps, const char *fmt, ...)
{
    if (!ps->failed && ps->err && ps->errlen) {
        char msg[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        snprintf(ps->err, ps->errlen, "%s:%d: %s", ps->cfg->name, ps->line, msg);
    }
    ps->failed = 1;
    return 0;
}

static void val_free(Val *v)
{
    if (v->type == RCFG_STRING)
        free(v->v.s);
}

/* Growable byte buffer for keys and strings. */
typedef struct { char *b; size_t n, cap; } Buf;

static int buf_put(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        char *nb;
        while (cap < b->n + n + 1)
            cap *= 2;
        nb = (char *)realloc(b->b, cap);
        if (!nb)
            return 0;
        b->b = nb;
        b->cap = cap;
    }
    memcpy(b->b + b->n, s, n);
    b->n += n;
    b->b[b->n] = 0;
    return 1;
}

static int is_bare(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int is_alnum(char c)
{
    return is_bare(c) && c != '_' && c != '-';
}

static void skip_ws(P *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t'))
        ps->p++;
}

static void skip_comment(P *ps)
{
    if (ps->p < ps->end && *ps->p == '#')
        while (ps->p < ps->end && *ps->p != '\n')
            ps->p++;
}

/* Consume a newline (\n or \r\n). 1 if one was there. */
static int eat_newline(P *ps)
{
    if (ps->p < ps->end && *ps->p == '\n') {
        ps->p++;
        ps->line++;
        return 1;
    }
    if (ps->end - ps->p >= 2 && ps->p[0] == '\r' && ps->p[1] == '\n') {
        ps->p += 2;
        ps->line++;
        return 1;
    }
    return 0;
}

/* Whitespace, an optional comment, then end of line or of input. */
static int expect_eol(P *ps)
{
    skip_ws(ps);
    skip_comment(ps);
    if (ps->p >= ps->end || eat_newline(ps))
        return 1;
    return fail(ps, "expected end of line, found '%c'", *ps->p);
}

/* Whitespace, newlines and comments, inside an array. */
static void skip_blank(P *ps)
{
    for (;;) {
        skip_ws(ps);
        skip_comment(ps);
        if (!eat_newline(ps))
            return;
    }
}

/* ── strings ───────────────────────────────────────────────────────── */

static int put_utf8(Buf *b, unsigned long cp)
{
    char u[4];
    size_t n;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                             u[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
           u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    return buf_put(b, u, n);
}

/* At a '"' or '\''. Appends the decoded string to `out`. */
static int parse_string(P *ps, Buf *out)
{
    char q = *ps->p;
    if (ps->end - ps->p >= 3 && ps->p[1] == q && ps->p[2] == q)
        return fail(ps, "multi-line strings are not supported");
    ps->p++;
    for (;;) {
        char c;
        if (ps->p >= ps->end || *ps->p == '\n' || *ps->p == '\r')
            return fail(ps, "unterminated string");
        c = *ps->p++;
        if (c == q)
            return 1;
        if (c == '\\' && q == '"') {
            char e;
            if (ps->p >= ps->end)
                return fail(ps, "unterminated string");
            e = *ps->p++;
            switch (e) {
            case 'b': c = '\b'; break;
            case 't': c = '\t'; break;
            case 'n': c = '\n'; break;
            case 'f': c = '\f'; break;
            case 'r': c = '\r'; break;
            case 'e': c = 0x1B; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case 'u': case 'U': {
                int nd = e == 'u' ? 4 : 8, k;
                unsigned long cp = 0;
                for (k = 0; k < nd; k++) {
                    char h = ps->p < ps->end ? *ps->p : 0;
                    int d = (h >= '0' && h <= '9') ? h - '0' :
                            (h >= 'a' && h <= 'f') ? h - 'a' + 10 :
                            (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (d < 0)
                        return fail(ps, "bad \\%c escape", e);
                    cp = cp * 16 + (unsigned long)d;
                    ps->p++;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return fail(ps, "bad unicode code point");
                if (!put_utf8(out, cp))
                    return fail(ps, "out of memory");
                continue;
            }
            default:
                return fail(ps, "bad escape '\\%c'", e);
            }
        } else if ((unsigned char)c < 0x20 && c != '\t') {
            return fail(ps, "control character in string");
        }
        if (!buf_put(out, &c, 1))
            return fail(ps, "out of memory");
    }
}

/* ── keys ──────────────────────────────────────────────────────────── */

/* A bare, quoted or dotted key, appended to `out` with '.' between parts. */
static int parse_key(P *ps, Buf *out)
{
    for (;;) {
        skip_ws(ps);
        if (ps->p < ps->end && (*ps->p == '"' || *ps->p == '\'')) {
            size_t before = out->n;
            if (!parse_string(ps, out))
                return 0;
            if (out->n == before)
                return fail(ps, "empty key");
            if (memchr(out->b + before, '.', out->n - before))
                return fail(ps, "'.' inside a quoted key is not supported");
        } else {
            const char *s = ps->p;
            while (ps->p < ps->end && is_bare(*ps->p))
                ps->p++;
            if (ps->p == s)
                return ps->p < ps->end && *ps->p != '\n'
                     ? fail(ps, "expected a key, found '%c'", *ps->p)
                     : fail(ps, "expected a key");
            if (!buf_put(out, s, (size_t)(ps->p - s)))
                return fail(ps, "out of memory");
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '.') {
            ps->p++;
            if (!buf_put(out, ".", 1))
                return fail(ps, "out of memory");
            continue;
        }
        return 1;
    }
}

/* ── values ────────────────────────────────────────────────────────── */

static int parse_number(P *ps, Val *v)
{
    char tok[128];
    size_t n = 0;
    const char *s = ps->p, *t;
    int sign = 0, is_float = 0;
    char *endp;

    while (ps->p < ps->end && (is_bare(*ps->p) || *ps->p == '+' || *ps->p == '.')) {
        if (*ps->p != '_') {
            if (n + 1 >= sizeof tok)
                return fail(ps, "number too long");
            tok[n++] = *ps->p;
        } else if (ps->p == s || !is_alnum(ps->p[-1]) ||
                   ps->p + 1 >= ps->end || !is_alnum(ps->p[1])) {
            return fail(ps, "misplaced '_' in number");
        }
        ps->p++;
    }
    tok[n] = 0;
    if (ps->p < ps->end && *ps->p == ':')
        return fail(ps, "dates and times are not supported");
    if (n == 0)
        return ps->p < ps->end ? fail(ps, "expected a value, found '%c'", *ps->p)
                               : fail(ps, "expected a value");

    t = tok;
    if (*t == '+' || *t == '-')
        sign = *t++ == '-' ? -1 : 1;
    if (!strcmp(t, "inf") || !strcmp(t, "nan")) {
        v->type = RCFG_FLOAT;
        v->v.f = t[0] == 'i' ? (sign < 0 ? -HUGE_VAL : HUGE_VAL) : NAN;
        return 1;
    }
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'o' || t[1] == 'b')) {
        int base = t[1] == 'x' ? 16 : t[1] == 'o' ? 8 : 2;
        unsigned long long u;
        if (sign)
            return fail(ps, "sign on a 0%c integer", t[1]);
        if (!t[2])
            return fail(ps, "invalid number '%s'", tok);
        errno = 0;
        u = strtoull(t + 2, &endp, base);
        if (*endp || t[2] == '-' || t[2] == '+')
            return fail(ps, "invalid number '%s'", tok);
        if (errno == ERANGE || u > (unsigned long long)9223372036854775807LL)
            return fail(ps, "integer out of range '%s'", tok);
        v->type = RCFG_INT;
        v->v.i = (long long)u;
        return 1;
    }
    if (!(t[0] >= '0' && t[0] <= '9'))
        return fail(ps, "invalid value '%s' (strings need quotes)", tok);
    is_float = strpbrk(t, ".eE") != NULL;
    if (t[0] == '0' && t[1] >= '0' && t[1] <= '9')
        return fail(ps, "leading zero in '%s'", tok);
    errno = 0;
    if (is_float) {
        const char *dot = strchr(t, '.');
        if (dot && !(dot[1] >= '0' && dot[1] <= '9'))
            return fail(ps, "invalid number '%s'", tok);
        v->type = RCFG_FLOAT;
        v->v.f = strtod(tok, &endp);
    } else {
        v->type = RCFG_INT;
        v->v.i = strtoll(tok, &endp, 10);
    }
    if (*endp)
        return fail(ps, "invalid number '%s'", tok);
    if (errno == ERANGE && !is_float)
        return fail(ps, "integer out of range '%s'", tok);
    return 1;
}

static int parse_scalar(P *ps, Val *v)
{
    char c = ps->p < ps->end ? *ps->p : 0;
    if (c == '"' || c == '\'') {
        Buf b = {0};
        if (!buf_put(&b, "", 0) || !parse_string(ps, &b)) {
            free(b.b);
            return ps->failed ? 0 : fail(ps, "out of memory");
        }
        v->type = RCFG_STRING;
        v->v.s = b.b;
        return 1;
    }
    if (c == '{')
        return fail(ps, "inline tables are not supported");
    if (c == '[')
        return fail(ps, "nested arrays are not supported");
    if (ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4) &&
        (ps->end - ps->p == 4 || !is_bare(ps->p[4]))) {
        ps->p += 4;
        v->type = RCFG_BOOL;
        v->v.b = 1;
        return 1;
    }
    if (ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5) &&
        (ps->end - ps->p == 5 || !is_bare(ps->p[5]))) {
        ps->p += 5;
        v->type = RCFG_BOOL;
        v->v.b = 0;
        return 1;
    }
    return parse_number(ps, v);
}

static int parse_array(P *ps, Entry *e)
{
    size_t cap = 0;
    ps->p++;   /* '[' */
    for (;;) {
        skip_blank(ps);
        if (ps->p >= ps->end)
            return fail(ps, "unterminated array");
        if (*ps->p == ']') {
            ps->p++;
            return 1;
        }
        if (e->n_items == cap) {
            size_t nc = cap ? cap * 2 : 4;
            Val *ni = (Val *)realloc(e->items, nc * sizeof *ni);
            if (!ni)
                return fail(ps, "out of memory");
            e->items = ni;
            cap = nc;
        }
        if (!parse_scalar(ps, &e->items[e->n_items]))
            return 0;
        e->n_items++;
        skip_blank(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return 1;
        }
        return ps->p < ps->end ? fail(ps, "expected ',' or ']' in array, found '%c'", *ps->p)
                               : fail(ps, "unterminated array");
    }
}

/* ── the table of entries ──────────────────────────────────────────── */

/* 1 if `a` is `b` or a dotted prefix of it ("render" of "render.scale"). */
static int is_prefix(const char *a, const char *b)
{
    size_t n = strlen(a);
    return !strncmp(a, b, n) && (b[n] == 0 || b[n] == '.');
}

static Entry *entry_get(const recomp_cfg *cfg, const char *key)
{
    long i = recomp_cfg_find(cfg, key);
    return i < 0 ? NULL : &cfg->e[i];
}

/* A key may not repeat, may not sit under a key that holds a value, and may
 * not be a table that already has keys under it. */
static int check_key(P *ps, const char *key)
{
    size_t i;
    for (i = 0; i < ps->cfg->n; i++) {
        const char *k = ps->cfg->e[i].key;
        if (!strcmp(k, key))
            return fail(ps, "duplicate key '%s' (first set on line %d)", key, ps->cfg->e[i].line);
        if (is_prefix(k, key))
            return fail(ps, "'%s' is a value (line %d), not a table", k, ps->cfg->e[i].line);
        if (is_prefix(key, k))
            return fail(ps, "'%s' is a table, not a value", key);
    }
    for (i = 0; i < ps->cfg->n_tables; i++)
        if (is_prefix(key, ps->cfg->tables[i]))
            return fail(ps, "'%s' is a table, not a value", key);
    return 1;
}

static int parse_table_header(P *ps, Buf *table)
{
    size_t i;
    ps->p++;   /* '[' */
    if (ps->p < ps->end && *ps->p == '[')
        return fail(ps, "arrays of tables are not supported");
    table->n = 0;
    if (!buf_put(table, "", 0) || !parse_key(ps, table))
        return 0;
    if (ps->p >= ps->end || *ps->p != ']')
        return fail(ps, "expected ']' after table name");
    ps->p++;
    for (i = 0; i < ps->cfg->n_tables; i++)
        if (!strcmp(ps->cfg->tables[i], table->b))
            return fail(ps, "table [%s] defined twice", table->b);
    for (i = 0; i < ps->cfg->n; i++)
        if (is_prefix(ps->cfg->e[i].key, table->b))
            return fail(ps, "'%s' is a value (line %d), not a table",
                        ps->cfg->e[i].key, ps->cfg->e[i].line);
    if (ps->cfg->n_tables == ps->cfg->cap_tables) {
        size_t nc = ps->cfg->cap_tables ? ps->cfg->cap_tables * 2 : 8;
        char **nt = (char **)realloc(ps->cfg->tables, nc * sizeof *nt);
        if (!nt)
            return fail(ps, "out of memory");
        ps->cfg->tables = nt;
        ps->cfg->cap_tables = nc;
    }
    if (!(ps->cfg->tables[ps->cfg->n_tables] = dup_n(table->b, table->n)))
        return fail(ps, "out of memory");
    ps->cfg->n_tables++;
    return expect_eol(ps);
}

static int parse_keyval(P *ps, const Buf *table)
{
    Buf key = {0};
    Entry e;
    int line = ps->line;

    memset(&e, 0, sizeof e);
    if (table->n && (!buf_put(&key, table->b, table->n) || !buf_put(&key, ".", 1)))
        return fail(ps, "out of memory");
    if (!buf_put(&key, "", 0) || !parse_key(ps, &key))
        goto bad;
    if (ps->p >= ps->end || *ps->p != '=') {
        fail(ps, "expected '=' after key '%s'", key.b);
        goto bad;
    }
    ps->p++;
    skip_ws(ps);
    if (!check_key(ps, key.b))
        goto bad;
    if (ps->p < ps->end && *ps->p == '[') {
        e.val.type = RCFG_ARRAY;
        if (!parse_array(ps, &e))
            goto bad;
    } else if (!parse_scalar(ps, &e.val)) {
        goto bad;
    }
    if (!expect_eol(ps))
        goto bad;
    if (ps->cfg->n == ps->cfg->cap) {
        size_t nc = ps->cfg->cap ? ps->cfg->cap * 2 : 16;
        Entry *ne = (Entry *)realloc(ps->cfg->e, nc * sizeof *ne);
        if (!ne) {
            fail(ps, "out of memory");
            goto bad;
        }
        ps->cfg->e = ne;
        ps->cfg->cap = nc;
    }
    e.key = key.b;
    e.line = line;
    ps->cfg->e[ps->cfg->n++] = e;
    return 1;
bad:
    {
        size_t i;
        for (i = 0; i < e.n_items; i++)
            val_free(&e.items[i]);
        free(e.items);
        val_free(&e.val);
        free(key.b);
    }
    return 0;
}

/* ── loading ───────────────────────────────────────────────────────── */

recomp_cfg *recomp_cfg_load_string(const char *text, const char *name,
                                   char *err, size_t errlen)
{
    recomp_cfg *cfg = (recomp_cfg *)calloc(1, sizeof *cfg);
    Buf table = {0};
    P ps;

    if (err && errlen)
        err[0] = 0;
    if (!cfg || !(cfg->name = dup_n(name ? name : "<string>", strlen(name ? name : "<string>")))) {
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        free(cfg);
        return NULL;
    }
    memset(&ps, 0, sizeof ps);
    ps.p = text ? text : "";
    ps.end = ps.p + strlen(ps.p);
    ps.line = 1;
    ps.cfg = cfg;
    ps.err = err;
    ps.errlen = errlen;
    if (ps.end - ps.p >= 3 && !memcmp(ps.p, "\xEF\xBB\xBF", 3))
        ps.p += 3;   /* UTF-8 byte-order mark */
    buf_put(&table, "", 0);

    while (ps.p < ps.end && !ps.failed) {
        skip_ws(&ps);
        skip_comment(&ps);
        if (ps.p >= ps.end || eat_newline(&ps))
            continue;
        if (*ps.p == '[')
            parse_table_header(&ps, &table);
        else
            parse_keyval(&ps, &table);
    }
    free(table.b);
    if (ps.failed) {
        recomp_cfg_free(cfg);
        return NULL;
    }
    return cfg;
}

recomp_cfg *recomp_cfg_load_file(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long size;
    size_t got;
    recomp_cfg *cfg;

    if (!f) {
        if (err && errlen)
            snprintf(err, errlen, "%s: cannot open: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        !(text = (char *)malloc((size_t)size + 1))) {
        if (err && errlen)
            snprintf(err, errlen, "%s: cannot read", path);
        fclose(f);
        return NULL;
    }
    got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = 0;
    if (strlen(text) != got) {
        if (err && errlen)
            snprintf(err, errlen, "%s: NUL byte in file", path);
        free(text);
        return NULL;
    }
    cfg = recomp_cfg_load_string(text, path, err, errlen);
    free(text);
    return cfg;
}

void recomp_cfg_free(recomp_cfg *cfg)
{
    size_t i, k;
    if (!cfg)
        return;
    for (i = 0; i < cfg->n; i++) {
        for (k = 0; k < cfg->e[i].n_items; k++)
            val_free(&cfg->e[i].items[k]);
        free(cfg->e[i].items);
        val_free(&cfg->e[i].val);
        free(cfg->e[i].key);
    }
    for (i = 0; i < cfg->n_tables; i++)
        free(cfg->tables[i]);
    free(cfg->tables);
    free(cfg->e);
    free(cfg->name);
    free(cfg);
}

/* ── getters ───────────────────────────────────────────────────────── */

long recomp_cfg_find(const recomp_cfg *cfg, const char *key)
{
    size_t i;
    if (!cfg || !key)
        return -1;
    for (i = 0; i < cfg->n; i++)
        if (!strcmp(cfg->e[i].key, key))
            return (long)i;
    return -1;
}

size_t recomp_cfg_count(const recomp_cfg *cfg) { return cfg ? cfg->n : 0; }

const char *recomp_cfg_key_at(const recomp_cfg *cfg, size_t i)
{
    return cfg && i < cfg->n ? cfg->e[i].key : NULL;
}

int recomp_cfg_line(const recomp_cfg *cfg, const char *key)
{
    const Entry *e = entry_get(cfg, key);
    return e ? e->line : 0;
}

const char *recomp_cfg_name(const recomp_cfg *cfg) { return cfg ? cfg->name : NULL; }

recomp_cfg_type recomp_cfg_type_of(const recomp_cfg *cfg, const char *key)
{
    const Entry *e = entry_get(cfg, key);
    return e ? e->val.type : RCFG_NONE;
}

static const char *val_string(const Val *v, const char *def) { return v && v->type == RCFG_STRING ? v->v.s : def; }
static long long val_int(const Val *v, long long def) { return v && v->type == RCFG_INT ? v->v.i : def; }
static int val_bool(const Val *v, int def) { return v && v->type == RCFG_BOOL ? v->v.b : def; }
static double val_float(const Val *v, double def)
{
    if (v && v->type == RCFG_FLOAT)
        return v->v.f;
    if (v && v->type == RCFG_INT)
        return (double)v->v.i;
    return def;
}

static const Val *scalar(const recomp_cfg *cfg, const char *key)
{
    const Entry *e = entry_get(cfg, key);
    return e ? &e->val : NULL;
}

const char *recomp_cfg_string(const recomp_cfg *cfg, const char *key, const char *def) { return val_string(scalar(cfg, key), def); }
long long recomp_cfg_int(const recomp_cfg *cfg, const char *key, long long def) { return val_int(scalar(cfg, key), def); }
double recomp_cfg_float(const recomp_cfg *cfg, const char *key, double def) { return val_float(scalar(cfg, key), def); }
int recomp_cfg_bool(const recomp_cfg *cfg, const char *key, int def) { return val_bool(scalar(cfg, key), def); }

int recomp_cfg_choice(const recomp_cfg *cfg, const char *key,
                      const char *const *choices, int def)
{
    const char *s = recomp_cfg_string(cfg, key, NULL);
    int i;
    if (!s || !choices)
        return def;
    for (i = 0; choices[i]; i++)
        if (!strcmp(choices[i], s))
            return i;
    return def;
}

static const Val *item(const recomp_cfg *cfg, const char *key, size_t i)
{
    const Entry *e = entry_get(cfg, key);
    return e && e->val.type == RCFG_ARRAY && i < e->n_items ? &e->items[i] : NULL;
}

size_t recomp_cfg_array_len(const recomp_cfg *cfg, const char *key)
{
    const Entry *e = entry_get(cfg, key);
    return e && e->val.type == RCFG_ARRAY ? e->n_items : 0;
}

recomp_cfg_type recomp_cfg_array_type(const recomp_cfg *cfg, const char *key, size_t i)
{
    const Val *v = item(cfg, key, i);
    return v ? v->type : RCFG_NONE;
}

const char *recomp_cfg_array_string(const recomp_cfg *cfg, const char *key, size_t i, const char *def) { return val_string(item(cfg, key, i), def); }
long long recomp_cfg_array_int(const recomp_cfg *cfg, const char *key, size_t i, long long def) { return val_int(item(cfg, key, i), def); }
double recomp_cfg_array_float(const recomp_cfg *cfg, const char *key, size_t i, double def) { return val_float(item(cfg, key, i), def); }
int recomp_cfg_array_bool(const recomp_cfg *cfg, const char *key, size_t i, int def) { return val_bool(item(cfg, key, i), def); }
