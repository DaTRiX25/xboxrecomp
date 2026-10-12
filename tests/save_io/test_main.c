/* save_io: recomp_save_dir.h's export and import, on a made save tree.
 *   save_io_test <work dir> */
#include "recomp_save_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#else
#include <unistd.h>
#endif

#define ID "4541005b"

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static char g_work[700];

static const char *in(const char *rel)
{
    static char p[8][1024];
    static int k;
    char *o = p[k++ & 7];
    snprintf(o, sizeof p[0], "%s/%s", g_work, rel);
    return o;
}

static void dirs(const char *rel)
{
    char p[1024];
    size_t i;
    snprintf(p, sizeof p, "%s", in(rel));
    for (i = strlen(g_work) + 1; p[i]; i++)
        if (p[i] == '/') { p[i] = 0; mkdir(p, 0755); p[i] = '/'; }
    mkdir(p, 0755);
}

static void put(const char *rel, const char *text)
{
    char d[1024];
    char *slash;
    FILE *f;
    snprintf(d, sizeof d, "%s", rel);
    slash = strrchr(d, '/');
    if (slash) { *slash = 0; dirs(d); }
    f = fopen(in(rel), "wb");
    if (!f) { CHECK("put", 0); return; }
    fwrite(text, 1, strlen(text) + 1, f);        /* with the NUL: not a text file */
    fclose(f);
}

static int has(const char *path, const char *text)
{
    char buf[256] = "";
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return n == strlen(text) + 1 && !strcmp(buf, text);
}

