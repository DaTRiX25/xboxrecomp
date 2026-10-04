/*
 * enhance_cfg test: the TOML subset (recomp_cfg.c), its errors with line
 * numbers, and the env > title file > root file > default layering
 * (enhance_cfg.c). Runs in its working directory, where it writes small
 * files under ecfg_root/.
 */
#include "enhance_cfg.h"
#include "recomp_cfg.h"
#include "recomp_env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

/* ── the subset ────────────────────────────────────────────────────── */

static void test_subset(void)
{
    static const char text[] =
        "\xEF\xBB\xBF# enhancements\n"
        "title = \"BLiNX 2\"   # trailing comment\n"
        "\n"
        "[render]\n"
        "scale = 2\n"
        "big = 1_000_000\n"
        "neg = -42\n"
        "hex = 0xFF_FF\n"
        "oct = 0o17\n"
        "bin = 0b101\n"
        "sharp = 0.5\n"
        "exp = 1e3\n"
        "nexp = -2.5E-2\n"
        "huge = inf\n"
        "notnum = nan\n"
        "filter = 'C:\\path\\raw'\n"
        "esc = \"a\\tb\\n\\\"q\\\" \\u00e9\"\n"
        "\"quoted-key\" = true\n"
        "vsync = false\n"
        "\r\n"
        "[fps]\n"
        "mode = \"lock60\"\n"
        "\n"
        "[aspect.program]\n"
        "a1b2c3 = \"hud\"\n"
        "\n"
        "[display]\n"
        "aspect = \"16:9\"\n"
        "sizes = [1, 2, 3]\n"
        "mixed = [ \"x\", 2.5, true, ]  # trailing comma\n"
        "multi = [\n"
        "  \"one\",   # first\n"
        "  \"two\"\n"
        "]\n"
        "empty = []\n"
        "dotted.inner = 7\n";
    char err[256];
    recomp_cfg *c = recomp_cfg_load_string(text, "subset", err, sizeof err);
    static const char *const fps[] = { "lock30", "lock60", "free", NULL };

    CHECK(c != NULL);
    if (!c) { fprintf(stderr, "%s\n", err); return; }
    CHECK(!strcmp(recomp_cfg_string(c, "title", ""), "BLiNX 2"));
    CHECK(recomp_cfg_int(c, "render.scale", 0) == 2);
    CHECK(recomp_cfg_int(c, "render.big", 0) == 1000000);
    CHECK(recomp_cfg_int(c, "render.neg", 0) == -42);
    CHECK(recomp_cfg_int(c, "render.hex", 0) == 0xFFFF);
    CHECK(recomp_cfg_int(c, "render.oct", 0) == 15);
    CHECK(recomp_cfg_int(c, "render.bin", 0) == 5);
    CHECK(recomp_cfg_float(c, "render.sharp", 0) == 0.5);
    CHECK(recomp_cfg_float(c, "render.exp", 0) == 1000.0);
    CHECK(fabs(recomp_cfg_float(c, "render.nexp", 0) + 0.025) < 1e-12);
    CHECK(isinf(recomp_cfg_float(c, "render.huge", 0)));
    CHECK(isnan(recomp_cfg_float(c, "render.notnum", 0)));
    CHECK(recomp_cfg_float(c, "render.scale", 0) == 2.0);       /* int as float */
    CHECK(!strcmp(recomp_cfg_string(c, "render.filter", ""), "C:\\path\\raw"));
    CHECK(!strcmp(recomp_cfg_string(c, "render.esc", ""), "a\tb\n\"q\" \xC3\xA9"));
    CHECK(recomp_cfg_bool(c, "render.quoted-key", 0) == 1);
    CHECK(recomp_cfg_bool(c, "render.vsync", 1) == 0);
    CHECK(recomp_cfg_choice(c, "fps.mode", fps, -1) == 1);
    CHECK(!strcmp(recomp_cfg_string(c, "aspect.program.a1b2c3", ""), "hud"));
    CHECK(!strcmp(recomp_cfg_string(c, "display.aspect", ""), "16:9"));
    CHECK(recomp_cfg_int(c, "display.dotted.inner", 0) == 7);

    /* defaults: missing key, wrong type, NULL config */
    CHECK(recomp_cfg_int(c, "render.missing", 9) == 9);
    CHECK(recomp_cfg_int(c, "render.sharp", 9) == 9);
    CHECK(recomp_cfg_bool(c, "render.scale", 1) == 1);
    CHECK(!strcmp(recomp_cfg_string(c, "render.scale", "d"), "d"));
    CHECK(recomp_cfg_type_of(c, "render") == RCFG_NONE);
    CHECK(recomp_cfg_int(NULL, "render.scale", 3) == 3);
    CHECK(recomp_cfg_choice(c, "display.aspect", fps, 0) == 0);

    /* arrays */
    CHECK(recomp_cfg_type_of(c, "display.sizes") == RCFG_ARRAY);
    CHECK(recomp_cfg_array_len(c, "display.sizes") == 3);
    CHECK(recomp_cfg_array_int(c, "display.sizes", 2, 0) == 3);
    CHECK(recomp_cfg_array_int(c, "display.sizes", 3, -1) == -1);
    CHECK(recomp_cfg_array_len(c, "display.mixed") == 3);
    CHECK(!strcmp(recomp_cfg_array_string(c, "display.mixed", 0, ""), "x"));
    CHECK(recomp_cfg_array_float(c, "display.mixed", 1, 0) == 2.5);
    CHECK(recomp_cfg_array_bool(c, "display.mixed", 2, 0) == 1);
    CHECK(recomp_cfg_array_type(c, "display.mixed", 1) == RCFG_FLOAT);
    CHECK(recomp_cfg_array_len(c, "display.multi") == 2);
    CHECK(!strcmp(recomp_cfg_array_string(c, "display.multi", 1, ""), "two"));
    CHECK(recomp_cfg_type_of(c, "display.empty") == RCFG_ARRAY);
    CHECK(recomp_cfg_array_len(c, "display.empty") == 0);
    CHECK(recomp_cfg_array_len(c, "render.scale") == 0);

    /* iteration and lines */
    CHECK(!strcmp(recomp_cfg_key_at(c, 0), "title"));
    CHECK(recomp_cfg_line(c, "render.scale") == 5);
    CHECK(recomp_cfg_line(c, "fps.mode") == 22);
    CHECK(recomp_cfg_line(c, "display.dotted.inner") == 36);
    CHECK(recomp_cfg_find(c, "nope") == -1);
    recomp_cfg_free(c);

    c = recomp_cfg_load_string("", "empty", err, sizeof err);
    CHECK(c && recomp_cfg_count(c) == 0);
    recomp_cfg_free(c);
    c = recomp_cfg_load_string("# only a comment", "comment", err, sizeof err);
    CHECK(c && recomp_cfg_count(c) == 0);
    recomp_cfg_free(c);
}

