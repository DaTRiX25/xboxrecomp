/**
 * NV2A state logic shared by the pushbuffer backends: the out-of-line half
 * of nv2a_backend_common.h. No OS or graphics headers.
 */
#include "nv2a_backend_common.h"

uint32_t nv2a_tex_extent(uint32_t fmt, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t levels)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(fmt), tb, l, n = 0;
    if (bb || d3d8_format_is_swizzled(fmt)) {
        tb = bb ? 0 : nv2a_tex_texel_bytes(nv2a_tex_linear_twin(fmt));
        if (!bb && !tb)
            return 0;
        for (l = 0; l < levels; l++) {
            n += bb ? ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb : w * h * tb;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        return n;
    }
    tb = nv2a_tex_texel_bytes(fmt);
    if (!tb || !pitch)
        return 0;
    return pitch * (h - 1u) + w * tb;
}

uint64_t nv2a_tex_hash(const uint8_t *p, uint32_t n, uint64_t h)
{
    uint64_t l[4] = { h, h ^ 1u, h ^ 2u, h ^ 3u };
    uint32_t i;
    for (i = 0; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        l[0] = (l[0] ^ w[0]) * 0x100000001B3ull;
        l[1] = (l[1] ^ w[1]) * 0x100000001B3ull;
        l[2] = (l[2] ^ w[2]) * 0x100000001B3ull;
        l[3] = (l[3] ^ w[3]) * 0x100000001B3ull;
    }
    for (; i < n; i++)
        l[0] = (l[0] ^ p[i]) * 0x100000001B3ull;
    return (l[0] ^ (l[1] << 1) ^ (l[2] << 2) ^ (l[3] << 3)) * 0x100000001B3ull;
}

size_t nv2a_tex_texels(uint32_t w, uint32_t h, uint32_t levels)
{
    size_t n = 0;
    uint32_t l;
    for (l = 0; l < levels; l++)
        n += (size_t)nv2a_tex_level_dim(w, l) * nv2a_tex_level_dim(h, l);
    return n;
}

uint32_t nv2a_tex_decode_level(const uint8_t *mem, uint32_t fmt, uint32_t w,
                               uint32_t h, const uint32_t *pal,
                               uint32_t pal_len, uint32_t *out)
{
    uint32_t x, y, bb = d3d8_format_dxt_block_bytes(fmt);
    if (bb) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                d3d8_dxt_decode_texel(mem, fmt, x, y, w, &argb);
                out[(size_t)y * w + x] = argb;
            }
        return ((w + 3u) / 4u) * ((h + 3u) / 4u) * bb;
    } else {
        uint32_t mx, my, lin = nv2a_tex_linear_twin(fmt);
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t oy = swizzle_deposit(y, my);
            for (x = 0; x < w; x++) {
                uint32_t argb = 0;
                nv2a_tex_decode_texel(lin, mem, swizzle_deposit(x, mx) | oy,
                                      pal, pal_len, &argb);
                out[(size_t)y * w + x] = argb;
            }
        }
        return w * h * nv2a_tex_texel_bytes(lin);
    }
}

void nv2a_tex_decode(const uint8_t *mem, uint32_t fmt, uint32_t w, uint32_t h,
                     uint32_t pitch, uint32_t levels, const uint32_t *pal,
                     uint32_t pal_len, uint32_t *out)
{
    uint32_t x, y, l;
    if (nv2a_tex_size_from_format(fmt)) {
        for (l = 0; l < levels; l++) {
            uint32_t lw = nv2a_tex_level_dim(w, l), lh = nv2a_tex_level_dim(h, l);
            mem += nv2a_tex_decode_level(mem, fmt, lw, lh, pal, pal_len, out);
            out += (size_t)lw * lh;
        }
        return;
    }
    for (y = 0; y < h; y++) {
        const uint8_t *row = mem + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            uint32_t argb = 0;
            nv2a_tex_decode_texel(fmt, row, x, pal, pal_len, &argb);
            out[(size_t)y * w + x] = argb;
        }
    }
}
