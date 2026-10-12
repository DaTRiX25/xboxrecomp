/* soundtrack: the music-folder scan, the ST.DB image as the XDK library
 * reads it, and the decoders' output (xbox_soundtrack.h).
 *
 *   soundtrack_test <work dir> <fixture dir>
 *
 * The WAV files are written here; the encoded fixtures (data/, half a second
 * of a 1 kHz sine each) come from make_fixtures.sh. */
#include "xbox_soundtrack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static char g_work[1024], g_data[1024];

static const char *in_work(const char *rel)
{
    static char p[4][1400];
    static int k;
    char *o = p[k++ & 3];
    snprintf(o, sizeof p[0], "%s/%s", g_work, rel);
    return o;
}

static void put(FILE *f, uint32_t v, int n) { while (n--) { fputc((int)(v & 0xFF), f); v >>= 8; } }

/* A 16-bit WAV: a sine of `hz` at half scale, `ms` long. */
static void write_wav(const char *path, unsigned rate, unsigned ch, double hz, unsigned ms)
{
    FILE *f = fopen(path, "wb");
    uint32_t n = rate * ms / 1000, i;
    unsigned c;

    if (!f) { CHECK("write_wav", 0); return; }
    fwrite("RIFF", 1, 4, f); put(f, 36 + n * ch * 2, 4); fwrite("WAVEfmt ", 1, 8, f);
    put(f, 16, 4); put(f, 1, 2); put(f, ch, 2); put(f, rate, 4); put(f, rate * ch * 2, 4);
    put(f, ch * 2, 2); put(f, 16, 2); fwrite("data", 1, 4, f); put(f, n * ch * 2, 4);
    for (i = 0; i < n; i++)
        for (c = 0; c < ch; c++)
            put(f, (uint32_t)(int16_t)lrint(16384.0 * sin(2 * 3.14159265358979323846 * hz * i / rate)), 2);
    fclose(f);
}

static void copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[4096];
    size_t n;
    if (!a || !b) { printf("copy %s -> %s\n", from, to); CHECK("copy_file", 0); }
    while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0)
        fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

static void fixture(const char *name, const char *to_rel)
{
    char from[1400];
    snprintf(from, sizeof from, "%s/%s", g_data, name);
    copy_file(from, in_work(to_rel));
}

static int name_is(const uint16_t *name, const char *ascii)
{
    int i;
    for (i = 0; ascii[i]; i++)
        if (name[i] != (unsigned char)ascii[i])
            return 0;
    return name[i] == 0;
}

/* The share of a signal's power at `hz` (Goertzel), 0..1. */
static double tone_share(const int16_t *pcm, size_t frames, int ch, double hz)
{
    double w = 2 * 3.14159265358979323846 * hz / XST_RATE, c = 2 * cos(w);
    double s0, s1 = 0, s2 = 0, total = 0;
    size_t i;
    for (i = 0; i < frames; i++) {
        double x = pcm[i * 2 + ch];
        s0 = x + c * s1 - s2; s2 = s1; s1 = s0;
        total += x * x;
    }
    if (total == 0 || frames == 0)
        return 0;
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (total * frames / 2.0);
}

static double rms_db(const int16_t *pcm, size_t frames)
{
    double sum = 0;
    size_t i;
    for (i = 0; i < frames * 2; i++)
        sum += (double)pcm[i] * pcm[i];
    return 10 * log10(sum / (frames * 2) / (32768.0 * 32768.0) + 1e-20);
}

static size_t decode_all(const char *path, int16_t *out, size_t cap, unsigned *rate, unsigned *ch)
{
    xst_stream *s = xst_stream_open(path);
    size_t n = 0, got;
    if (!s)
        return 0;
    xst_stream_format(s, rate, ch);
    /* Odd-sized reads, as a caller with its own packet size would make. */
    while (n + 1000 <= cap && (got = xst_stream_read(s, out + n * 2, 1000)) > 0)
        n += got;
    CHECK("a finished stream stays finished", xst_stream_read(s, out, 16) == 0);
    xst_stream_close(s);
    return n;
}

/* ── tests ─────────────────────────────────────────────────────────── */