/* ── errors carry the line ─────────────────────────────────────────── */

static void expect_error(const char *text, const char *want, int line)
{
    char err[256], prefix[32];
    recomp_cfg *c = recomp_cfg_load_string(text, "e", err, sizeof err);
    s_checks++;
    snprintf(prefix, sizeof prefix, "e:%d: ", line);
    if (c || strncmp(err, prefix, strlen(prefix)) || !strstr(err, want)) {
        s_fail++;
        fprintf(stderr, "FAIL error for %s\n  want \"%s%s\"\n  got  \"%s\"\n",
                text, prefix, want, c ? "(parsed)" : err);
    }
    recomp_cfg_free(c);
}

static void test_errors(void)
{
    expect_error("a = 1\nb = \n", "expected a value", 2);
    expect_error("a = 1\na = 2\n", "duplicate key 'a' (first set on line 1)", 2);
    expect_error("[r]\nx = 1\n[r]\n", "table [r] defined twice", 3);
    expect_error("x = 1\n[x]\n", "'x' is a value", 2);
    expect_error("x = 1\nx.y = 2\n", "'x' is a value", 2);
    expect_error("[t.a]\nx = 1\n[t]\na = 2\n", "'t.a' is a table", 4);
    expect_error("x.y = 1\nx = 2\n", "'x' is a table", 2);
    expect_error("s = \"open\n", "unterminated string", 1);
    expect_error("\n\ns = \"\"\"multi\"\"\"\n", "multi-line strings", 3);
    expect_error("t = {a = 1}\n", "inline tables", 1);
    expect_error("[[arr]]\n", "arrays of tables", 1);
    expect_error("a = [[1]]\n", "nested arrays", 1);
    expect_error("d = 1979-05-27T07:32:00\n", "dates and times", 1);
    expect_error("f = linear\n", "strings need quotes", 1);
    expect_error("n = 012\n", "leading zero", 1);
    expect_error("n = 1__0\n", "misplaced '_'", 1);
    expect_error("n = 1.\n", "invalid number", 1);
    expect_error("n = 99999999999999999999\n", "out of range", 1);
    expect_error("a = 1 2\n", "expected end of line", 1);
    expect_error("a 1\n", "expected '='", 1);
    expect_error("= 1\n", "expected a key", 1);
    expect_error("[render\n", "expected ']'", 1);
    expect_error("a = [1,\n2\n", "unterminated array", 3);
    expect_error("a = [1 2]\n", "expected ',' or ']'", 1);
    expect_error("s = \"bad \\q\"\n", "bad escape", 1);
    expect_error("b = True\n", "strings need quotes", 1);
    {
        char err[256];
        CHECK(recomp_cfg_load_file("ecfg_does_not_exist.toml", err, sizeof err) == NULL);
        CHECK(strstr(err, "cannot open") != NULL);
    }
}

/* ── env > title file > root file > default ────────────────────────── */

