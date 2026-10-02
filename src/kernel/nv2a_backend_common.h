/**
 * NV2A state logic shared by the pushbuffer backends.
 *
 * Everything here is API-independent: it turns NV2A register values and
 * guest memory into plain numbers and texels, and leaves the graphics API to
 * the backend. The D3D11 backend (src/d3d/nv2a_pb_d3d11.c) and the CPU
 * rasteriser (nv2a_pb_exec.c) both use it, so a fix to a texel format, a
 * blend factor or a stage decode lands in both at once, and a new backend
 * (Metal) starts from the same tables.
 *
 * Builds on every platform: no OS or graphics headers. The per-texel and
 * per-vertex helpers are static inline because the CPU path calls them per
 * pixel.
 */
#ifndef NV2A_BACKEND_COMMON_H
#define NV2A_BACKEND_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../d3d/d3d8_swizzle.h"
#include "nv2a_pb_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Textures                                                                  */
/* ------------------------------------------------------------------------ */

#define NV2A_TEX_P8 0x0Bu               /* SZ_I8_A8R8G8B8, through a palette */
#define NV2A_TEX_MAGENTA 0xFFFF00FFu    /* what an undecodable texel reads as */

/* The linear format with the same texel encoding as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06)
 * and LIN_A8R8G8B8 (0x12) are the same four bytes in the same order, so the
 * whole difference is the address calculation, and one decoder serves both.
 * Pairs read off the table in d3d8_xbox.h. */
static inline uint32_t nv2a_tex_linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;             /* L8        -> LIN_L8        */
    case 0x02: return 0x10;             /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;             /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;             /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;             /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;             /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;             /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;             /* A8        -> LIN_A8        */
    case 0x01: return 0x1B;             /* AY8       -> LIN_AY8       */
    case 0x1A: return 0x20;             /* A8Y8      -> LIN_A8Y8      */
    case 0x27: return 0x37;             /* R6G5B5    -> LIN_R6G5B5    */
    case 0x28: return 0x17;             /* G8B8      -> LIN_G8B8      */
    case 0x32: return 0x35;             /* Y16       -> LIN_Y16       */
    case 0x38: return 0x3D;             /* R5G5B5A1  -> LIN_R5G5B5A1  */
    case 0x39: return 0x3E;             /* R4G4B4A4  -> LIN_R4G4B4A4  */
    case 0x3A: return 0x3F;             /* A8B8G8R8  -> LIN_A8B8G8R8  */
    case 0x3B: return 0x40;             /* B8G8R8A8  -> LIN_B8G8R8A8  */
    case 0x3C: return 0x41;             /* R8G8B8A8  -> LIN_R8G8B8A8  */
    default:   return fmt;              /* already linear, P8, or unhandled */
    }
}

/* Bytes per texel of a linear format nv2a_tex_decode_texel reads (P8
 * included), 0 if it has no decoder. */
static inline uint32_t nv2a_tex_texel_bytes(uint32_t lin)
{
    switch (lin) {
    case 0x12: case 0x1E: case 0x3F: case 0x40: case 0x41: return 4;
    case 0x10: case 0x1C: case 0x11: case 0x1D: case 0x24: case 0x25:
    case 0x20: case 0x37: case 0x35: case 0x3D: case 0x3E: case 0x17: return 2;
    case 0x13: case 0x1F: case 0x1B: case 0x0B: return 1;
    default: return 0;
    }
}

/* Whether the size comes from the format register (swizzled and DXT, power
 * of two, mipmapped) rather than from the image-rect register (linear). */
static inline int nv2a_tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

/* Level l's size: halved per level, never below 1. */
static inline uint32_t nv2a_tex_level_dim(uint32_t d, uint32_t l)
{
    d >>= l;
    return d ? d : 1u;
}

/* Bytes of guest memory a w x h texture of `levels` levels occupies, 0 if it
 * cannot be decoded. Swizzled and DXT levels are packed one after another;
 * a linear texture is one level at `pitch` bytes per row. The caller checks
 * validity and its own size limit first. */
