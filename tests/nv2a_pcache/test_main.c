/*
 * nv2a_pcache test: the key list a GPU backend pre-warms from.
 * Usage: nv2a_pcache_test <scratch dir>
 */
#include "nv2a_pcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", what); s_fail++; } \
                            else fprintf(stderr, "ok: %s\n", what); } while (0)

static long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    long n;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static unsigned s_scan_n, s_scan_slots;
static void scan_cb(void *ctx, const uint32_t *slots, uint32_t n)
{
    (void)ctx;
    s_scan_n++;
    s_scan_slots = n + (slots[11] == 1 ? 0 : 100);     /* the third slot's FINAL word */
}

int main(int argc, char **argv)
{
    char dir[1024], path[1100], msg[256];
    uint32_t a[4] = {1, 2, 3, 4}, b[2] = {5, 6}, type, len;
    unsigned char big[300];
    const void *d;
    nv2a_pcache *pc;
    long size2;
    FILE *f;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    CHECK(!nv2a_pcache_dir("none", "x", dir, sizeof dir), "render.shader_cache=none is off");
    CHECK(!nv2a_pcache_dir("", "x", dir, sizeof dir), "an empty directory is off");
    CHECK(nv2a_pcache_dir(argv[1], "x", dir, sizeof dir) && !strcmp(dir, argv[1]),
          "a directory is created and used as given");
    snprintf(path, sizeof path, "%s/keys.bin", dir);
    remove(path);

    pc = nv2a_pcache_open(path, "v1", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 0 && strstr(msg, "new"), "a new file");
    memset(big, 0xAB, sizeof big);
    CHECK(nv2a_pcache_put(pc, 0, a, sizeof a) == 1, "put a");
    CHECK(nv2a_pcache_put(pc, 1, b, sizeof b) == 1, "put b");
    CHECK(nv2a_pcache_put(pc, 0, a, sizeof a) == 0, "a again is not written");
    CHECK(nv2a_pcache_put(pc, 1, a, sizeof a) == 1, "the same bytes as another type are");
    CHECK(nv2a_pcache_put(pc, 2, big, sizeof big) == 1, "put big");
    nv2a_pcache_close(pc);

    pc = nv2a_pcache_open(path, "v1", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 4, "four records load back");
    CHECK(nv2a_pcache_get(pc, 0, &type, &d, &len) && type == 0 && len == sizeof a
          && !memcmp(d, a, sizeof a), "record 0 is a");
    CHECK(nv2a_pcache_get(pc, 3, &type, &d, &len) && type == 2 && len == sizeof big
          && !memcmp(d, big, sizeof big), "record 3 is big");
    CHECK(!nv2a_pcache_get(pc, 4, &type, &d, &len), "no record 4");
    CHECK(nv2a_pcache_put(pc, 1, b, sizeof b) == 0, "a loaded record is not written again");
    nv2a_pcache_close(pc);
    size2 = file_size(path);

    /* A torn append: half a record at the end. */
    f = fopen(path, "ab");
    fwrite("\x00\x00\x00\x00\x10\x00\x00\x00garbage", 1, 15, f);
    fclose(f);
    pc = nv2a_pcache_open(path, "v1", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 4 && strstr(msg, "cut"), "a torn tail is cut");
    nv2a_pcache_close(pc);
    CHECK(file_size(path) == size2, "the file is back to its good length");

    /* A flipped byte in the last record's payload: it and what follows go. */
    f = fopen(path, "r+b");
    fseek(f, -5, SEEK_END);
    fputc(0x00, f);
    fclose(f);
    pc = nv2a_pcache_open(path, "v1", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 3, "a damaged record is dropped");
    nv2a_pcache_close(pc);

    /* Another version: started afresh. */
    pc = nv2a_pcache_open(path, "v2", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 0 && strstr(msg, "another version"),
          "another version starts afresh");
    nv2a_pcache_close(pc);

    /* A list from before BO-93 carries the toolkit commit after the key
     * format: the same keys, read as they are and appended to. */
    remove(path);
    pc = nv2a_pcache_open(path, "keys 1; K 40; toolkit v0.12.0-523-ge273044a4c6e-dirty", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_put(pc, 0, a, sizeof a) == 1, "a list named after a toolkit commit");
    nv2a_pcache_close(pc);
    pc = nv2a_pcache_open(path, "keys 1; K 40", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 1 && strstr(msg, "loaded"),
          "is read by the same key format without the commit");
    CHECK(nv2a_pcache_put(pc, 1, b, sizeof b) == 1, "and appended to");
    nv2a_pcache_close(pc);
    pc = nv2a_pcache_open(path, "keys 1; K 40", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 2, "both records load back");
    nv2a_pcache_close(pc);
    pc = nv2a_pcache_open(path, "keys 1; K 4", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 0 && strstr(msg, "another version"),
          "a prefix of another format is not that format");
    nv2a_pcache_close(pc);

    /* Vertex programs in an executable image: the XDK's blob (0x2078, the
     * slot count, the slots; FINAL on the last slot and only there). */
    {
        unsigned char img[4 + 3 * 16 + 8 + 4 + 2 * 16];
        uint32_t w[12] = {0, 1, 2, 0,  0, 3, 4, 0,  0, 5, 6, 1}, bad[8] = {0, 1, 2, 1,  0, 3, 4, 1};
        s_scan_n = s_scan_slots = 0;
        memset(img, 0xEE, sizeof img);
        img[0] = 0x78; img[1] = 0x20; img[2] = 3; img[3] = 0;
        memcpy(img + 4, w, sizeof w);
        img[60] = 0x78; img[61] = 0x20; img[62] = 2; img[63] = 0;  /* FINAL on both slots */
        memcpy(img + 64, bad, sizeof bad);
        CHECK(nv2a_pcache_scan_vsh(img, sizeof img, scan_cb, NULL) == 1 && s_scan_n == 1
              && s_scan_slots == 3, "one vertex program is found in an image, a malformed one is not");
    }

    /* Not a cache file at all. */
    f = fopen(path, "wb");
    fputs("hello", f);
    fclose(f);
    pc = nv2a_pcache_open(path, "v2", msg, sizeof msg);
    CHECK(pc && nv2a_pcache_count(pc) == 0 && strstr(msg, "not a cache"), "a foreign file is replaced");
    nv2a_pcache_close(pc);

    remove(path);
    fprintf(stderr, "%s\n", s_fail ? "FAILED" : "all passed");
    return s_fail ? 1 : 0;
}