static void test_names(void)
{
    uint16_t n[XST_NAME_UNITS];

    CHECK("2 before 10", xst_natural_cmp("2 song.mp3", "10 song.mp3") < 0);
    CHECK("case-insensitive", xst_natural_cmp("b.mp3", "A.mp3") > 0);
    CHECK("leading zeros", xst_natural_cmp("02 x", "2 y") < 0);
    CHECK("prefix first", xst_natural_cmp("Track", "Track 2") < 0);
    CHECK("equal", xst_natural_cmp("a1", "a1") == 0);
    CHECK("long digit runs", xst_natural_cmp("20260101120000", "20260101120001") < 0);

    xst_name_from_utf8(n, "\xC3\x81rv\xC3\xADzt\xC5\xB1r\xC5\x91");      /* Árvíztűrő */
    /* Latin-1 as it is; the double acutes, which a menu font does not have,
     * as the diaeresis. */
    CHECK("utf-8 to utf-16", n[0] == 0xC1 && n[3] == 0xED && n[6] == 0xFC && n[8] == 0xF6 && n[9] == 0);
    xst_name_from_utf8(n, "A\xCC\x81rvi\xCC\x81ztu\xCC\x8Bro\xCC\x8B n\xCC\x83 z\xCC\x8C");  /* decomposed */
    CHECK("decomposed letters composed", n[0] == 0xC1 && n[1] == 'r' && n[3] == 0xED && n[4] == 'z'
                                         && n[6] == 0xFC && n[8] == 0xF6 && n[10] == 0xF1
                                         && n[12] == 'z' && n[13] == 0);
    /* "Rip" with a caron and an acute, curly quotes, an en dash, a Cyrillic letter. */
    xst_name_from_utf8(n, "\xC5\x98\xC3\xADp \xE2\x80\x9Cx\xE2\x80\x9D \xE2\x80\x93 \xD0\x96");
    CHECK("folded to Latin-1", n[0] == 'R' && n[1] == 0xED && n[4] == '"' && n[6] == '"'
                               && n[8] == '-' && n[10] == '?' && n[11] == 0);
    xst_name_from_utf8(n, "0123456789012345678901234567890123456789");
    CHECK("cut at 31 units", n[30] == '0' && n[31] == 0);
    xst_name_from_utf8(n, "a\xF0\x9F\x8E\xB5" "b");                      /* outside the BMP */
    CHECK("one unit a character", n[0] == 'a' && n[1] == '?' && n[2] == 'b' && n[3] == 0);
    xst_name_from_utf8(n, "bad\xFF");
    CHECK("invalid byte", n[3] == '?' && n[4] == 0);

    CHECK("mp3", xst_is_playable_name("a.MP3"));
    CHECK("flac", xst_is_playable_name("a.b.flac"));
    CHECK("not m4a", !xst_is_playable_name("a.m4a"));
    CHECK("not hidden", !xst_is_playable_name("._a.mp3"));
    CHECK("no extension", !xst_is_playable_name("mp3"));
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* XGetSoundtrackSongInfo as the XDK library does it: the soundtrack's block
 * by its place in the header's id list, the song's group by index / 6. */
static int db_song(const uint8_t *db, size_t size, uint32_t st_id, uint32_t index,
                   uint32_t *id, uint32_t *ms, uint16_t *name)
{
    uint32_t n = rd32(db + 4), i, gid;
    const uint8_t *st, *g;
    int k;

    if (rd32(db) != 1)
        return 0;
    for (i = 0; i < n && i < 100; i++)
        if (rd32(db + 12 + i * 4) == st_id)
            break;
    if (i == n)
        return 0;
    st = db + 512 * (1 + i);
    if (rd32(st) != 0x21371 || rd32(st + 4) != st_id || index >= rd32(st + 8))
        return 0;
    gid = rd32(st + 12 + (index / 6) * 4);
    if ((size_t)(gid + 0x65 + 1) * 512 > size)
        return 0;
    g = db + (size_t)(gid + 0x65) * 512;
    if (rd32(g) != 0x31073 || rd32(g + 4) != st_id)
        return 0;
    *id = rd32(g + 16 + (index % 6) * 4);
    *ms = rd32(g + 40 + (index % 6) * 4);
    for (k = 0; k < 32; k++)
        name[k] = (uint16_t)(g[64 + (index % 6) * 64 + k * 2] | g[64 + (index % 6) * 64 + k * 2 + 1] << 8);
    return 1;
}

static void test_scan_and_db(void)
{
    xst_library *lib;
    uint8_t *db;
    size_t size;
    char rel[64];
    int i;

    mkdir(in_work("lib"), 0755);
    mkdir(in_work("lib/B album"), 0755);
    mkdir(in_work("lib/a first"), 0755);
    mkdir(in_work("lib/a first/disc 2"), 0755);
    mkdir(in_work("lib/empty"), 0755);
    mkdir(in_work("lib/.hidden"), 0755);
    write_wav(in_work("lib/10 ten.wav"), 44100, 2, 440, 300);
    write_wav(in_work("lib/2 two.wav"), 44100, 1, 440, 200);
    write_wav(in_work("lib/._2 two.wav"), 44100, 1, 440, 200);
    write_wav(in_work("lib/.hidden/x.wav"), 44100, 1, 440, 200);
    copy_file(in_work("lib/2 two.wav"), in_work("lib/notes.txt"));
    copy_file(in_work("lib/notes.txt"), in_work("lib/broken.flac"));        /* not a FLAC */
    fixture("tone_id3v23.mp3", "lib/a first/01.mp3");
    fixture("tone.flac", "lib/a first/02.flac");
    fixture("tone48.ogg", "lib/a first/disc 2/a.ogg");
    for (i = 1; i <= 14; i++) {                                             /* three groups */
        snprintf(rel, sizeof rel, "lib/B album/%d.wav", i);
        write_wav(in_work(rel), 22050, 1, 440, 100 * (unsigned)i);
    }

    lib = xst_scan(in_work("lib"));
    CHECK("scan", lib != NULL);
    if (!lib)
        return;
    CHECK("four soundtracks", lib->n_st == 4);
    CHECK("songs", lib->n_songs == 2 + 2 + 1 + 14);
    CHECK("the broken file is counted", lib->bad_files == 1);
    /* The folder itself first, then its folders in natural order, each
     * followed by its own. */
    CHECK("root named after the folder", name_is(lib->st[0].name, "lib"));
    CHECK("then 'a first'", name_is(lib->st[1].name, "a first"));
    CHECK("then its 'disc 2'", name_is(lib->st[2].name, "disc 2"));
    CHECK("then 'B album'", name_is(lib->st[3].name, "B album"));
    CHECK("ids from 1", lib->st[0].id == 1 && lib->st[3].id == 4);
    CHECK("2 before 10", name_is(lib->songs[0].name, "2 two") && name_is(lib->songs[1].name, "10 ten"));
    CHECK("wav length", lib->songs[0].ms == 200 && lib->songs[1].ms == 300);
    CHECK("soundtrack length", lib->st[0].ms == 500);
    /* Title tags: ID3v2.3 (UTF-16 there), a FLAC and an Ogg comment. */
    CHECK("id3 title", lib->songs[2].name[0] == 0xC1 && lib->songs[2].name[6] == 0xFC
                       && lib->songs[2].name[9] == ' ');
    CHECK("flac title", name_is(lib->songs[3].name, "Flac Title"));
    CHECK("ogg title", name_is(lib->songs[4].name, "Ogg Title"));
    CHECK("mp3 length about 0.5 s", lib->songs[2].ms > 400 && lib->songs[2].ms < 650);
    CHECK("flac length", lib->songs[3].ms == 500);
    CHECK("ogg length", lib->songs[4].ms >= 495 && lib->songs[4].ms <= 505);
    CHECK("song ids", lib->songs[0].id == 0x00010000 && lib->songs[1].id == 0x00010001
                      && lib->songs[5].id == 0x00040000);
    CHECK("by id", xst_song_by_id(lib, 0x00040003) == &lib->songs[5 + 3]);
    CHECK("no such song", !xst_song_by_id(lib, 0x00040010) && !xst_song_by_id(lib, 0x00000000)
                          && !xst_song_by_id(lib, 0x00050000));

    size = xst_db_size(lib);
    CHECK("db size: header, 100 soundtracks, 1+1+1+3 groups", size == 512 * (101 + 6));
    db = (uint8_t *)malloc(size);
    CHECK("too small a buffer", xst_db_build(lib, db, size - 1) == 0);
    CHECK("db build", xst_db_build(lib, db, size) == size);
    CHECK("header", rd32(db) == 1 && rd32(db + 4) == 4 && rd32(db + 12) == 1 && rd32(db + 24) == 4
                    && rd32(db + 28) == 0);
    CHECK("soundtrack block", rd32(db + 512 * 4) == 0x21371 && rd32(db + 512 * 4 + 8) == 14
                              && rd32(db + 512 * 4 + 348) == lib->st[3].ms
                              && db[512 * 4 + 352] == 'B' && db[512 * 4 + 353] == 0);
    CHECK("unused soundtrack blocks are empty", rd32(db + 512 * 5) == 0);
    for (i = 0; i < lib->n_st; i++) {
        int k;
        for (k = 0; k < lib->st[i].count; k++) {
            const xst_song *s = &lib->songs[lib->st[i].first + k];
            uint32_t id = 0, ms = 0;
            uint16_t name[32];
            int ok = db_song(db, size, lib->st[i].id, (uint32_t)k, &id, &ms, name);
            CHECK("song read back", ok && id == s->id && ms == s->ms
                                    && !memcmp(name, s->name, sizeof name));
            CHECK("song's folder is its soundtrack", (id >> 16) == lib->st[i].id);
        }
    }
    {
        uint32_t id, ms; uint16_t name[32];
        CHECK("index past the end", !db_song(db, size, 4, 14, &id, &ms, name));
        CHECK("unknown soundtrack", !db_song(db, size, 9, 0, &id, &ms, name));
    }
    free(db);
    xst_free(lib);

    CHECK("a folder that is not there", xst_scan(in_work("nothing here")) == NULL);
    lib = xst_scan(in_work("lib/empty"));
    CHECK("an empty folder", lib && lib->n_st == 0 && lib->n_songs == 0);
    xst_free(lib);
}

static void test_limits(void)
{
    xst_library *lib;
    char rel[64];
    int i;

    mkdir(in_work("many"), 0755);
    write_wav(in_work("many/1.wav"), 8000, 1, 440, 10);
    for (i = 2; i <= XST_MAX_SONGS + 3; i++) {
        snprintf(rel, sizeof rel, "many/%d.wav", i);
        copy_file(in_work("many/1.wav"), in_work(rel));
    }
    lib = xst_scan(in_work("many"));
    CHECK("500 songs kept", lib && lib->n_songs == XST_MAX_SONGS && lib->dropped_songs == 3);
    if (lib) {
        CHECK("the first 500 in order", name_is(lib->songs[499].name, "500"));
        CHECK("84 groups", xst_db_size(lib) == 512 * (101 + 84));
    }
    xst_free(lib);
}

static void test_decode(void)
{
    static int16_t pcm[XST_RATE * 2 * 2];
    const size_t cap = XST_RATE * 2;
    unsigned rate, ch;
    size_t n, i;
    int same = 1;

    /* 44.1 kHz stereo: the samples as they are. */
    write_wav(in_work("d441.wav"), 44100, 2, 1000, 500);
    n = decode_all(in_work("d441.wav"), pcm, cap, &rate, &ch);
    CHECK("44.1 kHz length", n == 22050 && rate == 44100 && ch == 2);
    for (i = 0; i < n; i++) {
        int16_t want = (int16_t)lrint(16384.0 * sin(2 * 3.14159265358979323846 * 1000 * i / 44100.0));
        if (abs(pcm[i * 2] - want) > 1 || pcm[i * 2] != pcm[i * 2 + 1])
            same = 0;
    }
    CHECK("44.1 kHz passes through", same);

    /* 48 kHz mono: resampled, both channels, the tone and its level kept. */
    write_wav(in_work("d48.wav"), 48000, 1, 1000, 1000);
    n = decode_all(in_work("d48.wav"), pcm, cap, &rate, &ch);
    CHECK("48 kHz length", n >= 44098 && n <= 44102 && rate == 48000 && ch == 1);
    CHECK("48 kHz: 1 kHz left", tone_share(pcm + 2000, n - 4000, 0, 1000) > 0.999);
    CHECK("48 kHz: 1 kHz right", tone_share(pcm + 2000, n - 4000, 1, 1000) > 0.999);
    CHECK("48 kHz level", fabs(rms_db(pcm + 2000, n - 4000) - (-9.03)) < 0.05);

    /* 22.05 kHz (upsampling) and 96 kHz (more than 2:1 down). */
    write_wav(in_work("d22.wav"), 22050, 2, 1000, 1000);
    n = decode_all(in_work("d22.wav"), pcm, cap, &rate, &ch);
    CHECK("22.05 kHz", n >= 44098 && n <= 44102 && tone_share(pcm + 2000, n - 4000, 0, 1000) > 0.999
                       && fabs(rms_db(pcm + 2000, n - 4000) - (-9.03)) < 0.05);
    write_wav(in_work("d96.wav"), 96000, 2, 1000, 1000);
    n = decode_all(in_work("d96.wav"), pcm, cap, &rate, &ch);
    CHECK("96 kHz", n >= 44098 && n <= 44102 && tone_share(pcm + 2000, n - 4000, 0, 1000) > 0.999
                    && fabs(rms_db(pcm + 2000, n - 4000) - (-9.03)) < 0.05);
    /* A tone above the output's Nyquist rate must not fold back. */
    write_wav(in_work("d96hi.wav"), 96000, 2, 30000, 1000);
    n = decode_all(in_work("d96hi.wav"), pcm, cap, &rate, &ch);
    CHECK("30 kHz at 96 kHz is filtered out", n > 44000 && rms_db(pcm + 2000, n - 4000) < -70.0);

    /* The encoded formats: the 1 kHz tone comes out, at its level. */
    fixture("tone_id3v23.mp3", "f.mp3");
    n = decode_all(in_work("f.mp3"), pcm, cap, &rate, &ch);
    /* LAME at 64 kbit/s scales its input down a little: -9.5 dB, not -9.0. */
    CHECK("mp3", n > 21000 && n < 25000 && rate == 44100
                 && tone_share(pcm + 4000, n - 8000, 0, 1000) > 0.99
                 && fabs(rms_db(pcm + 4000, n - 8000) - (-9.03)) < 0.6);
    fixture("tone48_id3v24.mp3", "f48.mp3");
    n = decode_all(in_work("f48.mp3"), pcm, cap, &rate, &ch);
    CHECK("mp3 48 kHz", n > 21000 && n < 25000 && rate == 48000
                        && tone_share(pcm + 4000, n - 8000, 1, 1000) > 0.99);
    fixture("tone.flac", "f.flac");
    n = decode_all(in_work("f.flac"), pcm, cap, &rate, &ch);
    CHECK("flac", n == 22050 && tone_share(pcm, n, 0, 1000) > 0.999
                  && fabs(rms_db(pcm, n) - (-9.03)) < 0.05);
    fixture("tone48.ogg", "f48.ogg");
    n = decode_all(in_work("f48.ogg"), pcm, cap, &rate, &ch);
    CHECK("ogg", n > 21900 && n < 22200 && rate == 48000
                 && tone_share(pcm + 4000, n - 8000, 0, 1000) > 0.99
                 && fabs(rms_db(pcm + 4000, n - 8000) - (-9.03)) < 0.3);

    {
        char title[64];
        uint32_t ms = 0;
        CHECK("probe mp3 v2.4", xst_probe(in_work("f48.mp3"), &ms, title, sizeof title)
                                && !strcmp(title, "Tone 48k") && ms > 400 && ms < 650);
        CHECK("probe a short title buffer", xst_probe(in_work("f.mp3"), &ms, title, 4)
                                            && strlen(title) <= 3);
        CHECK("probe a text file", !xst_probe(in_work("lib/notes.txt"), &ms, title, sizeof title));
        CHECK("open a text file", xst_stream_open(in_work("lib/notes.txt")) == NULL);
        CHECK("open a missing file", xst_stream_open(in_work("none.mp3")) == NULL);
        CHECK("open a broken flac", xst_stream_open(in_work("lib/broken.flac")) == NULL);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: soundtrack_test <work dir> <fixture dir>\n");
        return 2;
    }
    snprintf(g_work, sizeof g_work, "%s", argv[1]);
    snprintf(g_data, sizeof g_data, "%s", argv[2]);
    mkdir(g_work, 0755);
    test_names();
    test_scan_and_db();
    test_limits();
    test_decode();
    printf(failures ? "%d FAILED\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
