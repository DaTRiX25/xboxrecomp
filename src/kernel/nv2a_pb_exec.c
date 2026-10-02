/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_cpu.h"
#include "nv2a_pb_state.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  D3DCOLOR */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
#define IMM_VERTEX_DWORDS 7

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* GPU completion fences. D3D names a semaphore DMA object once, points the
 * semaphore at a word inside it once, and from then on asks the back end to
 * write the fence value it has just submitted -- BLiNX 2 sends the first two
 * once and the release ~3 times a frame. The title's fence wait spins on that
 * word, so writing it HERE, when the executor reaches it in the stream, is
 * what makes the fence mean "the GPU got this far". The old answer, mirroring
 * "submitted" into "completed" from the ack thread (xbox_Nv2aMirrorFence),
 * said the work was done before anything had read it. */
#define NV097_SET_CONTEXT_DMA_SEMAPHORE   0x01A4
#define NV097_SET_SEMAPHORE_OFFSET        0x1D6C
#define NV097_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70

/* VertexAttr, Texture and the decoded state's layout: nv2a_pb_state.h. */
static struct nv2a_pb_gpu s_gpu;

/* Releases written, and where the last one landed. fence_mirrors_tick reads
 * them to decide whether its mirror is still needed; both run on the NV2A ack
 * thread, which is also the thread that drives this executor. */
static uint32_t s_sema_releases;
static uint32_t s_sema_last_va, s_sema_last_value;

