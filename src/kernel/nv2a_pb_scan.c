/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 *
 * The walk goes from GET to PUT the way the DMA engine does: a jump moves
 * GET to its target (new style w & ~3, old style w & 0x1FFFFFFC), a call
 * does the same and keeps one return address, and a return goes back to it.
 * So a ring wrap is the title's own jump back to the start, and a segment
 * with a jump in it goes on after it to PUT.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

#include "nv2a_pb_state.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
extern void nv2a_pb_perf_scan_begin(void);
extern void nv2a_pb_perf_scan_end(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;   /* RECOMP_PB_SCAN, read once */

/* s_seen's slot for each (subchannel, method), plus one (0: not seen yet).
 * The walker calls note() for every method in the stream, ~100,000 a frame in
 * BLiNX 2's attract demo, and a linear search of a few hundred pairs there was
 * half the walker's time. s_seen keeps first-seen order for the report. */
static uint16_t s_seen_slot[8][0x2000 / 4];

static void note(uint32_t subch, uint32_t method)
{
    static uint16_t none;
    uint16_t *slot = &none;

    /* An incrementing run can carry the method past 0x1FFC; those (rare)
     * keep the linear search. */
    if (method < 0x2000 && !(method & 3)) {
        slot = &s_seen_slot[subch & 7][method >> 2];
        if (*slot) {
            s_seen[*slot - 1].count++;
            return;
        }
    } else {
        for (int i = 0; i < s_seen_count; i++) {
            if (s_seen[i].method == method && s_seen[i].subch == subch) {
                s_seen[i].count++;
                return;
            }
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        if (slot != &none)
            *slot = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0110, "WAIT_FOR_IDLE" },
    { 0x0120, "SET_FLIP_READ" },
    { 0x0124, "SET_FLIP_WRITE" },
    { 0x0128, "SET_FLIP_MODULO" },
    { 0x012C, "FLIP_INCREMENT_WRITE" },
    { 0x0130, "FLIP_STALL" },
    { 0x01A4, "SET_CONTEXT_DMA_SEMAPHORE" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_ZSTENCIL_CLEAR_VALUE" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_SEMAPHORE_OFFSET" },
    { 0x1D70, "BACK_END_WRITE_SEMAPHORE_RELEASE" },
    { 0x1D98, "SET_CLEAR_RECT_HORIZONTAL" },
    { 0x1D9C, "SET_CLEAR_RECT_VERTICAL" },
    { 0x0B00, "SET_TRANSFORM_PROGRAM" },
    { 0x0B80, "SET_TRANSFORM_CONSTANT" },
    { 0x1EA4, "SET_TRANSFORM_CONSTANT_LOAD" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "ARRAY_ELEMENT32" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1818, "INLINE_ARRAY" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B10, "SET_TEXTURE_CONTROL1" },
    { 0x1B14, "SET_TEXTURE_FILTER" },
    { 0x1B1C, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Once per pass of the ack thread's loop: the backend's on_poll, so a
 * deferred visibility-test report can complete while no pushbuffer comes
 * (a title spinning on GetVisibilityTestResult submits nothing). */
void nv2a_pb_poll(void)
{
    const struct nv2a_pb_backend *b;
    if (s_exec_enabled <= 0)
        return;
    b = nv2a_pb_get_backend();
    if (b && b->on_poll)
        b->on_poll();
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va, base = start_va & 0xF0000000u, ret_va = 0;
    uint32_t words = 0, jumps = 0, unknown = 0;
    int in_call = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(s_scan_enabled || s_exec_enabled) || end_va == start_va)
        return;
    if (s_exec_enabled)
        nv2a_pb_perf_scan_begin();

    /* Bounded at 4 MB of words, a sane single-frame amount: GET that never
     * reaches PUT is a stream this walk is out of step with. */
    while (va != end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            jumps++;
            va = base | (((w & 3u) == 1u ? w & ~3u : w & 0x1FFFFFFCu) & 0x0FFFFFFFu);
            continue;
        }
        if ((w & 3u) == 2u) {                 /* call */
            jumps++;
            ret_va = va;
            in_call = 1;
            va = base | (w & 0x0FFFFFFCu);
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {   /* return */
            if (in_call)
                va = ret_va;
            in_call = 0;
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va != end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        unknown++;
    }
    if (va != end_va) {
        static unsigned said;
        if (said++ < 8)
            fprintf(stderr, "[PB] walk 0x%08X -> 0x%08X stopped at 0x%08X after"
                    " %u words without reaching PUT\n", start_va, end_va, va, words);
    }

    if (s_exec_enabled)
        nv2a_pb_perf_scan_end();
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}