static void test_layers(void)
{
    static const char *const fps[] = { "lock30", "lock60", "free", NULL };
    static const char *const aspect[] = { "4:3", "16:9", "16:10", "21:9", NULL };

    MKDIR("ecfg_root");
    MKDIR("ecfg_root/TITLE");
    write_file("ecfg_root/enhance.toml",
               "[render]\nscale = 3\nfilter = \"nearest\"\n"
               "[display]\naspect = \"16:10\"\n"
               "[fps]\nmode = \"free\"\n"
               "[extra]\nunread = 1\n");
    write_file("ecfg_root/TITLE/enhance.toml",
               "[render]\nscale = 2\nsharp = 0.25\nvsync = true\nmodes = [1, 2]\n"
               "[fps]\nmode = \"lock60\"\n"
               "[display]\nbadtype = 5\n");

    recomp_env_set(RENV_ENHANCE_CONFIG, NULL);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);

    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 2);
    /* title file over root file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_FILE);
    /* root file where the title file is silent */
    CHECK(!strcmp(enhance_cfg_string("render.filter", "linear"), "nearest"));
    CHECK(enhance_cfg_choice("display.aspect", aspect, 0) == 2);
    /* default when neither has it */
    CHECK(enhance_cfg_int("render.missing", 7) == 7);
    CHECK(enhance_cfg_source_of("render.missing") == ENHANCE_SRC_DEFAULT);
    CHECK(enhance_cfg_float("render.sharp", 1.0) == 0.25);
    CHECK(enhance_cfg_float("render.scale", 1.0) == 2.0);
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    /* wrong type in the file: the default, not a coercion */
    CHECK(!strcmp(enhance_cfg_string("display.badtype", "dflt"), "dflt"));
    {
        const recomp_cfg *c = enhance_cfg_lookup("render.modes");
        CHECK(c == enhance_cfg_file(0));
        CHECK(recomp_cfg_array_len(c, "render.modes") == 2);
        CHECK(enhance_cfg_lookup("nope") == NULL);
    }

    /* fps.mode is a game key: unbound, the file answers; bound, env wins. */
    CHECK(enhance_cfg_choice("fps.mode", fps, 0) == 1);
    CHECK(enhance_cfg_bind_env("fps.mode", RENV_LOG_LEVEL));   /* any config id */
    recomp_env_set(RENV_LOG_LEVEL, "free");
    CHECK(enhance_cfg_choice("fps.mode", fps, 0) == 2);
    CHECK(enhance_cfg_source_of("fps.mode") == ENHANCE_SRC_ENV);
    recomp_env_set(RENV_LOG_LEVEL, "bogus");                    /* invalid: file */
    CHECK(enhance_cfg_choice("fps.mode", fps, 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, NULL);

    /* env over both files */
    recomp_env_set(RENV_RENDER_SCALE, "4");
    CHECK(enhance_cfg_int("render.scale", 1) == 4);
    CHECK(enhance_cfg_float("render.scale", 1) == 4.0);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_ENV);
    recomp_env_set(RENV_RENDER_SCALE, "0x2");
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, "two");                   /* not a number: file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, "");                      /* empty = unset */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, "21:9");
    CHECK(enhance_cfg_choice("display.aspect", aspect, 0) == 3);
    CHECK(!strcmp(enhance_cfg_string("display.aspect", ""), "21:9"));
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);

    /* bool spellings from env */
    CHECK(enhance_cfg_bind_env("render.vsync", RENV_LOG_LEVEL));
    recomp_env_set(RENV_LOG_LEVEL, "Off");
    CHECK(enhance_cfg_bool("render.vsync", 1) == 0);
    recomp_env_set(RENV_LOG_LEVEL, "yes");
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, "maybe");                    /* file: true */
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, NULL);

    /* only extra.unread was never read */
    CHECK(enhance_cfg_report_unused() == 1);

    /* real environment, read through recomp_env */
    set_env("RECOMP_RENDER_SCALE", "5");
    recomp_env_reload();
    CHECK(enhance_cfg_int("render.scale", 1) == 5);
    set_env("RECOMP_RENDER_SCALE", NULL);
    recomp_env_reload();
    CHECK(enhance_cfg_int("render.scale", 1) == 2);

    /* RECOMP_ENHANCE_CONFIG: one file, or none */
    recomp_env_set(RENV_ENHANCE_CONFIG, "ecfg_root/enhance.toml");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 1);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    recomp_env_set(RENV_ENHANCE_CONFIG, "none");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 0);
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
    CHECK(enhance_cfg_file(0) == NULL && enhance_cfg_file(1) == NULL);
    recomp_env_set(RENV_RENDER_SCALE, "2");                     /* env with no file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_ENHANCE_CONFIG, "ecfg_root/missing.toml");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == -1);
    recomp_env_set(RENV_ENHANCE_CONFIG, NULL);

    /* no title file: root only; a broken title file is skipped whole */
    CHECK(enhance_cfg_init("ecfg_root", "OTHER") == 1);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    write_file("ecfg_root/TITLE/enhance.toml", "[render]\nscale = 2\nscale = 3\n");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == -1);
    CHECK(enhance_cfg_file(0) == NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    enhance_cfg_shutdown();
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
}

int main(void)
{
    test_subset();
    test_errors();
    test_layers();
    printf("enhance_cfg: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