uint32_t nv2a_pb_exec_semaphore_releases(void) { return s_sema_releases; }
uint32_t nv2a_pb_exec_semaphore_last_va(void) { return s_sema_last_va; }

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
static uint32_t s_tex_reg[NV_TEX_REGS];
static uint8_t  s_tex_set[NV_TEX_REGS];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    s_gpu.tex.valid = s_gpu.tex.offset && s_gpu.tex.width && s_gpu.tex.height
                   && (tex_size_from_format(s_gpu.tex.color)
                       || s_gpu.tex.pitch);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.tex.valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.tex.offset
         && s_tex_use[i].color  == s_gpu.tex.color) {
            s_tex_use[i].batches++;
            /* Dump a surface that is redrawn, every Nth time it is bound.
             *
             * First use alone cannot tell a decode error that is wrong in
             * every frame from one that accumulates across them. A block
             * transform that is wrong is wrong on its own, in the keyframe
             * as much as anywhere; motion compensation that is wrong starts
             * from a clean keyframe and smears further with each predicted
             * frame after it. In a single frame the two look identical, and
             * in a sequence they look nothing alike -- so the sequence is
             * what has to be captured.
             *
             * It belongs on this side of the return: a video surface keeps
             * one address for the whole film, so after the first frame it is
             * only ever found here, and the first-use dump below never fires
             * for it again. RECOMP_TEX_DUMP_EVERY=<n> sets the interval, and
             * RECOMP_TEX_DUMP still names the files. */
            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = getenv("RECOMP_TEX_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.tex.offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.tex.color;
        s_tex_use[s_tex_use_count].width   = s_gpu.tex.width;
        s_tex_use[s_tex_use_count].height  = s_gpu.tex.height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

static void note_unhandled(uint32_t method, uint32_t param)
{
    int i;

    s_gpu.unhandled_total++;
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            s_unhandled[i].last_param = param;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
        return 0;
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = mem + a->offset + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* The packed types 3D models use: positions and normals in shorts, or a
     * normal squeezed into one dword. Decoded the way xemu hands them to GL. */
    case 1:                                  /* S1: signed short, normalised */
        for (i = 0; i < a->size && i < 4; i++) {
            float f = (float)((const int16_t *)p)[i] / 32767.0f;
            out[i] = f < -1.0f ? -1.0f : f;
        }
        return 1;
    case 5:                                  /* S32K: signed short, as is */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* CMP: 11:11:10 signed, normalised */
        uint32_t w = *(const uint32_t *)p;
        int32_t x = (int32_t)(w << 21) >> 21;
        int32_t y = (int32_t)(w << 10) >> 21;
        int32_t z = (int32_t)w >> 22;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void)
{
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    static int seq;
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(s_gpu.drawn_offset
                                                ? s_gpu.drawn_offset
                                                : s_gpu.color_offset)
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* A colour clear is drawing too. drawn_offset is what a flip presents, and
     * only the triangle rasteriser used to set it, so a frame that was a clear
     * plus non-triangle work left it on the last surface a triangle touched.
     * BLiNX 2 under Proton: every flip republished 0x01034000 while the
     * executor was clearing and drawing 0x01160000, and the window was black. */
    s_gpu.drawn_offset = s_gpu.color_offset;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    if (getenv("RECOMP_RASTER_TEST")) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (s_gpu.flips == 0)
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return c % size;
    return c >= size ? size - 1 : c;       /* clamp, and everything else */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

static int sample_tex(const Texture *t, uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt;

    if (!t->valid)
        return 0;
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt))
        return d3d8_dxt_decode_texel(mem + t->offset, fmt, u, v,
                                     t->width, argb);
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + t->offset;
        u = swizzle_offset(u, v, t->width, t->height);
    } else {
        p = mem + t->offset + (size_t)v * t->pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&s_gpu.tex, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.tex.width, h = s_gpu.tex.height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.tex.offset, s_gpu.tex.color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.tex.color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);

    if (surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}

static int blend_known(uint32_t f)
{
    return f == 0 || f == 1 || (f >= 0x300 && f <= 0x307);
}

/* A GL blend factor for the channel at bit `sh` of an ARGB word, 0..255. */
static uint32_t blend_factor(uint32_t f, uint32_t src, uint32_t dst, uint32_t sh)
{
    switch (f) {
    case 0:     return 0;
    case 1:     return 255;
    case 0x300: return (src >> sh) & 0xFF;                 /* SRC_COLOR */
    case 0x301: return 255 - ((src >> sh) & 0xFF);
    case 0x302: return src >> 24;                          /* SRC_ALPHA */
    case 0x303: return 255 - (src >> 24);
    case 0x304: return dst >> 24;                          /* DST_ALPHA */
    case 0x305: return 255 - (dst >> 24);
    case 0x306: return (dst >> sh) & 0xFF;                 /* DST_COLOR */
    default:    return 255 - ((dst >> sh) & 0xFF);         /* 0x307 */
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    s_gpu.pixels++;
    if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
        s_gpu.pixel_max = argb;
    row = s_surface + (size_t)y * s_gpu.pitch;

    /* src*srcAlpha + dst*(1-srcAlpha) first, on its own path, because it was
     * the only pair here for a long time and the intro was checked against
     * exactly this arithmetic. The other GL factor pairs follow it.
     *
     * Fully opaque is left alone deliberately. It is the same arithmetic,
     * but skipping it keeps the full-screen quads -- which are drawn with
     * blending enabled and alpha 255 -- on exactly the path they were on
     * before, so this cannot change what they produce. */
    if (s_gpu.blend_enable
        && s_gpu.blend_sfactor == NV_BLEND_SRC_ALPHA
        && s_gpu.blend_dfactor == NV_BLEND_ONE_MINUS_SRC_ALPHA
        && (argb >> 24) != 0xFF) {
        uint32_t sa = argb >> 24;
        uint32_t dst = 0;
        if (sa == 0)
            return;                        /* nothing of the source survives */
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[x];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[x];
            dst = (((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                 | ((t & 0x001Fu) << 3));
        }
        {
            uint32_t r = (((argb >> 16) & 0xFF) * sa
                        + ((dst >> 16) & 0xFF) * (255u - sa) + 127u) / 255u;
            uint32_t g = (((argb >>  8) & 0xFF) * sa
                        + ((dst >>  8) & 0xFF) * (255u - sa) + 127u) / 255u;
            uint32_t b = (((argb      ) & 0xFF) * sa
                        + ((dst      ) & 0xFF) * (255u - sa) + 127u) / 255u;
            argb = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    } else if (s_gpu.blend_enable
               && !(s_gpu.blend_sfactor == NV_BLEND_SRC_ALPHA
                    && s_gpu.blend_dfactor == NV_BLEND_ONE_MINUS_SRC_ALPHA)
               && blend_known(s_gpu.blend_sfactor)
               && blend_known(s_gpu.blend_dfactor)) {
        /* Every other GL factor pair, with the ADD equation. Additive glows
         * (SRC_ALPHA, ONE) are what the title's loading screen uses, and an
         * opaque write turns each one into a black square. */
        uint32_t dst = 0, out = 0, sh;
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[x];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[x];
            dst = 0xFF000000u | (((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                 | ((t & 0x001Fu) << 3));
        }
        for (sh = 0; sh <= 24; sh += 8) {
            uint32_t sc = (argb >> sh) & 0xFF, dc = (dst >> sh) & 0xFF;
            uint32_t v = (sc * blend_factor(s_gpu.blend_sfactor, argb, dst, sh)
                        + dc * blend_factor(s_gpu.blend_dfactor, argb, dst, sh)
                        + 127u) / 255u;
            out |= (v > 255u ? 255u : v) << sh;
        }
        argb = out;
    }

    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.tex.valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.tex.color)) {
        out[0] *= (float)s_gpu.tex.width;
        out[1] *= (float)s_gpu.tex.height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV_PRIM_*: nv2a_pb_state.h. */

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured;

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

/* ── Vertex programs ─────────────────────────────────────────────────────
 *
 * The title-stage geometry is drawn through NV2A vertex programs: attribute 0
 * is object space and only the program knows where it lands. So the executor
 * keeps the transform unit's state -- 136 program slots, 192 constants, the
 * execution mode and the inline attribute values -- and runs the program per
 * vertex on the CPU (nv2a_vsh_cpu.c). XDK D3D appends a viewport epilogue to
 * every program, so oPos arrives already in surface pixels with the clip-space
 * w beside it, which is everything the rasteriser needs.
 *
 * A batch takes this path when SET_TRANSFORM_EXECUTION_MODE says PROGRAM.
 * Fixed-function mode keeps the old screen-space path: this title never sends
 * a fixed-function matrix (no SET_MODEL_VIEW/COMPOSITE_MATRIX in its stream),
 * so there is nothing to synthesise yet, and the report says if that changes.
 *
 * RECOMP_PB_VSH=0 turns the path off, which is the A/B switch for comparing
 * dumps against the old executor.
 *
 * Texture stages 0..3 and the register combiners are evaluated per pixel (see
 * rc_eval); RECOMP_PB_RC=0 falls back to stage 0 modulated by oD0.
 *
 * ponytail: triangles with any w <= 0 are dropped rather than clipped
 * against the near plane. Depth is a private float buffer, not the title's
 * zeta surface, which nothing else here reads.
 */
#define NV097_SET_TRANSFORM_PROGRAM_M        0x0B00   /* +0..0x7C */
#define NV097_SET_TRANSFORM_CONSTANT_M       0x0B80   /* +0..0x7C */
#define NV097_SET_TRANSFORM_EXECUTION_MODE_M 0x1E94
#define NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN_M 0x1E98
#define NV097_SET_TRANSFORM_PROGRAM_LOAD_M   0x1E9C
#define NV097_SET_TRANSFORM_PROGRAM_START_M  0x1EA0
#define NV097_SET_TRANSFORM_CONSTANT_LOAD_M  0x1EA4
#define NV097_SET_VIEWPORT_OFFSET_M          0x0A20
#define NV097_SET_VIEWPORT_SCALE_M           0x0AF0
#define NV097_SET_MODEL_VIEW_MATRIX_M        0x0480
#define NV097_SET_COMPOSITE_MATRIX_M         0x0680
#define NV097_SET_PROJECTION_MATRIX_M        0x0440
#define NV097_SET_CULL_FACE_ENABLE_M         0x0308
#define NV097_SET_DEPTH_TEST_ENABLE_M        0x030C
#define NV097_SET_DEPTH_FUNC_M               0x0354
#define NV097_SET_DEPTH_MASK_M               0x035C
#define NV097_SET_CULL_FACE_M                0x039C
#define NV097_SET_FRONT_FACE_M               0x03A0
#define NV097_SET_SURFACE_ZETA_OFFSET_M      0x0214
#define NV097_SET_ZSTENCIL_CLEAR_VALUE_M     0x1D8C
#define NV097_SET_VERTEX_DATA2S_M            0x1900   /* + attr*4, 2 shorts */
#define NV097_SET_VERTEX_DATA4S_M_M          0x1980   /* + attr*8, 4 shorts */

#define NV097_SET_COMBINER_ALPHA_ICW_M      0x0260   /* +stage*4 */
#define NV097_SET_COMBINER_SPECULAR_FOG_CW0_M 0x0288
#define NV097_SET_COMBINER_SPECULAR_FOG_CW1_M 0x028C
#define NV097_SET_FOG_ENABLE_M              0x02A4
#define NV097_SET_FOG_COLOR_M               0x02A8
#define NV097_SET_ALPHA_TEST_ENABLE_M       0x0300
#define NV097_SET_ALPHA_FUNC_M              0x033C
#define NV097_SET_ALPHA_REF_M               0x0340
#define NV097_SET_COMBINER_FACTOR0_M        0x0A60
#define NV097_SET_COMBINER_FACTOR1_M        0x0A80
#define NV097_SET_COMBINER_ALPHA_OCW_M      0x0AA0
#define NV097_SET_COMBINER_COLOR_ICW_M      0x0AC0
#define NV097_SET_SPECULAR_FOG_FACTOR_M     0x1E20   /* +0, +4 */
#define NV097_SET_COMBINER_COLOR_OCW_M      0x1E40
#define NV097_SET_COMBINER_CONTROL_M        0x1E60
#define NV097_SET_SHADER_STAGE_PROGRAM_M    0x1E70
#define NV097_SET_POLY_OFFSET_FILL_ENABLE_M 0x0338
#define NV097_SET_POLYGON_OFFSET_SCALE_FACTOR_M 0x0384
#define NV097_SET_POLYGON_OFFSET_BIAS_M     0x0388

/* XfVert and the s_vsh layout: nv2a_pb_state.h. */
static struct nv2a_pb_vsh s_vsh;

/* Distinct programs, by hash of the slots from START to FINAL. */
#define VSH_MAX_PROGS 64
static struct { uint32_t hash, start, len, batches; } s_vsh_progs[VSH_MAX_PROGS];
static int s_vsh_prog_count;

static uint64_t s_vsh_ab_batches, s_vsh_ab_diff_batches, s_vsh_ab_diff_px;
static float s_vsh_last_d0[4];
static XfVert s_vsh_last_v0;
static float *s_zbuf;
static uint32_t s_zbuf_w, s_zbuf_h;

static float u2f(uint32_t u) { union { uint32_t u; float f; } v; v.u = u; return v.f; }

static int vsh_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PB_VSH");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* The transform-unit methods. Returns 1 if `method` was one of them. */
static int vsh_method(uint32_t method, uint32_t param)
{
    if (method >= NV097_SET_TRANSFORM_PROGRAM_M
            && method < NV097_SET_TRANSFORM_PROGRAM_M + 0x80) {
        uint32_t comp = ((method - NV097_SET_TRANSFORM_PROGRAM_M) / 4) & 3;
        if (s_vsh.load < NV2A_VSH_SLOTS)
            s_vsh.prog[s_vsh.load][comp] = param;
        if (comp == 3)
            s_vsh.load++;
        s_vsh.prog_dwords++;
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT_M
            && method < NV097_SET_TRANSFORM_CONSTANT_M + 0x80) {
        uint32_t comp = ((method - NV097_SET_TRANSFORM_CONSTANT_M) / 4) & 3;
        if (s_vsh.cload < NV2A_VSH_CONSTANTS)
            s_vsh.c[s_vsh.cload][comp] = u2f(param);
        if (comp == 3)
            s_vsh.cload++;
        s_vsh.const_dwords++;
        return 1;
    }
    if (method >= NV097_SET_VIEWPORT_OFFSET_M && method < NV097_SET_VIEWPORT_OFFSET_M + 16) {
        s_vsh.vp_off[(method - NV097_SET_VIEWPORT_OFFSET_M) / 4] = u2f(param);
        s_vsh.vp_dwords++;
        return 1;
    }
    if (method >= NV097_SET_VIEWPORT_SCALE_M && method < NV097_SET_VIEWPORT_SCALE_M + 16) {
        s_vsh.vp_scale[(method - NV097_SET_VIEWPORT_SCALE_M) / 4] = u2f(param);
        s_vsh.vp_dwords++;
        return 1;
    }
    /* Fixed-function matrices: counted, not used -- see the block comment. */
    if ((method >= NV097_SET_PROJECTION_MATRIX_M && method < NV097_SET_PROJECTION_MATRIX_M + 0x40)
     || (method >= NV097_SET_MODEL_VIEW_MATRIX_M && method < NV097_SET_MODEL_VIEW_MATRIX_M + 0x100)
     || (method >= NV097_SET_COMPOSITE_MATRIX_M && method < NV097_SET_COMPOSITE_MATRIX_M + 0x40)) {
        s_vsh.ff_matrix_dwords++;
        note_unhandled(method, param);
        return 1;
    }
    if (method >= NV097_SET_COMBINER_ALPHA_ICW_M && method < NV097_SET_COMBINER_ALPHA_ICW_M + 32) {
        s_vsh.rc_aicw[(method - NV097_SET_COMBINER_ALPHA_ICW_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_FACTOR0_M && method < NV097_SET_COMBINER_FACTOR0_M + 32) {
        s_vsh.rc_f0[(method - NV097_SET_COMBINER_FACTOR0_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_FACTOR1_M && method < NV097_SET_COMBINER_FACTOR1_M + 32) {
        s_vsh.rc_f1[(method - NV097_SET_COMBINER_FACTOR1_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_ALPHA_OCW_M && method < NV097_SET_COMBINER_ALPHA_OCW_M + 32) {
        s_vsh.rc_aocw[(method - NV097_SET_COMBINER_ALPHA_OCW_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_COLOR_ICW_M && method < NV097_SET_COMBINER_COLOR_ICW_M + 32) {
        s_vsh.rc_cicw[(method - NV097_SET_COMBINER_COLOR_ICW_M) / 4] = param; return 1;
    }
    if (method >= NV097_SET_COMBINER_COLOR_OCW_M && method < NV097_SET_COMBINER_COLOR_OCW_M + 32) {
        s_vsh.rc_cocw[(method - NV097_SET_COMBINER_COLOR_OCW_M) / 4] = param; return 1;
    }
    switch (method) {
    case NV097_SET_COMBINER_CONTROL_M:  s_vsh.rc_ctl = param; s_vsh.rc_set = 1; return 1;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW0_M: s_vsh.rc_fcw0 = param; return 1;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW1_M: s_vsh.rc_fcw1 = param; return 1;
    case NV097_SET_SPECULAR_FOG_FACTOR_M:     s_vsh.rc_sf[0] = param; return 1;
    case NV097_SET_SPECULAR_FOG_FACTOR_M + 4: s_vsh.rc_sf[1] = param; return 1;
    case NV097_SET_SHADER_STAGE_PROGRAM_M:
        s_vsh.shader_prog = param; s_vsh.shader_set = 1; return 1;
    case NV097_SET_FOG_ENABLE_M:        s_vsh.fog_enable = param != 0;  return 1;
    case NV097_SET_FOG_COLOR_M:         s_vsh.fog_color = param;        return 1;
    case NV097_SET_ALPHA_TEST_ENABLE_M: s_vsh.atest_enable = param != 0; return 1;
    case NV097_SET_ALPHA_FUNC_M:        s_vsh.atest_func = param;       return 1;
    case NV097_SET_ALPHA_REF_M:         s_vsh.atest_ref = param & 0xFF; return 1;
    case NV097_SET_POLY_OFFSET_FILL_ENABLE_M: s_vsh.poly_offset = param != 0; return 1;
    case NV097_SET_POLYGON_OFFSET_SCALE_FACTOR_M: s_vsh.poly_factor = u2f(param); return 1;
    case NV097_SET_POLYGON_OFFSET_BIAS_M: s_vsh.poly_bias = u2f(param); return 1;
    case NV097_SET_TRANSFORM_EXECUTION_MODE_M:
        s_vsh.mode = param;
        s_vsh.mode_sets++;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN_M:
        s_vsh.cxt_write = param != 0;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD_M:
        s_vsh.load = param;
        s_vsh.prog_loads++;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_START_M:
        s_vsh.start = param;
        s_vsh.prog_starts++;
        return 1;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD_M:
        s_vsh.cload = param;
        s_vsh.const_loads++;
        return 1;
    case NV097_SET_CULL_FACE_ENABLE_M:  s_vsh.cull_enable = param != 0; return 1;
    case NV097_SET_CULL_FACE_M:         s_vsh.cull_face = param;        return 1;
    case NV097_SET_FRONT_FACE_M:        s_vsh.front_face = param;       return 1;
    case NV097_SET_DEPTH_TEST_ENABLE_M: s_vsh.depth_enable = param != 0; return 1;
    case NV097_SET_DEPTH_FUNC_M:        s_vsh.depth_func = param;       return 1;
    case NV097_SET_DEPTH_MASK_M:        s_vsh.depth_mask = param != 0;  return 1;
    case NV097_SET_SURFACE_ZETA_OFFSET_M: s_vsh.zeta_offset = param;    return 1;
    case NV097_SET_ZSTENCIL_CLEAR_VALUE_M: s_vsh.zclear = param;        return 1;
    default:
        return 0;
    }
}

/* Inline attribute values, the inputs a program reads for any attribute with
 * no array behind it. Called for every SET_VERTEX_DATA* write, alongside the
 * immediate-mode bookkeeping that already consumes some of them. */
static void vsh_inline_attr(uint32_t method, uint32_t param)
{
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA2F_M, a = off / 8;
        s_vsh.inl[a][(off % 8) / 4] = u2f(param);
        s_vsh.inl[a][2] = 0.0f; s_vsh.inl[a][3] = 1.0f;
    } else if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4F_M;
        s_vsh.inl[off / 16][(off % 16) / 4] = u2f(param);
    } else if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        float *d = s_vsh.inl[(method - NV097_SET_VERTEX_DATA4UB) / 4];
        /* Same byte order as a D3DCOLOR array element: B,G,R,A from the
         * bottom, returned as R,G,B,A (see fetch_attr). */
        d[0] = (float)((param >> 16) & 0xFF) / 255.0f;
        d[1] = (float)((param >>  8) & 0xFF) / 255.0f;
        d[2] = (float)( param        & 0xFF) / 255.0f;
        d[3] = (float)( param >> 24        ) / 255.0f;
    } else if (method >= NV097_SET_VERTEX_DATA2S_M
            && method < NV097_SET_VERTEX_DATA2S_M + NV_VERTEX_ATTRS * 4) {
        float *d = s_vsh.inl[(method - NV097_SET_VERTEX_DATA2S_M) / 4];
        d[0] = (float)(int16_t)(param & 0xFFFF);
        d[1] = (float)(int16_t)(param >> 16);
        d[2] = 0.0f; d[3] = 1.0f;
    } else if (method >= NV097_SET_VERTEX_DATA4S_M_M
            && method < NV097_SET_VERTEX_DATA4S_M_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4S_M_M;
        float *d = s_vsh.inl[off / 8];
        uint32_t k = ((off % 8) / 4) * 2;
        d[k]     = (float)(int16_t)(param & 0xFFFF);
        d[k + 1] = (float)(int16_t)(param >> 16);
    } else if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        s_vsh.inl[0][(method - NV097_SET_VERTEX4F) / 4] = u2f(param);
    } else if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        s_vsh.inl[0][(method - NV097_SET_VERTEX3F) / 4] = u2f(param);
        s_vsh.inl[0][3] = 1.0f;
    }
}

static int vsh_batch_active(void)
{
    return vsh_enabled() && (s_vsh.mode & 3u) == 2u;
}

/* Track the program a batch runs, for the report and for RECOMP_VSH_TRACE.
 * Returns the program's length in slots. */
static uint32_t vsh_note_program(void)
{
    uint32_t h = 2166136261u, len = 0, pc, k;
    int i;

    for (pc = s_vsh.start; pc < NV2A_VSH_SLOTS; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ s_vsh.prog[pc][k]) * 16777619u;
        len++;
        if (s_vsh.prog[pc][3] & 1u)
            break;
    }
    for (i = 0; i < s_vsh_prog_count; i++)
        if (s_vsh_progs[i].hash == h && s_vsh_progs[i].start == s_vsh.start) {
            s_vsh_progs[i].batches++;
            return len;
        }
    if (s_vsh_prog_count >= VSH_MAX_PROGS)
        return len;
    s_vsh_progs[s_vsh_prog_count].hash = h;
    s_vsh_progs[s_vsh_prog_count].start = s_vsh.start;
    s_vsh_progs[s_vsh_prog_count].len = len;
    s_vsh_progs[s_vsh_prog_count].batches = 1;
    s_vsh_prog_count++;

    if (getenv("RECOMP_VSH_TRACE")) {
        char buf[160];
        fprintf(stderr, "  [VSH] program #%d hash %08X start %u len %u"
                        " (cxt_write %d)\n",
                s_vsh_prog_count - 1, h, s_vsh.start, len, s_vsh.cxt_write);
        for (pc = s_vsh.start; pc < s_vsh.start + len && pc < NV2A_VSH_SLOTS; pc++)
            fprintf(stderr, "  [VSH]   %3u: %08X %08X %08X  %s\n", pc,
                    s_vsh.prog[pc][1], s_vsh.prog[pc][2], s_vsh.prog[pc][3],
                    nv2a_vsh_disasm(s_vsh.prog[pc], buf, sizeof buf));
    }
    /* Program memory, constants and the attribute table, raw, so the
     * interpreter can be run offline against exactly what the title sent. */
    {
        const char *pre = getenv("RECOMP_VSH_DUMP");
        if (pre) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s%02d_%08X.bin", pre,
                     s_vsh_prog_count - 1, h);
            f = fopen(path, "wb");
            if (f) {
                fwrite(&s_vsh.start, 4, 1, f);
                fwrite(s_vsh.prog, sizeof s_vsh.prog, 1, f);
                fwrite(s_vsh.c, sizeof s_vsh.c, 1, f);
                fwrite(s_vsh.inl, sizeof s_vsh.inl, 1, f);
                fclose(f);
            }
        }
    }
    return len;
}

/* Gather v0..v15 for one vertex: array data where the title enabled an array,
 * the inline value everywhere else. */
static void vsh_inputs(uint32_t index, float v[NV2A_VSH_INPUTS][4])
{
    uint32_t a;

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        if (at->size && at->stride
                && (s_gpu.inline_active || at->offset)
                && fetch_attr(at, index, v[a])) {
            /* An array of fewer than four components fills the rest with
             * (0, 0, 1) like every other vertex fetch. */
            continue;
        }
        memcpy(v[a], s_vsh.inl[a], sizeof v[a]);
    }
}

static void vsh_transform(uint32_t index, XfVert *xv)
{
    float v[NV2A_VSH_INPUTS][4];
    Nv2aVshOut o;
    int k;

    vsh_inputs(index, v);
    nv2a_vsh_run((const uint32_t (*)[4])s_vsh.prog, s_vsh.start, s_vsh.c,
                 s_vsh.cxt_write, (const float (*)[4])v, &o);
    s_vsh.verts_run++;
    xv->x = o.o[NV2A_VSH_O_POS][0];
    xv->y = o.o[NV2A_VSH_O_POS][1];
    xv->z = o.o[NV2A_VSH_O_POS][2];
    xv->w = o.o[NV2A_VSH_O_POS][3];
    for (k = 0; k < 4; k++) {
        float c = o.o[NV2A_VSH_O_D0][k], s = o.o[NV2A_VSH_O_D1][k];
        int t;
        xv->d0[k] = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
        xv->d1[k] = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
        for (t = 0; t < 4; t++)
            xv->t[t][k] = o.o[NV2A_VSH_O_T0 + t][k];
    }
    xv->ok = o.ok && isfinite(xv->x) && isfinite(xv->y) && isfinite(xv->z)
          && xv->w > 0.0f
          && fabsf(xv->x) < 1.0e6f && fabsf(xv->y) < 1.0e6f;
    if (!xv->ok)
        s_vsh.verts_bad++;
}

/* Depth buffer: one float per pixel of the clip rectangle's extent, in [0,1].
 * A colour clear that also names Z resets it (see vsh_clear_depth). */
static float vsh_zmax(void)
{
    /* SET_SURFACE_FORMAT zeta field: 1 = Z16, 2 = Z24S8. */
    return ((s_gpu.format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
}

static int vsh_zbuf_ready(void)
{
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h;
    if (!w || !h || w > 4096 || h > 4096)
        return 0;
    if (w != s_zbuf_w || h != s_zbuf_h || !s_zbuf) {
        float *n = (float *)realloc(s_zbuf, (size_t)w * h * sizeof(float));
        uint32_t i;
        if (!n)
            return 0;
        s_zbuf = n;
        s_zbuf_w = w; s_zbuf_h = h;
        for (i = 0; i < w * h; i++)
            s_zbuf[i] = 1.0f;
    }
    return 1;
}

static void vsh_clear_depth(uint32_t param)
{
    float z;
    uint32_t i;

    s_vsh.clears_seen++;
    if (getenv("RECOMP_VSH_ZLOG") && s_vsh.batches_prog < 1200)
        fprintf(stderr, "  [ZLOG] clear 0x%X colour 0x%X zeta 0x%X zclear 0x%X\n",
                param, s_gpu.color_offset, s_vsh.zeta_offset, s_vsh.zclear);
    if (!(param & 1u) || !vsh_zbuf_ready())
        return;
    s_vsh.zclears++;
    s_vsh.zclear_color_off = s_gpu.color_offset;
    s_vsh.zclear_zeta_off = s_vsh.zeta_offset;
    s_vsh.zclear_batch = s_vsh.batches_prog;
    z = ((s_gpu.format >> 4) & 0xF) == 1
      ? (float)(s_vsh.zclear & 0xFFFFu) / 65535.0f
      : (float)(s_vsh.zclear >> 8) / 16777215.0f;
    for (i = 0; i < s_zbuf_w * s_zbuf_h; i++)
        s_zbuf[i] = z;
}

static int depth_pass(float z, float stored)
{
    switch (s_vsh.depth_func) {
    case 0x200: return 0;
    case 0x201: return z <  stored;
    case 0x202: return z == stored;
    case 0x203: return z <= stored;
    case 0x204: return z >  stored;
    case 0x205: return z != stored;
    case 0x206: return z >= stored;
    default:    return 1;                  /* ALWAYS, and anything unset */
    }
}

static int wrap_signed(int c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1) {                       /* wrap */
        int m = c % (int)size;
        return m < 0 ? m + (int)size : m;
    }
    if (c < 0) return 0;
    return c >= (int)size ? (int)size - 1 : c;
}

/* ---- Texture stages and register combiners ----------------------------
 *
 * Per batch, each of the four stages is decoded from the texture registers
 * the title last wrote (s_tex_reg), and given a texture-shader mode from
 * SET_SHADER_STAGE_PROGRAM. Per pixel, the stages are sampled into T0..T3 and
 * the general combiners then the final combiner run on them, after xemu's
 * psh.c. That is what decides whether a texel is modulated by oD0, added to
 * it, ignored, or used only for its alpha -- which a fixed MODULATE got wrong
 * for every intro batch whose oD0 is zero.
 *
 * ponytail: point sampling, 2D only. Cube, 3D, dot-product and bump-map
 * shader modes sample as plain 2D at (s, t); the report counts them. Fog is
 * fog colour with factor 1 (no fog). */
static Texture s_stage_tex[4];
static int     s_stage_mode[4];         /* 0 none, 1 2D, 4 pass-through, other */
static float   s_stage_scale[4][2];     /* normalised -> texels */

static void vsh_build_stages(void)
{
    int n;
    for (n = 0; n < 4; n++) {
        const uint32_t *r = &s_tex_reg[(0x40u * n) / 4];
        const uint8_t  *set = &s_tex_set[(0x40u * n) / 4];
        Texture *t = &s_stage_tex[n];
        uint32_t fmt = r[0x04 / 4], ctl0 = r[0x0C / 4];
        int on = (ctl0 >> 30) & 1;
        int mode;

        memset(t, 0, sizeof *t);
        t->offset = r[0] ? dma_resolve(r[0]) : 0;
        t->color = (fmt >> 8) & 0xFF;
        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((fmt >> 20) & 0xF);
            t->height = 1u << ((fmt >> 24) & 0xF);
        } else {
            t->width  = r[0x1C / 4] >> 16;
            t->height = r[0x1C / 4] & 0xFFFF;
        }
        t->pitch  = r[0x10 / 4] >> 16;
        t->addr_u =  r[0x08 / 4]       & 0xF;
        t->addr_v = (r[0x08 / 4] >> 8) & 0xF;
        t->valid = t->offset && t->width && t->height
                && (tex_size_from_format(t->color) || t->pitch);
        if (n == 0 && !set[0x0C / 4])
            on = 1;                        /* never configured: as before */
        if (s_vsh.shader_set)
            mode = (int)((s_vsh.shader_prog >> (5 * n)) & 0x1F);
        else
            mode = n == 0 ? 1 : 0;
        s_vsh.stage_mode_seen[n][mode]++;
        if (mode != 4 && (!on || !t->valid))
            mode = 0;
        s_stage_mode[n] = mode;
        s_stage_scale[n][0] = tex_size_from_format(t->color) ? (float)t->width : 1.0f;
        s_stage_scale[n][1] = tex_size_from_format(t->color) ? (float)t->height : 1.0f;
    }
}

static void rc_unpack(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    o[2] = (float)( c        & 0xFF) / 255.0f;
    o[3] = (float)( c >> 24        ) / 255.0f;
}

static float rc_map(float x, uint32_t map)
{
    switch (map) {
    case 0: return x < 0.0f ? 0.0f : x;                     /* unsigned */
    case 1: return 1.0f - (x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x);
    case 2: return 2.0f * (x < 0.0f ? 0.0f : x) - 1.0f;     /* expand */
    case 3: return 1.0f - 2.0f * (x < 0.0f ? 0.0f : x);
    case 4: return (x < 0.0f ? 0.0f : x) - 0.5f;            /* half bias */
    case 5: return 0.5f - (x < 0.0f ? 0.0f : x);
    case 6: return x;                                       /* signed */
    default: return -x;
    }
}

static void rc_in_rgb(float r[16][4], uint32_t b, float v[3])
{
    const float *s = r[b & 0xF];
    uint32_t map = (b >> 5) & 7;
    int k;
    for (k = 0; k < 3; k++)
        v[k] = rc_map((b & 0x10) ? s[3] : s[k], map);
}

static float rc_in_a(float r[16][4], uint32_t b)
{
    const float *s = r[b & 0xF];
    return rc_map((b & 0x10) ? s[3] : s[2], (b >> 5) & 7);
}

static float rc_op(float x, uint32_t op)
{
    switch (op) {
    case 1: x -= 0.5f; break;
    case 2: x *= 2.0f; break;
    case 3: x = (x - 0.5f) * 2.0f; break;
    case 4: x *= 4.0f; break;
    case 6: x *= 0.5f; break;
    default: break;
    }
    return x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* General combiners then the final combiner, on r[] (register-code indexed);
 * the result is written to out[4] as r,g,b,a. */
static void rc_eval(float r[16][4], float out[4])
{
    uint32_t stages = s_vsh.rc_ctl & 0xFF, s;
    int mux_msb = (s_vsh.rc_ctl >> 8) & 1;
    int uniq0 = (s_vsh.rc_ctl >> 12) & 1, uniq1 = (s_vsh.rc_ctl >> 16) & 1;
    float nr[16][4], A[3], B[3], C[3], D[3], E[3], F[3];
    int k;

    if (stages > 8)
        stages = 8;
    for (s = 0; s < stages; s++) {
        uint32_t icw = s_vsh.rc_cicw[s], ocw = s_vsh.rc_cocw[s];
        uint32_t aicw = s_vsh.rc_aicw[s], aocw = s_vsh.rc_aocw[s];
        float ab[3], cd[3], sum[3], aab, acd, asum;
        int sel;

        rc_unpack(s_vsh.rc_f0[uniq0 ? s : 0], r[1]);
        rc_unpack(s_vsh.rc_f1[uniq1 ? s : 0], r[2]);
        memcpy(nr, r, sizeof nr);
        sel = mux_msb ? r[12][3] >= 0.5f
                      : (((uint32_t)(clamp01(r[12][3]) * 255.0f + 0.5f)) & 1u);

        /* colour */
        rc_in_rgb(r, icw >> 24, A); rc_in_rgb(r, icw >> 16, B);
        rc_in_rgb(r, icw >>  8, C); rc_in_rgb(r, icw,       D);
        if (ocw & (1u << 13)) {
            float d = A[0]*B[0] + A[1]*B[1] + A[2]*B[2];
            ab[0] = ab[1] = ab[2] = d;
        } else {
            for (k = 0; k < 3; k++) ab[k] = A[k] * B[k];
        }
        if (ocw & (1u << 12)) {
            float d = C[0]*D[0] + C[1]*D[1] + C[2]*D[2];
            cd[0] = cd[1] = cd[2] = d;
        } else {
            for (k = 0; k < 3; k++) cd[k] = C[k] * D[k];
        }
        for (k = 0; k < 3; k++) {
            sum[k] = (ocw & (1u << 14)) ? (sel ? cd[k] : ab[k]) : ab[k] + cd[k];
            ab[k]  = rc_op(ab[k],  (ocw >> 15) & 7);
            cd[k]  = rc_op(cd[k],  (ocw >> 15) & 7);
            sum[k] = rc_op(sum[k], (ocw >> 15) & 7);
        }
        if ((ocw >> 4) & 0xF) memcpy(nr[(ocw >> 4) & 0xF], ab, sizeof ab);
        if ( ocw       & 0xF) memcpy(nr[ ocw       & 0xF], cd, sizeof cd);
        if ((ocw >> 8) & 0xF) memcpy(nr[(ocw >> 8) & 0xF], sum, sizeof sum);

        /* alpha */
        aab = rc_in_a(r, aicw >> 24) * rc_in_a(r, aicw >> 16);
        acd = rc_in_a(r, aicw >>  8) * rc_in_a(r, aicw);
        asum = (aocw & (1u << 12)) ? (sel ? acd : aab) : aab + acd;
        if ((aocw >> 4) & 0xF) nr[(aocw >> 4) & 0xF][3] = rc_op(aab,  (aocw >> 13) & 7);
        if ( aocw       & 0xF) nr[ aocw       & 0xF][3] = rc_op(acd,  (aocw >> 13) & 7);
        if ((aocw >> 8) & 0xF) nr[(aocw >> 8) & 0xF][3] = rc_op(asum, (aocw >> 13) & 7);

        /* blue to alpha overrides the alpha portion's write */
        if ((ocw & (1u << 19)) && ((ocw >> 4) & 0xF)) nr[(ocw >> 4) & 0xF][3] = ab[2];
        if ((ocw & (1u << 18)) && (ocw & 0xF))        nr[ocw & 0xF][3] = cd[2];
        memcpy(r, nr, sizeof nr);
    }

    /* Final combiner: unsigned inputs, A*B + (1-A)*C + D, alpha from G. */
    {
        uint32_t c0 = s_vsh.rc_fcw0, c1 = s_vsh.rc_fcw1;
        float v1[3], r0[3], G;
        rc_unpack(s_vsh.rc_sf[0], r[1]);
        rc_unpack(s_vsh.rc_sf[1], r[2]);
#define FC_IN(b, v) do { const float *s_ = r[(b) & 0xF]; int k_; \
        for (k_ = 0; k_ < 3; k_++) { float x_ = clamp01(((b) & 0x10) ? s_[3] : s_[k_]); \
            (v)[k_] = ((b) & 0x20) ? 1.0f - x_ : x_; } } while (0)
        FC_IN(c1 >> 24, E);
        FC_IN(c1 >> 16, F);
        for (k = 0; k < 3; k++) {
            v1[k] = clamp01(r[5][k]);  if (c1 & 0x20) v1[k] = 1.0f - v1[k];
            r0[k] = clamp01(r[12][k]); if (c1 & 0x40) r0[k] = 1.0f - r0[k];
            r[14][k] = v1[k] + r0[k];
            if (c1 & 0x80) r[14][k] = clamp01(r[14][k]);
            r[15][k] = E[k] * F[k];
        }
        r[14][3] = r[15][3] = 0.0f;
        FC_IN(c0 >> 24, A); FC_IN(c0 >> 16, B);
        FC_IN(c0 >>  8, C); FC_IN(c0,       D);
        {
            uint32_t g = c1 >> 8;
            const float *sg = r[g & 0xF];
            G = clamp01((g & 0x10) ? sg[3] : sg[2]);
            if (g & 0x20) G = 1.0f - G;
        }
#undef FC_IN
        for (k = 0; k < 3; k++)
            out[k] = clamp01(A[k] * B[k] + (1.0f - A[k]) * C[k] + D[k]);
        out[3] = G;
    }
}

static int vsh_rc_active(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PB_RC");
        on = !(e && e[0] == '0');
    }
    return on && s_vsh.rc_set && (s_vsh.rc_ctl & 0xFF);
}

static int alpha_test_pass(float a)
{
    uint32_t v = (uint32_t)(clamp01(a) * 255.0f + 0.5f), ref = s_vsh.atest_ref;
    switch (s_vsh.atest_func) {
    case 0x200: return 0;
    case 0x201: return v <  ref;
    case 0x202: return v == ref;
    case 0x203: return v <= ref;
    case 0x204: return v >  ref;
    case 0x205: return v != ref;
    case 0x206: return v >= ref;
    default:    return 1;
    }
}

static void stage_sample(int n, const float tc[4], float o[4])
{
    int mode = s_stage_mode[n];
    uint32_t texel;
    float s, t;
    int iu, iv;

    if (mode == 0) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    if (mode == 4) {                       /* pass-through: the coordinate */
        int k;
        for (k = 0; k < 4; k++) o[k] = clamp01(tc[k]);
        return;
    }
    s = tc[0]; t = tc[1];
    if (mode == 1 && tc[3] != 0.0f && tc[3] != 1.0f) {   /* 2D projective */
        s /= tc[3]; t /= tc[3];
    }
    iu = wrap_signed((int)floorf(s * s_stage_scale[n][0]), s_stage_tex[n].width,  s_stage_tex[n].addr_u);
    iv = wrap_signed((int)floorf(t * s_stage_scale[n][1]), s_stage_tex[n].height, s_stage_tex[n].addr_v);
    if (sample_tex(&s_stage_tex[n], (uint32_t)iu, (uint32_t)iv, &texel))
        rc_unpack(texel, o);
    else
        o[0] = o[1] = o[2] = 0.0f, o[3] = 1.0f;
}

static void raster_tri_xf(const XfVert *a, const XfVert *b, const XfVert *c,
                          int textured)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area, inv_w[3], zscale = 1.0f / vsh_zmax();
    int minx, maxx, miny, maxy, x, y, use_z, use_rc, n;

    if (bpp != 4 && bpp != 2)
        return;
    if (!a->ok || !b->ok || !c->ok) {
        s_vsh.tris_clipped++;
        return;
    }
    area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0.0f)
        return;
    if (s_vsh.cull_enable) {
        /* area > 0 is clockwise as displayed (y down). FRONT_FACE CW means a
         * clockwise triangle faces the viewer. */
        int cw = area > 0.0f;
        int front = (s_vsh.front_face == 0x900) ? cw : !cw;
        if (getenv("RECOMP_PB_CULL_FLIP"))
            front = !front;
        if ((s_vsh.cull_face == 0x405 && !front)
         || (s_vsh.cull_face == 0x404 && front)
         || s_vsh.cull_face == 0x408) {
            s_vsh.tris_culled++;
            return;
        }
    }
    if (!surface_begin_batch(mem))
        return;
    use_z = s_vsh.depth_enable && vsh_zbuf_ready();
    use_rc = vsh_rc_active();

    minx = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    maxx = (int)ceilf (fmaxf(a->x, fmaxf(b->x, c->x)));
    miny = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    maxy = (int)ceilf (fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    inv_w[0] = 1.0f / a->w; inv_w[1] = 1.0f / b->w; inv_w[2] = 1.0f / c->w;

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
            float w1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
            float w2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
            float la, lb, lc, pa, pb, pc, ps, col[4], z = 0.0f;
            float *zp = NULL;
            uint32_t argb;
            int k;

            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            /* w1 is opposite a, w2 opposite b, w0 opposite c. */
            la = w1 / area; lb = w2 / area; lc = w0 / area;

            if (use_z) {
                z = (la * a->z + lb * b->z + lc * c->z) * zscale;
                zp = &s_zbuf[(size_t)y * s_zbuf_w + (size_t)x];
                if (!depth_pass(z, *zp)) {
                    s_vsh.z_rejected++;
                    continue;
                }
            }

            /* Perspective-correct weights. */
            pa = la * inv_w[0]; pb = lb * inv_w[1]; pc = lc * inv_w[2];
            ps = pa + pb + pc;
            if (ps != 0.0f) { pa /= ps; pb /= ps; pc /= ps; }

            for (k = 0; k < 4; k++)
                col[k] = pa * a->d0[k] + pb * b->d0[k] + pc * c->d0[k];

            if (use_rc) {
                float r[16][4], tc[4];
                memset(r, 0, sizeof r);
                memcpy(r[4], col, sizeof col);
                for (k = 0; k < 4; k++)
                    r[5][k] = pa * a->d1[k] + pb * b->d1[k] + pc * c->d1[k];
                rc_unpack(s_vsh.fog_color, r[3]);
                { float t_ = r[3][0]; r[3][0] = r[3][2]; r[3][2] = t_; } /* RGBA */
                r[3][3] = 1.0f;
                for (n = 0; n < 4; n++) {
                    if (!s_stage_mode[n])
                        continue;
                    for (k = 0; k < 4; k++)
                        tc[k] = pa * a->t[n][k] + pb * b->t[n][k] + pc * c->t[n][k];
                    stage_sample(n, tc, r[8 + n]);
                }
                r[12][3] = r[8][3];
                rc_eval(r, col);
            } else if (textured) {
                float tc[4], tx[4];
                for (k = 0; k < 4; k++)
                    tc[k] = pa * a->t[0][k] + pb * b->t[0][k] + pc * c->t[0][k];
                if (s_stage_mode[0]) {
                    stage_sample(0, tc, tx);
                    for (k = 0; k < 4; k++)
                        col[k] *= tx[k];
                }
            }
            if (s_vsh.atest_enable && !alpha_test_pass(col[3])) {
                s_vsh.a_rejected++;
                continue;
            }
            if (use_z && s_vsh.depth_mask)
                *zp = z;
            argb = ((uint32_t)(clamp01(col[3]) * 255.0f + 0.5f) << 24)
                 | ((uint32_t)(clamp01(col[0]) * 255.0f + 0.5f) << 16)
                 | ((uint32_t)(clamp01(col[1]) * 255.0f + 0.5f) <<  8)
                 |  (uint32_t)(clamp01(col[2]) * 255.0f + 0.5f);
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_vsh.tris++;
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
}

#define NV097_SET_TEXTURE_CONTROL0_M 0x1B0C

static void raster_batch_vsh(void)
{
    static XfVert xv[NV_MAX_INDICES];
    uint32_t i, n = s_gpu.idx_count, a;
    uint32_t before = s_gpu.tris_drawn, plen;
    int textured;

    s_vsh.batches_prog++;
    plen = vsh_note_program();
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_vsh.fmt_seen[a][s_gpu.attr[a].type & 7]++;

    for (i = 0; i < n; i++)
        vsh_transform(s_gpu.idx[i], &xv[i]);
    if (getenv("RECOMP_VSH_ZLOG") && s_vsh.batches_prog < 1200 && n)
        fprintf(stderr, "  [ZLOG] batch %u colour 0x%X depth %d func 0x%X mask %d"
                        " poly %d %g %g | v0 z %g w %g tex 0x%X fmt %02X\n",
                s_vsh.batches_prog, s_gpu.color_offset, s_vsh.depth_enable,
                s_vsh.depth_func, s_vsh.depth_mask, s_vsh.poly_offset,
                s_vsh.poly_factor, s_vsh.poly_bias, xv[0].z, xv[0].w,
                s_gpu.tex.offset, s_gpu.tex.color);
    if (plen > 12 && getenv("RECOMP_VSH_TRACE")) {
        static int shown3d;
        if (shown3d++ < 6) {
            float v[NV2A_VSH_INPUTS][4];
            fprintf(stderr, "  [VSH] 3D batch: prim %u n %u start %u c58 %g %g %g %g"
                            " c59 %g %g %g %g\n", s_gpu.prim, n, s_vsh.start,
                    s_vsh.c[58][0], s_vsh.c[58][1], s_vsh.c[58][2], s_vsh.c[58][3],
                    s_vsh.c[59][0], s_vsh.c[59][1], s_vsh.c[59][2], s_vsh.c[59][3]);
            for (i = 0; i < n && i < 6; i++) {
                vsh_inputs(s_gpu.idx[i], v);
                fprintf(stderr, "  [VSH]   v%u idx %u in v0 %g %g %g %g ->"
                                " (%g, %g, %g, w %g) ok %d\n", i, s_gpu.idx[i],
                        v[0][0], v[0][1], v[0][2], v[0][3],
                        xv[i].x, xv[i].y, xv[i].z, xv[i].w, xv[i].ok);
            }
        }
    }
    if (n)
        memcpy(s_vsh_last_d0, xv[0].d0, sizeof s_vsh_last_d0), s_vsh_last_v0 = xv[0];

    {
        uint32_t ctl0 = s_tex_reg[(NV097_SET_TEXTURE_CONTROL0_M - NV_TEX_FIRST) / 4];
        int stage_on = !s_tex_set[(NV097_SET_TEXTURE_CONTROL0_M - NV_TEX_FIRST) / 4]
                    || (ctl0 & (1u << 30));
        textured = stage_on && s_gpu.tex.valid;
        vsh_build_stages();
        if (vsh_rc_active()) s_vsh.batches_rc++; else s_vsh.batches_norc++;
        if (textured) {
            s_gpu.batches_textured++;
            note_texture_use();
        } else {
            s_gpu.batches_no_tex++;
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_tri_xf(&xv[i], &xv[i+1], &xv[i+2], textured);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        /* Odd triangles of a strip are wound the other way; swap two of them
         * back so culling sees one winding for the whole strip. */
        for (i = 0; i + 2 < n; i++) {
            if (i & 1)
                raster_tri_xf(&xv[i+1], &xv[i], &xv[i+2], textured);
            else
                raster_tri_xf(&xv[i], &xv[i+1], &xv[i+2], textured);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_tri_xf(&xv[0], &xv[i], &xv[i+1], textured);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_tri_xf(&xv[i], &xv[i+1], &xv[i+2], textured);
            raster_tri_xf(&xv[i], &xv[i+2], &xv[i+3], textured);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_tri_xf(&xv[i], &xv[i+1], &xv[i+3], textured);
            raster_tri_xf(&xv[i], &xv[i+3], &xv[i+2], textured);
        }
        break;
    default:
        break;
    }
    if (textured)
        s_vsh.tris_textured += s_gpu.tris_drawn - before;

    if (getenv("RECOMP_VSH_TRACE")) {
        static int shown;
        if (shown < 24 && n) {
            shown++;
            fprintf(stderr, "  [VSH] batch prim %u n %u start %u tex %d:"
                            " v0 -> (%.1f, %.1f, %.4g, w %.4g) d0 %.2f %.2f %.2f %.2f"
                            " t0 %.3f %.3f ok %d\n",
                    s_gpu.prim, n, s_vsh.start, textured,
                    xv[0].x, xv[0].y, xv[0].z, xv[0].w,
                    xv[0].d0[0], xv[0].d0[1], xv[0].d0[2], xv[0].d0[3],
                    xv[0].t[0][0], xv[0].t[0][1], xv[0].ok);
        }
    }

    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
    /* Longer than the pretransformed passthroughs (7 and 10 slots): a real
     * transform. Dump every 400th such batch, first included, up to 12, so a
     * run that reaches 3D leaves frames of it behind. */
    if (plen > 12 && s_gpu.tris_drawn != before) {
        s_vsh.batches_3d++;
        if (s_vsh.batches_3d % 400 == 1 && s_vsh.dumps_3d < 12) {
            s_vsh.dumps_3d++;
            fprintf(stderr, "  [VSH] 3D batch %u (program len %u): dumping\n",
                    s_vsh.batches_3d, plen);
            dump_surface_bmp();
        }
    }
}

static void vsh_report(void)
{
    int i;
    uint32_t a, t;

    fprintf(stderr, "[VSH] mode 0x%X (set %u) | program: %u dwords, %u loads,"
                    " %u starts, start %u | constants: %u dwords, %u loads |"
                    " viewport %u dwords, ff-matrix %u dwords\n",
            s_vsh.mode, s_vsh.mode_sets, s_vsh.prog_dwords, s_vsh.prog_loads,
            s_vsh.prog_starts, s_vsh.start, s_vsh.const_dwords,
            s_vsh.const_loads, s_vsh.vp_dwords, s_vsh.ff_matrix_dwords);
    fprintf(stderr, "[VSH] batches: %u program, %u fixed-function; %u vertices"
                    " run (%u unusable); %u tris drawn (%u textured), %u culled,"
                    " %u dropped for w<=0/overflow, %llu pixels depth-rejected%s\n",
            s_vsh.batches_prog, s_vsh.batches_fixed, s_vsh.verts_run,
            s_vsh.verts_bad, s_vsh.tris, s_vsh.tris_textured, s_vsh.tris_culled,
            s_vsh.tris_clipped, (unsigned long long)s_vsh.z_rejected,
            vsh_enabled() ? "" : "  [RECOMP_PB_VSH=0: path off]");
    fprintf(stderr, "[VSH] viewport offset %.2f %.2f %.2f %.2f scale %.2f %.2f"
                    " %.2f %.2f | c58 %.2f %.2f %.2f c59 %.2f %.2f %.2f |"
                    " cull %d face 0x%X front 0x%X | depth %d func 0x%X mask %d"
                    " | surface fmt 0x%X zeta 0x%X, %u clears (%u with Z)\n",
            s_vsh.vp_off[0], s_vsh.vp_off[1], s_vsh.vp_off[2], s_vsh.vp_off[3],
            s_vsh.vp_scale[0], s_vsh.vp_scale[1], s_vsh.vp_scale[2], s_vsh.vp_scale[3],
            s_vsh.c[58][0], s_vsh.c[58][1], s_vsh.c[58][2],
            s_vsh.c[59][0], s_vsh.c[59][1], s_vsh.c[59][2],
            s_vsh.cull_enable, s_vsh.cull_face, s_vsh.front_face,
            s_vsh.depth_enable, s_vsh.depth_func, s_vsh.depth_mask,
            s_gpu.format, s_vsh.zeta_offset, s_vsh.clears_seen, s_vsh.zclears);
    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        int any = 0;
        for (t = 0; t < 8; t++) any |= s_vsh.fmt_seen[a][t] != 0;
        if (!any) continue;
        fprintf(stderr, "[VSH]   v%-2u batches by type:", a);
        for (t = 0; t < 8; t++)
            if (s_vsh.fmt_seen[a][t])
                fprintf(stderr, " %s=%u",
                        t == 0 ? "ub_d3d" : t == 1 ? "s1" : t == 2 ? "f"
                        : t == 4 ? "ub_ogl" : t == 5 ? "s32k" : t == 6 ? "cmp" : "?",
                        s_vsh.fmt_seen[a][t]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[VSH] combiners: %u batches evaluated, %u fixed MODULATE%s;"
                    " ctl 0x%X fcw 0x%08X/0x%08X; alpha test %d func 0x%X ref %u"
                    " (%llu px rejected); fog %d\n",
            s_vsh.batches_rc, s_vsh.batches_norc,
            getenv("RECOMP_PB_RC") && getenv("RECOMP_PB_RC")[0] == '0'
                ? " [RECOMP_PB_RC=0]" : "",
            s_vsh.rc_ctl, s_vsh.rc_fcw0, s_vsh.rc_fcw1, s_vsh.atest_enable,
            s_vsh.atest_func, s_vsh.atest_ref,
            (unsigned long long)s_vsh.a_rejected, s_vsh.fog_enable);
    for (a = 0; a < 4; a++) {
        fprintf(stderr, "[VSH]   stage %u shader modes (batches):", a);
        for (t = 0; t < 32; t++)
            if (s_vsh.stage_mode_seen[a][t])
                fprintf(stderr, " %u=%u", t, s_vsh.stage_mode_seen[a][t]);
        fprintf(stderr, "\n");
    }
    if (s_vsh_ab_batches)
        fprintf(stderr, "[VSH] A/B vs screen-space path: %llu batches, %llu differ,"
                        " %llu px differ\n",
                (unsigned long long)s_vsh_ab_batches,
                (unsigned long long)s_vsh_ab_diff_batches,
                (unsigned long long)s_vsh_ab_diff_px);
    for (i = 0; i < s_vsh_prog_count; i++)
        fprintf(stderr, "[VSH]   program %08X start %u len %u: %u batches\n",
                s_vsh_progs[i].hash, s_vsh_progs[i].start, s_vsh_progs[i].len,
                s_vsh_progs[i].batches);
}

/* RECOMP_PB_VSH_AB=1: draw every program-mode batch twice, the old
 * screen-space way and through the vertex program, keep the program result,
 * and count the pixels where the two disagree. This is the before/after check
 * on the intro, made per batch inside one run so movie timing cannot move it. */
static int s_vsh_ab_inner;

static void raster_batch(void);

static void ab_write_bmp(const char *path, const uint8_t *surf, uint32_t bpp)
{
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h, x, y;
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, fsz = 54 + (row + pad) * h;
    uint8_t hdr[54] = {0}, px[3], z[3] = {0};
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    hdr[0] = 'B'; hdr[1] = 'M'; memcpy(hdr + 2, &fsz, 4); hdr[10] = 54;
    hdr[14] = 40; memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = h; y-- > 0; ) {
        const uint8_t *r = surf + (size_t)y * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            if (bpp == 4) {
                px[0] = r[x*4]; px[1] = r[x*4+1]; px[2] = r[x*4+2];
            } else {
                uint16_t t = ((const uint16_t *)r)[x];
                px[0] = (uint8_t)((t & 0x1F) << 3);
                px[1] = (uint8_t)(((t >> 5) & 0x3F) << 2);
                px[2] = (uint8_t)((t >> 11) << 3);
            }
            fwrite(px, 1, 3, f);
        }
        fwrite(z, 1, pad, f);
    }
    fclose(f);
}

static void raster_batch_ab(void)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    static uint8_t *save, *old;
    static size_t cap;
    size_t size = (size_t)(s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch, k;
    uint32_t bpp = surface_bpp(), diff = 0, maxd = 0;
    int dumps = s_drawn_dumps;
    uint64_t zrej0;

    if (!size || !surface_begin_batch(mem) || (bpp != 4 && bpp != 2)) {
        raster_batch_vsh();
        return;
    }
    if (size > cap) {
        save = (uint8_t *)realloc(save, size);
        old  = (uint8_t *)realloc(old, size);
        cap = save && old ? size : 0;
        if (!cap) { raster_batch_vsh(); return; }
    }
    memcpy(save, s_surface, size);
    s_vsh_ab_inner = 1;
    s_drawn_dumps = FB_DUMP_AFTER_DRAW;
    raster_batch();                        /* old path */
    s_drawn_dumps = dumps;
    s_vsh_ab_inner = 0;
    surface_begin_batch(mem);
    memcpy(old, s_surface, size);
    memcpy(s_surface, save, size);
    zrej0 = s_vsh.z_rejected;
    raster_batch_vsh();
    surface_begin_batch(mem);
    for (k = 0; k + bpp <= size; k += bpp) {
        uint32_t d = 0, c;
        for (c = 0; c < (bpp == 4 ? 3u : 2u); c++) {
            int e = (int)s_surface[k + c] - (int)old[k + c];
            if (e < 0) e = -e;
            if ((uint32_t)e > d) d = (uint32_t)e;
        }
        if (d > 2) diff++;
        if (d > maxd) maxd = d;
    }
    s_vsh_ab_batches++;
    if (diff) {
        /* RECOMP_PB_VSH_AB_DUMP=<prefix>: both versions of the first
         * differing batch per texture, as <prefix>NNN_old/new.bmp. */
        const char *pre = getenv("RECOMP_PB_VSH_AB_DUMP");
        static uint32_t seen[16];
        static int nseen;
        int j, have = 0;
        for (j = 0; j < nseen; j++) have |= seen[j] == s_gpu.tex.offset;
        static int zdumps;
        int zd = pre && s_vsh.z_rejected != zrej0 && zdumps < 4;
        zdumps += zd;
        if ((pre && !have && nseen < 16) || zd) {
            char path[512];
            if (!have && nseen < 16)
                seen[nseen++] = s_gpu.tex.offset;
            snprintf(path, sizeof path, "%s%03llu_old.bmp", pre,
                     (unsigned long long)s_vsh_ab_batches);
            ab_write_bmp(path, old, bpp);
            snprintf(path, sizeof path, "%s%03llu_new.bmp", pre,
                     (unsigned long long)s_vsh_ab_batches);
            ab_write_bmp(path, s_surface, bpp);
        }
        s_vsh_ab_diff_batches++;
        s_vsh_ab_diff_px += diff;
        static int zshown;
        int zlog = s_vsh.z_rejected != zrej0 && zshown < 24;
        zshown += zlog;
        if (s_vsh_ab_diff_batches <= 16 || s_vsh_ab_diff_batches % 64 == 0 || zlog
                || (pre && !have && nseen <= 16 && seen[nseen - 1] == s_gpu.tex.offset))
            fprintf(stderr, "  [VSH-AB] batch %llu prim %u n %u: %u px differ"
                            " (max %u); d0 %.2f %.2f %.2f %.2f tex %08X fmt %02X"
                            " %ux%u blend %u %X/%X | z %.4g w %.4g zrej %llu"
                            " t0 %.3f %.3f | colour 0x%X zeta 0x%X; last Z clear"
                            " colour 0x%X zeta 0x%X %u batches ago\n",
                    (unsigned long long)s_vsh_ab_batches, s_gpu.prim,
                    s_gpu.idx_count, diff, maxd,
                    s_vsh_last_d0[0], s_vsh_last_d0[1], s_vsh_last_d0[2],
                    s_vsh_last_d0[3], s_gpu.tex.offset, s_gpu.tex.color,
                    s_gpu.tex.width, s_gpu.tex.height, s_gpu.blend_enable,
                    s_gpu.blend_sfactor, s_gpu.blend_dfactor,
                    s_vsh_last_v0.z, s_vsh_last_v0.w,
                    (unsigned long long)(s_vsh.z_rejected - zrej0),
                    s_vsh_last_v0.t[0][0], s_vsh_last_v0.t[0][1],
                    s_gpu.color_offset, s_vsh.zeta_offset, s_vsh.zclear_color_off,
                    s_vsh.zclear_zeta_off, s_vsh.batches_prog - s_vsh.zclear_batch);
    }
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;

    if (s_gpu.idx_count < 3)
        return;
    if (!s_vsh_ab_inner && vsh_batch_active()) {
        static int ab = -1;
        if (ab < 0)
            ab = getenv("RECOMP_PB_VSH_AB") != NULL;
        if (ab)
            raster_batch_ab();
        else
            raster_batch_vsh();
        return;
    }
    if ((s_vsh.mode & 3u) != 2u)
        s_vsh.batches_fixed++;
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.tex.valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* ── Backend hooks (nv2a_pb_state.h) ─────────────────────────────────────
 *
 * The walker decodes; a backend draws. The CPU rasteriser below is the
 * default and reads s_gpu directly, so its on_draw ignores the descriptor. */
static void cpu_on_draw(const struct nv2a_pb_draw *draw)
{
    (void)draw;
    raster_batch();
}

/* Hand the window a copy of the frame just finished. Copying here, rather
 * than letting the window read guest memory on its own clock, is what stops
 * it showing a surface the rasteriser is still writing. */
static void cpu_on_flip(uint32_t surface, uint32_t pitch)
{
    extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);

    if (!pitch || !surface)
        return;
    xbox_FramebufferWindowSet(dma_resolve(surface), pitch);
    xbox_FramebufferWindowPresent(dma_resolve(surface), pitch);
}

static const struct nv2a_pb_backend s_cpu_backend = {
    clear_surface,                       /* on_clear  */
    vsh_clear_depth,                     /* on_zclear */
    cpu_on_draw,                         /* on_draw   */
    cpu_on_flip,                         /* on_flip   */
};
static const struct nv2a_pb_backend *s_backend = &s_cpu_backend;

void nv2a_pb_set_backend(const struct nv2a_pb_backend *backend)
{
    s_backend = backend ? backend : &s_cpu_backend;
}

const struct nv2a_pb_backend *nv2a_pb_get_backend(void) { return s_backend; }
const struct nv2a_pb_backend *nv2a_pb_cpu_backend(void) { return &s_cpu_backend; }

const struct nv2a_pb_gpu *nv2a_pb_gpu_state(void) { return &s_gpu; }
const struct nv2a_pb_vsh *nv2a_pb_vsh_state(void) { return &s_vsh; }
const uint32_t *nv2a_pb_tex_regs(void)            { return s_tex_reg; }
const uint8_t  *nv2a_pb_tex_regs_set(void)        { return s_tex_set; }
uint32_t nv2a_pb_dma_resolve(uint32_t offset)     { return dma_resolve(offset); }

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if ((s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    if (s_backend->on_draw) {
        struct nv2a_pb_draw d;
        d.prim         = s_gpu.prim;
        d.attr         = s_gpu.attr;
        d.idx          = s_gpu.idx;
        d.idx_count    = s_gpu.idx_count;
        d.inline_data  = s_gpu.inline_active;
        d.inline_buf   = s_gpu.inline_buf;
        d.inline_bytes = s_gpu.inline_count * 4;
        d.program      = vsh_batch_active();
        s_backend->on_draw(&d);
    }

    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                    s_gpu.tex.pitch, s_gpu.tex.color,
                    d3d8_format_is_swizzled(s_gpu.tex.color), s_gpu.tex.valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                        s_gpu.tex.pitch, s_gpu.tex.color, s_gpu.tex.valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = (uint16_t)i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. */
    s_gpu.attr[0].type = 2; s_gpu.attr[0].size = 4;   /* position float4  */
    s_gpu.attr[0].offset = 0;
    s_gpu.attr[3].type = 0; s_gpu.attr[3].size = 4;   /* diffuse D3DCOLOR */
    s_gpu.attr[3].offset = 16;
    s_gpu.attr[9].type = 2; s_gpu.attr[9].size = 2;   /* texcoord0 float2 */
    s_gpu.attr[9].offset = 20;
    s_gpu.attr[0].stride = s_gpu.attr[3].stride = s_gpu.attr[9].stride =
        IMM_VERTEX_DWORDS * 4;

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = (uint16_t)i;
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append it in the layout draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at],     s_gpu.imm_pos, 4 * sizeof(float));
    memcpy(&s_gpu.inline_buf[at + 4], &s_gpu.imm_diffuse, sizeof(uint32_t));
    memcpy(&s_gpu.inline_buf[at + 5], s_gpu.imm_tex, 2 * sizeof(float));
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    v.u = param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        uint32_t c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 3)                      /* w completes the vertex */
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        uint32_t c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 2) {                    /* z completes it, w is implicitly 1 */
            s_gpu.imm_pos[3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA2F_M;
        if (off / 8 == 9)                /* attribute 9 is texture coord 0 */
            s_gpu.imm_tex[(off % 8) / 4] = v.f;
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4F_M;
        uint32_t attr = off / 16, c = (off % 16) / 4;
        if (attr == 0) {
            s_gpu.imm_pos[c] = v.f;
            if (c == 3)
                imm_emit_vertex();
        } else if (attr == 9 && c < 2) {
            s_gpu.imm_tex[c] = v.f;
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        if ((method - NV097_SET_VERTEX_DATA4UB) / 4 == 3)   /* diffuse */
            s_gpu.imm_diffuse = param;
        return 1;
    }
    return 0;
}
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;
    if (!inited) {
        inited = 1;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
#ifdef _WIN32
        {   /* RECOMP_PB_BACKEND=d3d11: src/d3d/nv2a_pb_d3d11.c */
            extern void nv2a_pb_d3d11_register_from_env(void);
            nv2a_pb_d3d11_register_from_env();
        }
#endif
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    if (vsh_method(method, param))
        return;
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_CLEAR_SURFACE:
        if (s_backend->on_zclear)
            s_backend->on_zclear(param);
        if (s_backend->on_clear)
            s_backend->on_clear(param);
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(start + i);
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;
    case NV097_SET_TEXTURE_OFFSET:
        /* A texture offset is a DMA-object offset, exactly like a surface or a
         * vertex array offset -- physical, and reachable only through the
         * contiguous window when it names contiguous memory. */
        s_gpu.tex.offset = dma_resolve(param);
        record_tex_reg(method, param);
        break;

    case NV097_SET_CONTEXT_DMA_SEMAPHORE:
        s_gpu.sema_dma = param;
        return;

    case NV097_SET_SEMAPHORE_OFFSET:
        s_gpu.sema_offset = param;
        s_gpu.sema_offset_set = 1;
        return;

    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: {
        /* The semaphore offset is a DMA-object offset like every other one
         * here, so it goes through the same resolution. The DMA object's own
         * base is not read (no RAMHT/PRAMIN walk yet); for XDK D3D it covers
         * physical memory from 0, which is what dma_resolve assumes, and the
         * first release is logged so a title where that is false says so. */
        uint32_t va = dma_resolve(s_gpu.sema_offset);
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();

        if (!s_gpu.sema_offset_set) {
            /* No offset yet: nowhere honest to write. */
            note_unhandled(method, param);
            return;
        }
        if (surface_hits_image(va, 4)) {
            static int said;
            if (!said++)
                fprintf(stderr, "  [GPU] REFUSING semaphore release to 0x%08X"
                        " (offset 0x%08X): inside the loaded image\n",
                        va, s_gpu.sema_offset);
            return;
        }
        *(volatile uint32_t *)(mem + va) = param;
        s_sema_last_va = va;
        s_sema_last_value = param;
        if (s_sema_releases++ == 0 || getenv("RECOMP_PB_SEMA_TRACE")) {
            static unsigned shown;
            if (shown++ < 16) {
                fprintf(stderr, "  [GPU] semaphore release 0x%08X -> 0x%08X"
                        " (dma 0x%08X offset 0x%08X)\n", param, va,
                        s_gpu.sema_dma, s_gpu.sema_offset);
                fflush(stderr);
            }
        }
        return;
    }

    case NV097_SET_FLIP_READ:
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        return;

    case NV097_FLIP_STALL:
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        /* Present the frame just finished. The buffer the title has
         * finished is the one the last batch drew into, which is what
         * drawn_offset holds and why it exists: by the flip, color_offset has
         * already moved to the next buffer. */
        if (s_backend->on_flip)
            s_backend->on_flip(s_gpu.drawn_offset ? s_gpu.drawn_offset
                                                  : s_gpu.color_offset,
                               s_gpu.pitch);
        /* RECOMP_FLIP_LOG=1: every flip, with the batches drawn since the last. */
        {
            static int on = -1;
            static uint32_t prev_b, prev_t;
            if (on < 0)
                on = getenv("RECOMP_FLIP_LOG") != NULL;
            if (on) {
                uint32_t b = s_gpu.batches_textured + s_gpu.batches_no_uv
                           + s_gpu.batches_no_tex;
                uint32_t done = s_gpu.drawn_offset ? s_gpu.drawn_offset
                                                   : s_gpu.color_offset;
                fprintf(stderr, "[GPU] flip %u %lu ms fence 0x%08X batches %u"
                        " tex %u presents 0x%08X\n", s_gpu.flips,
                        xbox_log_ms(), s_sema_last_value, b - prev_b,
                        s_gpu.batches_textured - prev_t, done);
                prev_b = b; prev_t = s_gpu.batches_textured;
            }
        }
        /* The first few always, so a log says whether flips happen at all
         * and which surface each one put on screen; more with VERBOSE. */
        {
            static unsigned n;
            unsigned limit = getenv("RECOMP_PB_EXEC_VERBOSE") ? 8u : 4u;
            if (n++ < limit) {
                uint32_t done = s_gpu.drawn_offset ? s_gpu.drawn_offset
                                                   : s_gpu.color_offset;
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u presents"
                        " 0x%08X (-> 0x%08X)\n", s_gpu.flips, s_gpu.flip_read,
                        s_gpu.flip_write, done, done ? dma_resolve(done) : 0u);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_TEXTURE_FORMAT:
        s_gpu.tex.color = (param >> 8) & 0xFF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V (nv2a_regs.h: 0x00F00000 / 0x0F000000). It has to:
         * SET_TEXTURE_IMAGE_RECT describes a linear image, and a title that
         * only uses swizzled textures never sends one -- this title sends it
         * once and sets a format 3,176 times. Without this the width and
         * height stayed zero and nothing was ever sampled. */
        if (tex_size_from_format((param >> 8) & 0xFF)) {
            s_gpu.tex.width  = 1u << ((param >> 20) & 0xF);
            s_gpu.tex.height = 1u << ((param >> 24) & 0xF);
        }
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis. 1 is wrap, 3 is clamp-to-edge; the rest (mirror,
         * border) fall back to clamp, which is wrong at an edge rather than
         * wrong everywhere. */
        s_gpu.tex.addr_u =  param        & 0xF;
        s_gpu.tex.addr_v = (param >>  8) & 0xF;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL1:
        /* Pitch lives in the top half. Only meaningful for a linear format; a
         * swizzled texture has no pitch because it has no rows. */
        s_gpu.tex.pitch = param >> 16;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_IMAGE_RECT:
        s_gpu.tex.width  = param >> 16;
        s_gpu.tex.height = param & 0xFFFF;
        record_tex_reg(method, param);
        break;

    default:
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (vsh_inline_attr(method, param), !imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    vsh_report();
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] semaphore: %u releases, last 0x%08X at 0x%08X\n",
            s_sema_releases, s_sema_last_value, s_sema_last_va);
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }    fflush(stderr);
}
