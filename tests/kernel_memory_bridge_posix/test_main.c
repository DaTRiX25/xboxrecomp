/*
 * Kernel memory and file bridges, through the path a title takes.
 *
 * Three bridges that were wrong in ways nothing caught:
 *
 *   NtFreeVirtualMemory (199)   passed a guest VA to the host as a PVOID * and
 *                               called VirtualFree on what it read there; it
 *                               failed every call (BLiNX 2: 51,215 in 4 min).
 *   MmFreeContiguousMemory (171) handed window addresses to the general heap,
 *                               which never matched them; the window was a
 *                               bump allocator and ran out at ~119 s.
 *   NtSetInformationFile (226)  had no FileRenameInformation, so a title that
 *                               writes x.tmp and renames it to x.ipk could
 *                               never open x.ipk.
 *
 * Each is driven by ordinal through recomp_lookup_kernel, as in
 * tests/kernel_bridge, but against a real guest window from
 * xbox_MemoryLayoutInit so the contiguous window at 0x80000000 exists.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G16(va) (*(volatile uint16_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G8(va)  (*(volatile uint8_t  *)((uintptr_t)(va) + g_xbox_mem_offset))
#define GP(va)  ((uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_MMALLOC = 165, ORD_MMALLOCEX = 166, ORD_MMFREE = 171,
       ORD_MMQUERYSIZE = 180, ORD_NTALLOC = 184, ORD_NTCLOSE = 187,
       ORD_NTCREATE = 190, ORD_NTFREE = 199, ORD_NTSETINFO = 226 };
static const uint32_t ORDS[] = { ORD_MMALLOC, ORD_MMALLOCEX, ORD_MMFREE,
    ORD_MMQUERYSIZE, ORD_NTALLOC, ORD_NTCLOSE, ORD_NTCREATE, ORD_NTFREE,
    ORD_NTSETINFO };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

static uint32_t g_thunks, g_stack, g_scratch;

/* stdcall: return address, then the arguments. */
static uint32_t call(uint32_t ordinal, int argc, const uint32_t *argv)
{
    unsigned i;
    recomp_func_t fn = NULL;

    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            fn = recomp_lookup_kernel(G32(g_thunks + 4 * i));
    if (!fn) {
        printf("  ordinal %u did not resolve\n", ordinal);
        failures++;
        return 0xFFFFFFFFu;
    }
    G32(g_stack) = 0xBEEF0001u;
    for (i = 0; i < (unsigned)argc; i++)
        G32(g_stack + 4 + 4 * i) = argv[i];
    g_esp = g_stack;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}
#define CALL(ord, ...) call((ord), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