uint32_t nv2a_tex_extent(uint32_t fmt, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t levels);

/* FNV-style over n bytes, four independent lanes so it runs at memory
 * speed. h is the seed (NV2A_TEX_HASH_SEED for a fresh hash). */
#define NV2A_TEX_HASH_SEED 0xCBF29CE484222325ull
uint64_t nv2a_tex_hash(const uint8_t *p, uint32_t n, uint64_t h);

static inline uint8_t nv2a_clamp255(int x)
{
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

/* One texel as A8R8G8B8 into *argb, from texel index u of the row at p
 * (linear formats; for a swizzled one p is the image base and u the Morton
 * index). P8 reads pal[0..pal_len), magenta for a missing palette or an
 * index past it. Returns 0, *argb untouched, for a format with no decoder. */
static inline int nv2a_tex_decode_texel(uint32_t fmt, const uint8_t *p,
                                        uint32_t u, const uint32_t *pal,
                                        uint32_t pal_len, uint32_t *argb)
{
    uint32_t t;
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
    case 0x3F:                                      /* LIN_A8B8G8R8 */
        t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    case 0x40:                                      /* LIN_B8G8R8A8 */
        t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (((t >> 8) & 0xFFu) << 16)
              | (((t >> 16) & 0xFFu) << 8) | ((t >> 24) & 0xFFu);
        return 1;
    case 0x41:                                      /* LIN_R8G8B8A8 */
        t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;

    /* 16-bit. */
    case 0x10:                                      /* LIN_A1R5G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (d3d8_expand_channel((t >> 10) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x1C:                                      /* LIN_X1R5G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 10) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x11:                                      /* LIN_R5G6B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 11) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x3F, 6) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x1D:                                      /* LIN_A4R4G4B4 */
        t = ((const uint16_t *)p)[u];
        *argb = (d3d8_expand_channel((t >> 12) & 0xF, 4) << 24)
              | (d3d8_expand_channel((t >> 8) & 0xF, 4) << 16)
              | (d3d8_expand_channel((t >> 4) & 0xF, 4) << 8)
              |  d3d8_expand_channel(t & 0xF, 4);
        return 1;

    /* 8-bit. */
    case 0x13:                                      /* LIN_L8 */
        t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;
    case 0x1B:                                      /* LIN_AY8: Y in all four */
        *argb = (uint32_t)p[u] * 0x01010101u;
        return 1;
    case 0x0B:                                      /* P8, through the palette */
        t = p[u];
        *argb = (pal && t < pal_len) ? pal[t] : NV2A_TEX_MAGENTA;
        return 1;

    /* 16-bit, the rest of the D3D8 set. */
    case 0x20:                                      /* LIN_A8Y8: Y low, A high */
        t = ((const uint16_t *)p)[u];
        *argb = ((t >> 8) << 24) | (t & 0xFFu) * 0x010101u;
        return 1;
    case 0x17:                                      /* LIN_G8B8 (and V8U8) */
        /* xemu's table loads it as GL_RG8 (low byte red, high byte green)
         * and swizzles to (R, G, R, G): so B and R are the low byte, G and
         * A the high one. A bump-map stage reads it back from b and g. */
        t = ((const uint16_t *)p)[u];
        *argb = ((t >> 8) << 24) | ((t & 0xFFu) << 16) | ((t >> 8) << 8)
              | (t & 0xFFu);
        return 1;
    case 0x35:                                      /* LIN_Y16: top byte */
        t = ((const uint16_t *)p)[u] >> 8;
        *argb = 0xFF000000u | t * 0x010101u;
        return 1;
    case 0x37:                                      /* LIN_R6G5B5 */
        t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (d3d8_expand_channel((t >> 10) & 0x3F, 6) << 16)
              | (d3d8_expand_channel((t >> 5) & 0x1F, 5) << 8)
              |  d3d8_expand_channel(t & 0x1F, 5);
        return 1;
    case 0x3D:                                      /* LIN_R5G5B5A1 */
        t = ((const uint16_t *)p)[u];
        *argb = ((t & 1u) ? 0xFF000000u : 0u)
              | (d3d8_expand_channel((t >> 11) & 0x1F, 5) << 16)
              | (d3d8_expand_channel((t >> 6) & 0x1F, 5) << 8)
              |  d3d8_expand_channel((t >> 1) & 0x1F, 5);
        return 1;
    case 0x3E:                                      /* LIN_R4G4B4A4 */
        t = ((const uint16_t *)p)[u];
        *argb = (d3d8_expand_channel(t & 0xF, 4) << 24)
              | (d3d8_expand_channel((t >> 12) & 0xF, 4) << 16)
              | (d3d8_expand_channel((t >> 8) & 0xF, 4) << 8)
              |  d3d8_expand_channel((t >> 4) & 0xF, 4);
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes: how a title hands over a
     * decoded video frame. The chroma pair is shared between an even texel
     * and the one after it, so the group is found by masking the bottom bit
     * of the index. BT.601, the coefficients the D3D8 upload path uses. */
    case 0x24:                                      /* YUY2 */
    case 0x25: {                                    /* UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        *argb = 0xFF000000u
              | ((uint32_t)nv2a_clamp255((298 * c + 409 * cv + 128) >> 8) << 16)
              | ((uint32_t)nv2a_clamp255((298 * c - 100 * cu - 208 * cv + 128) >> 8) << 8)
              |  (uint32_t)nv2a_clamp255((298 * c + 516 * cu + 128) >> 8);
        return 1;
    }

    default:
        return 0;
    }
}

/* Texels in levels 0..levels-1 of a w x h texture. */
size_t nv2a_tex_texels(uint32_t w, uint32_t h, uint32_t levels);

/* One swizzled or DXT level of w x h at mem into out (A8R8G8B8, row-major,
 * 0 for a texel with no decoder); returns the guest bytes it took, which is
 * where the next level starts. */
uint32_t nv2a_tex_decode_level(const uint8_t *mem, uint32_t fmt, uint32_t w,
                               uint32_t h, const uint32_t *pal,
                               uint32_t pal_len, uint32_t *out);

/* A whole texture into out (nv2a_tex_texels() entries, A8R8G8B8): level 0
 * (w x h), then each further level packed after it. Swizzled and DXT
 * formats read `levels` levels from mem; a linear one reads one level at
 * `pitch` bytes per row. */
void nv2a_tex_decode(const uint8_t *mem, uint32_t fmt, uint32_t w, uint32_t h,
                     uint32_t pitch, uint32_t levels, const uint32_t *pal,
                     uint32_t pal_len, uint32_t *out);

/* ------------------------------------------------------------------------ */
/* Texture stages                                                            */
/* ------------------------------------------------------------------------ */

/* One texture stage as the registers program it. */
struct nv2a_stage {
    uint32_t offset, color, width, height, pitch, addr_u, addr_v;
    uint32_t palette, pal_len;  /* P8: resolved palette address, entries */
    uint32_t filter;            /* SET_TEXTURE_FILTER as written */
    uint32_t levels;            /* mip levels (1 for linear, or without
                                 * NV2A_STAGE_MIPS) */
    uint32_t lod_min, lod_max;  /* CONTROL0 LOD clamps, within levels    */
    int      valid;
    int      on;        /* CONTROL0 enable as used (never-set stage 0 counts on) */
    int      mode;      /* 0 none, 1 2D, 4 pass-through, other: sampled as 2D */
    int      raw_mode;  /* the shader program's mode before the gating */
};

#define NV2A_STAGE_MIPS        1u   /* count mip levels as xemu does */
#define NV2A_STAGE_P8_PALETTE  2u   /* P8 is valid only with a palette */
#define NV2A_STAGE_PITCH_LINEAR 4u  /* pitch 0 for a swizzled or DXT texture */

typedef uint32_t (*nv2a_dma_resolve_fn)(uint32_t offset);

/* Stage n from its 16 registers r (SET_TEXTURE_OFFSET 0x1B00 + 0x40 * n
 * onwards) and their written flags, with the shader-stage program
 * (shader_set false: stage 0 2D, the rest off). mode is the program's mode,
 * 0 where it asks for a texture the stage cannot give (disabled or
 * invalid; pass-through, 4, needs none). */
void nv2a_stage_decode(int n, const uint32_t *r, const uint8_t *set,
                       int shader_set, uint32_t shader_prog,
                       nv2a_dma_resolve_fn resolve, unsigned flags,
                       struct nv2a_stage *t);

/* ------------------------------------------------------------------------ */
/* Vertex attributes                                                         */
/* ------------------------------------------------------------------------ */

/* SET_VERTEX_DATA_ARRAY_FORMAT type codes (d3d8_vsh.h has the same values as
 * NV2A_VTX_*, behind <d3d11.h>). */
#define NV2A_VT_D3DCOLOR 0u
#define NV2A_VT_S1       1u     /* signed short, normalised */
#define NV2A_VT_FLOAT    2u
#define NV2A_VT_UBYTE    4u     /* unsigned byte, normalised */
#define NV2A_VT_S32K     5u     /* signed short, as is */
#define NV2A_VT_CMP      6u     /* 11:11:10 signed, normalised */

/* One attribute of `size` components of `type` at p as float4, missing
 * components (0,0,0,1). Returns 0 for a type with no decoder (out is then
 * (0,0,0,1)). Decoded the way xemu hands them to GL. */
static inline int nv2a_vtx_decode(uint32_t type, uint32_t size,
                                  const uint8_t *p, float out[4])
{
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case NV2A_VT_D3DCOLOR:
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case NV2A_VT_FLOAT:
        for (i = 0; i < size && i < 4; i++)
            memcpy(&out[i], p + 4 * i, 4);
        return 1;
    case NV2A_VT_UBYTE:
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case NV2A_VT_S1:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            float f;
            memcpy(&s, p + 2 * i, 2);
            f = (float)s / 32767.0f;
            out[i] = f < -1.0f ? -1.0f : f;
        }
        return 1;
    case NV2A_VT_S32K:
        for (i = 0; i < size && i < 4; i++) {
            int16_t s;
            memcpy(&s, p + 2 * i, 2);
            out[i] = (float)s;
        }
        return 1;
    case NV2A_VT_CMP: {
        uint32_t w;
        int32_t x, y, z;
        memcpy(&w, p, 4);
        x = (int32_t)(w << 21) >> 21;
        y = (int32_t)(w << 10) >> 21;
        z = (int32_t)w >> 22;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Primitives and programs                                                   */
/* ------------------------------------------------------------------------ */

enum nv2a_topology {
    NV2A_TOPO_NONE = 0,
    NV2A_TOPO_POINTS,
    NV2A_TOPO_LINES,
    NV2A_TOPO_TRIANGLES
};

/* Point, line and triangle lists from any NV097 primitive (NV_PRIM_*):
 * the n indices at v, each rebased by -base, into out (room for 3 * n).
 * Returns the index count and sets *topo; 0 and NV2A_TOPO_NONE for an
 * unknown primitive. Strips keep their winding, loops close. */
uint32_t nv2a_prim_to_list(uint32_t prim, const uint16_t *v, uint32_t n,
                           uint32_t base, uint32_t *out,
                           enum nv2a_topology *topo);

/* FNV-1a over the vertex program's slots, from v->start through the slot
 * with the FINAL bit (or the end). *len, if given, gets the slot count. */
uint32_t nv2a_vsh_program_hash(const struct nv2a_pb_vsh *v, uint32_t *len);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_BACKEND_COMMON_H */