static int there(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static const char *join(const char *a, const char *b)
{
    static char p[8][1400];
    static int k;
    char *o = p[k++ & 7];
    snprintf(o, sizeof p[0], "%s/%s", a, b);
    return o;
}

/* A save root as a start leaves it: this title's save and title data, and
 * what must not travel with it. */
static void make_root(const char *root, const char *mark)
{
    char rel[256];
    snprintf(rel, sizeof rel, "%s/UDATA/" ID "/TitleMeta.xbx", root);            put(rel, "meta");
    snprintf(rel, sizeof rel, "%s/UDATA/" ID "/57BD267AFF58/Profile 1", root);    put(rel, mark);
    snprintf(rel, sizeof rel, "%s/UDATA/" ID "/57BD267AFF58/SaveMeta.xbx", root); put(rel, "savemeta");
    snprintf(rel, sizeof rel, "%s/TDATA/" ID "/cache.dat", root);                 put(rel, mark);
    snprintf(rel, sizeof rel, "%s/TDATA/FFFE0000/MUSIC.HOST/ST.DB", root);        put(rel, "music database");
    snprintf(rel, sizeof rel, "%s/UDATA/11112222/other title", root);             put(rel, "not ours");
    snprintf(rel, sizeof rel, "%s/Partition1.img", root);                         put(rel, "image");
    snprintf(rel, sizeof rel, "%s/settings.toml", root);                          put(rel, "settings");
}

int main(int argc, char **argv)
{
    char out[1024], out2[1024], err[1400], text[2048], newest[1024], staged[1024];
    static char A[1024], B[1024], home[1024];

    if (argc < 2) { printf("usage: save_io_test <work dir>\n"); return 2; }
    snprintf(g_work, sizeof g_work, "%s", argv[1]);
    mkdir(g_work, 0755);
    make_root("A", "profile of A");
    make_root("B", "profile of B");
    dirs("empty");
    dirs("home");
    snprintf(A, sizeof A, "%s", in("A"));
    snprintf(B, sizeof B, "%s", in("B"));
    snprintf(home, sizeof home, "%s", in("home/Takedawn-saves"));

    /* ── export ── */
    CHECK("A has a save", recomp_save_has_data(A, ID));
    CHECK("an empty root has none", !recomp_save_has_data(in("empty"), ID));
    CHECK("nor one of another title", !recomp_save_has_data(A, "99998888"));
    err[0] = 0;
    CHECK("nothing to export says so", !recomp_save_export(in("empty"), ID, home, NULL, out, sizeof out,
                                                           err, sizeof err) && strstr(err, "no save yet"));
    CHECK("export", recomp_save_export(A, ID, home, "from a test", out, sizeof out, err, sizeof err));
    CHECK("in the home folder, dated", !strncmp(out, home, strlen(home)) && strlen(out) == strlen(home) + 16);
    CHECK("the profile, unchanged", has(join(out, "UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of A"));
    CHECK("the title's meta file", has(join(out, "UDATA/" ID "/TitleMeta.xbx"), "meta"));
    CHECK("the title data", has(join(out, "TDATA/" ID "/cache.dat"), "profile of A"));
    CHECK("not the music database", !there(join(out, "TDATA/FFFE0000")));
    CHECK("not another title", !there(join(out, "UDATA/11112222")));
    CHECK("not the images or settings", !there(join(out, "Partition1.img")) && !there(join(out, "settings.toml")));
    CHECK("the note", there(join(out, "save-export.txt")));
    CHECK("it checks", recomp_save_export_check(out, ID, err, sizeof err));
    /* A second one in the same second gets a name of its own. */
    CHECK("export again", recomp_save_export(A, ID, home, NULL, out2, sizeof out2, err, sizeof err)
                          && strcmp(out, out2) != 0);
    /* A target that cannot be made: a file is in the way. */
    put("home/afile", "x");
    err[0] = 0;
    CHECK("an unwritable target says so", !recomp_save_export(A, ID, in("home/afile/sub"), NULL, out2,
                                                              sizeof out2, err, sizeof err) && err[0]);

    /* ── the check ── */
    err[0] = 0;
    CHECK("another title's export is refused", !recomp_save_export_check(out, "99998888", err, sizeof err)
                                               && strstr(err, "another game"));
    dirs("home/Takedawn-saves/zzz-notes");
    err[0] = 0;
    CHECK("a folder without a save is refused", !recomp_save_export_check(in("home/Takedawn-saves/zzz-notes"),
                                                                           ID, err, sizeof err)
                                                && strstr(err, "not a save of this game"));
    CHECK("a missing folder is refused", !recomp_save_export_check(in("home/none"), ID, err, sizeof err));
    /* A hand copy without the note, the folder spelled another way. */
    put("hand/udata/4541005B/TitleMeta.xbx", "meta");
    put("hand/udata/4541005B/AAAA/Profile 1", "by hand");
    CHECK("a hand copy with the same shape is taken", recomp_save_export_check(in("hand"), ID, err, sizeof err));

    /* ── the newest ── */
    CHECK("the newest export, not the notes folder after it",
          recomp_save_export_newest(home, ID, newest, sizeof newest) && strstr(newest, "zzz") == NULL
          && recomp_save_export_check(newest, ID, NULL, 0));
    CHECK("none where there is none", !recomp_save_export_newest(in("empty"), ID, newest, sizeof newest));
    CHECK("none for another title", !recomp_save_export_newest(home, "99998888", newest, sizeof newest));

    /* ── import: staged, then at start ── */
    CHECK("nothing staged", !recomp_save_import_staged(B, staged, sizeof staged));
    CHECK("apply with nothing staged", recomp_save_import_apply(B, ID, text, sizeof text) == 0);
    err[0] = 0;
    CHECK("staging what is not a save is refused", !recomp_save_import_stage(B, in("home/Takedawn-saves/zzz-notes"),
                                                                             ID, err, sizeof err) && err[0]
                                                   && !recomp_save_import_staged(B, staged, sizeof staged));
    CHECK("stage", recomp_save_import_stage(B, out, ID, err, sizeof err));
    CHECK("staged", recomp_save_import_staged(B, staged, sizeof staged) && !strcmp(staged, out));
    CHECK("B untouched until the start", has(in("B/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of B"));
    recomp_save_import_cancel(B);
    CHECK("cancelled", !recomp_save_import_staged(B, staged, sizeof staged)
                       && recomp_save_import_apply(B, ID, text, sizeof text) == 0);

    CHECK("stage again", recomp_save_import_stage(B, out, ID, err, sizeof err));
    put("B/UDATA/" ID "/57BD267AFF58/only in B", "extra");
    CHECK("apply", recomp_save_import_apply(B, ID, text, sizeof text) == 1);
    CHECK("the report names both folders", strstr(text, out) && strstr(text, "save-before-import"));
    CHECK("B has A's profile", has(in("B/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of A"));
    CHECK("and A's title data", has(in("B/TDATA/" ID "/cache.dat"), "profile of A"));
    CHECK("replaced, not merged", !there(in("B/UDATA/" ID "/57BD267AFF58/only in B")));
    CHECK("the rest of B is as it was", has(in("B/UDATA/11112222/other title"), "not ours")
                                        && has(in("B/TDATA/FFFE0000/MUSIC.HOST/ST.DB"), "music database")
                                        && has(in("B/settings.toml"), "settings"));
    {
        /* The old save, aside: <B>/save-before-import/<stamp>/UDATA/<id>/... */
        const char *at = strstr(text, "is in ");
        char before[1024] = "";
        if (at) {
            snprintf(before, sizeof before, "%s", at + 6);
            before[strcspn(before, "\r\n")] = 0;
            if (before[0] && before[strlen(before) - 1] == '.')
                before[strlen(before) - 1] = 0;
        }
        CHECK("B's old profile was set aside", has(join(before, "UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of B")
                                               && has(join(before, "UDATA/" ID "/57BD267AFF58/only in B"), "extra")
                                               && has(join(before, "TDATA/" ID "/cache.dat"), "profile of B"));
        CHECK("beside the save, in its own folder", !strncmp(before, B, strlen(B))
                                                    && strstr(before, "/save-before-import/"));
    }
    CHECK("done once", !recomp_save_import_staged(B, staged, sizeof staged));
    CHECK("the result is kept for this start", recomp_save_import_result(B, text, sizeof text)
                                               && strstr(text, "Imported"));
    CHECK("and gone at the next", recomp_save_import_apply(B, ID, text, sizeof text) == 0
                                  && !recomp_save_import_result(B, text, sizeof text));

    /* Into a root with no save yet. */
    CHECK("stage into an empty root", recomp_save_import_stage(in("empty"), out, ID, err, sizeof err));
    CHECK("apply there", recomp_save_import_apply(in("empty"), ID, text, sizeof text) == 1
                         && strstr(text, "no save here before")
                         && has(in("empty/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of A"));

    /* The export vanished between asking and the start: nothing changes. */
    make_root("C", "profile of C");
    put("gone/UDATA/" ID "/TitleMeta.xbx", "meta");
    CHECK("stage one that will vanish", recomp_save_import_stage(in("C"), in("gone"), ID, err, sizeof err));
    remove(in("gone/UDATA/" ID "/TitleMeta.xbx"));
    CHECK("apply fails", recomp_save_import_apply(in("C"), ID, text, sizeof text) == -1
                         && strstr(text, "not imported") && strstr(text, "unchanged"));
    CHECK("C is as it was", has(in("C/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of C")
                            && !there(in("C/save-before-import")));
    CHECK("and it is not tried again", recomp_save_import_apply(in("C"), ID, text, sizeof text) == 0);

    /* The hand copy imports too, into the title's own spelling. */
    CHECK("stage the hand copy", recomp_save_import_stage(in("C"), in("hand"), ID, err, sizeof err));
    CHECK("apply it", recomp_save_import_apply(in("C"), ID, text, sizeof text) == 1
                      && has(in("C/UDATA/" ID "/AAAA/Profile 1"), "by hand"));

    {
        char h[1024];
        CHECK("a home folder", recomp_home_dir(h, sizeof h) && h[0]);
    }
    printf(failures ? "%d FAILED\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