/* ---- NtAllocateVirtualMemory / NtFreeVirtualMemory -------------------- */
static void test_virtual_memory(void)
{
    char d[160];
    uint32_t pbase = g_scratch, psize = g_scratch + 4, base, st, i;
    int intact = 1, zeroed = 1;

    printf("NtFreeVirtualMemory\n");
    G32(pbase) = 0;
    G32(psize) = 0x10000;
    st = CALL(ORD_NTALLOC, pbase, 0, psize, 0x3000 /* RESERVE|COMMIT */, 4);
    base = G32(pbase);
    check(st == 0 && base, "allocate 64 KB", NULL);
    memset(GP(base), 0xAB, 0x10000);

    /* Decommit one page in the middle, unaligned base, as RtlHeap does. */
    G32(pbase) = base + 0x1010;
    G32(psize) = 0x10;
    st = CALL(ORD_NTFREE, pbase, psize, 0x4000 /* MEM_DECOMMIT */);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "MEM_DECOMMIT succeeds (guest pointers read as guest)", d);
    snprintf(d, sizeof d, "base 0x%08X size 0x%X", G32(pbase), G32(psize));
    check(G32(pbase) == base + 0x1000 && G32(psize) == 0x1000,
          "rounded base and size written back to the guest", d);
    for (i = 0; i < 0x1000; i++) zeroed &= GP(base + 0x1000)[i] == 0;
    intact = GP(base + 0xFFF)[0] == 0xAB && GP(base + 0x2000)[0] == 0xAB;
    check(zeroed, "decommitted page reads back as zero", NULL);
    check(intact, "neighbouring pages untouched", NULL);

    /* Release: size must be 0; the whole block goes back to the heap. */
    G32(pbase) = base;
    G32(psize) = 0x1000;
    st = CALL(ORD_NTFREE, pbase, psize, 0x8000);
    check(st != 0, "MEM_RELEASE with a nonzero size is refused", NULL);
    G32(psize) = 0;
    st = CALL(ORD_NTFREE, pbase, psize, 0x8000 /* MEM_RELEASE */);
    snprintf(d, sizeof d, "status 0x%08X size 0x%X", st, G32(psize));
    check(st == 0 && G32(psize) == 0x10000, "MEM_RELEASE frees the region", d);
    G32(pbase) = 0;
    G32(psize) = 0x10000;
    CALL(ORD_NTALLOC, pbase, 0, psize, 0x3000, 4);
    snprintf(d, sizeof d, "got 0x%08X, released 0x%08X", G32(pbase), base);
    check(G32(pbase) == base, "released region is reused", d);

    G32(pbase) = 0x00001000;
    G32(psize) = 0;
    st = CALL(ORD_NTFREE, pbase, psize, 0x8000);
    check(st == 0xC00000A0u, "free below the heap is refused, not zeroed", NULL);
}

/* ---- contiguous arena -------------------------------------------------- */
static void test_contiguous(void)
{
    char d[160];
    uint32_t a, b, c, e, big, i, ok = 1;

    printf("contiguous arena\n");
    a = CALL(ORD_MMALLOC, 0x2000);
    b = CALL(ORD_MMALLOC, 0x1000);
    snprintf(d, sizeof d, "a=0x%08X b=0x%08X", a, b);
    check(a >= XBOX_CONTIG_BASE && b >= XBOX_CONTIG_BASE && a != b,
          "allocations come from the window", d);
    snprintf(d, sizeof d, "got %u", CALL(ORD_MMQUERYSIZE, a));
    check(CALL(ORD_MMQUERYSIZE, a) == 0x2000,
          "MmQueryAllocationSize answers for a contiguous block", d);
    check(CALL(ORD_MMQUERYSIZE, a + 0x1800) == 0x800,
          "MmQueryAllocationSize on an interior address", NULL);
    CALL(ORD_MMFREE, a);
    check(CALL(ORD_MMQUERYSIZE, a) == 0, "freed block reports size 0", NULL);
    c = CALL(ORD_MMALLOC, 0x1000);
    snprintf(d, sizeof d, "c=0x%08X a=0x%08X", c, a);
    check(c == a, "freed pages are reused", d);

    /* MmAllocateContiguousMemoryEx: range and alignment honoured. */
    e = CALL(ORD_MMALLOCEX, 0x3000, 0x01000000, 0x01FFFFFF, 0x10000, 4);
    snprintf(d, sizeof d, "e=0x%08X", e);
    check(e && (e - XBOX_CONTIG_BASE) >= 0x01000000
            && (e - XBOX_CONTIG_BASE) + 0x3000 - 1 <= 0x01FFFFFF
            && (e & 0xFFFF) == 0,
          "Ex honours lowest/highest and alignment", d);
    check(CALL(ORD_MMALLOCEX, 0x1000, 0x5000000, 0x4000000, 0, 4) == 0,
          "Ex with highest < lowest fails", NULL);
    CALL(ORD_MMFREE, e);
    CALL(ORD_MMFREE, b);
    CALL(ORD_MMFREE, c);

    /* Churn: 40 MB allocated and freed eight times would exhaust a bump
     * allocator over a 64 MB window on the second pass. */
    for (i = 0; i < 8 && ok; i++) {
        big = CALL(ORD_MMALLOC, 40u << 20);
        ok = big != 0;
        if (ok) CALL(ORD_MMFREE, big);
    }
    check(ok, "allocate/free 40 MB eight times without exhausting", NULL);
    snprintf(d, sizeof d, "%u bytes still in use",
             xbox_ContiguousInUseBytes());
    check(xbox_ContiguousInUseBytes() == 0, "everything freed is released", d);
}

/* ---- FileRenameInformation -------------------------------------------- */
static uint32_t put_str(uint32_t at, const char *s)
{
    /* ANSI_STRING at `at`, text right after it. */
    uint16_t n = (uint16_t)strlen(s);
    memcpy(GP(at + 8), s, n + 1);
    G16(at) = n;
    G16(at + 2) = (uint16_t)(n + 1);
    G32(at + 4) = at + 8;
    return at;
}

static uint32_t create(const char *path)
{
    uint32_t oa = g_scratch + 0x100, str = g_scratch + 0x200;
    uint32_t ph = g_scratch + 0x10, ios = g_scratch + 0x18, st;

    put_str(str, path);
    G32(oa) = 0; G32(oa + 4) = str; G32(oa + 8) = 0x40;
    st = CALL(ORD_NTCREATE, ph, 0xC0110000u /* R|W|DELETE|SYNC */, oa, ios, 0,
              0x80, 0, 5 /* OVERWRITE_IF */, 0x60);
    return st ? 0 : G32(ph);
}

static uint32_t rename_to(uint32_t h, const char *name, int replace)
{
    uint32_t info = g_scratch + 0x400, ios = g_scratch + 0x18;
    uint16_t n = (uint16_t)strlen(name);

    memset(GP(info), 0, 16);
    G8(info) = (uint8_t)replace;
    G32(info + 4) = 0;
    G16(info + 8) = n;
    G16(info + 10) = (uint16_t)(n + 1);
    G32(info + 12) = info + 16;
    memcpy(GP(info + 16), name, n + 1);
    return CALL(ORD_NTSETINFO, h, ios, info, 16, 10);
}

static int exists(const char *dir, const char *name)
{
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return stat(p, &st) == 0;
}

static void test_rename(const char *cache)
{
    char d[160];
    uint32_t h, h2, st;

    printf("NtSetInformationFile(FileRenameInformation)\n");
    h = create("z:\\a.tmp");
    check(h != 0 && exists(cache, "a.tmp"), "create z:\\a.tmp", NULL);
    st = rename_to(h, "z:\\a.ipk", 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "rename to a full path succeeds", d);
    check(exists(cache, "a.ipk") && !exists(cache, "a.tmp"),
          "a.tmp became a.ipk on the host", NULL);
    CALL(ORD_NTCLOSE, h);

    h2 = create("z:\\b.tmp");
    st = rename_to(h2, "z:\\a.ipk", 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0xC0000035u, "existing target without ReplaceIfExists collides", d);
    st = rename_to(h2, "z:\\a.ipk", 1);
    check(st == 0 && !exists(cache, "b.tmp"), "ReplaceIfExists replaces", NULL);
    st = rename_to(h2, "c.ipk", 0);
    check(st == 0 && exists(cache, "c.ipk") && !exists(cache, "a.ipk"),
          "bare name renames within the same directory", NULL);
    CALL(ORD_NTCLOSE, h2);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/kmembridgeXXXXXX", cache[256], cmd[300];
    size_t n = 0;
    unsigned char *xbe;
    unsigned i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(tmpl)) return 2;
    setenv("XDG_DATA_HOME", tmpl, 1);
    snprintf(cache, sizeof cache, "%s/xboxrecomp/Cache", tmpl);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", cache);
    if (system(cmd) != 0) return 2;

    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &n);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, n)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }

    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_scratch = xbox_HeapAlloc(0x1000, 4096);
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();

    test_virtual_memory();
    test_contiguous();
    test_rename(cache);

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
