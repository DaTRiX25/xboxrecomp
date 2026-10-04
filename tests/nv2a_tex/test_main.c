/* The texture helpers every path shares (nv2a_backend_common.h): cube-map
 * face selection and face stride, and SET_TEXTURE_FILTER's linear rule. */
#include <stdio.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define NEAR(a, b) ((a) - (b) < 1e-6f && (b) - (a) < 1e-6f)

int main(void)
{
    static const float dir[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
    };
    float s, t;
    uint32_t f;

    /* Each axis picks its face, at the face's centre. */
    for (f = 0; f < 6; f++) {
        CHECK(nv2a_cube_face(dir[f], &s, &t) == f);
        CHECK(NEAR(s, 0.5f) && NEAR(t, 0.5f));
    }
    /* +X: s runs along -z, t along -y (D3D's cube layout). */
    { const float c[3] = { 1.0f, -0.5f, -0.5f };
      CHECK(nv2a_cube_face(c, &s, &t) == 0);
      CHECK(NEAR(s, 0.75f) && NEAR(t, 0.75f)); }
    /* +Y: s along x, t along z. */
    { const float c[3] = { 0.5f, 1.0f, -0.5f };
      CHECK(nv2a_cube_face(c, &s, &t) == 2);
      CHECK(NEAR(s, 0.75f) && NEAR(t, 0.25f)); }
    /* -Z: s along -x. */
    { const float c[3] = { 0.5f, 0.0f, -1.0f };
      CHECK(nv2a_cube_face(c, &s, &t) == 5);
      CHECK(NEAR(s, 0.25f) && NEAR(t, 0.5f)); }
    /* The zero vector does not divide by zero. */
    { const float c[3] = { 0, 0, 0 };
      nv2a_cube_face(c, &s, &t);
      CHECK(NEAR(s, 0.5f) && NEAR(t, 0.5f)); }

    /* Face stride: every level of a face, 128-aligned. A8R8G8B8 (0x06,
     * swizzled) 4x4, three levels: (16 + 4 + 1) * 4 = 84 -> 128. 16x16 one
     * level: 1024. DXT1 (0x0C) 8x8, four levels: 32 + 8 + 8 + 8 = 56 -> 128. */
    CHECK(nv2a_tex_face_stride(0x06, 4, 4, 0, 3) == 128);
    CHECK(nv2a_tex_face_stride(0x06, 16, 16, 0, 1) == 1024);
    CHECK(nv2a_tex_face_stride(0x0C, 8, 8, 0, 4) == 128);
    CHECK(nv2a_tex_face_stride(0x06, 32, 32, 0, 6) == 5504);
    /* Linear (LIN_A8R8G8B8 0x12): pitch * height. */
    CHECK(nv2a_tex_face_stride(0x12, 10, 10, 64, 1) == 640);

    /* SET_TEXTURE_FILTER: MAG [27:24], MIN [21:16]. */
    CHECK(!nv2a_tex_filter_linear(0x01010000u, 1) && !nv2a_tex_filter_linear(0x01010000u, 0));
    CHECK(nv2a_tex_filter_linear(0x02000000u, 1));      /* MAG tent */
    CHECK(nv2a_tex_filter_linear(0x04000000u, 1));      /* MAG convolution */
    CHECK(nv2a_tex_filter_linear(0x00020000u, 0));      /* MIN tent LOD 0 */
    CHECK(nv2a_tex_filter_linear(0x00060000u, 0));      /* MIN tent-tent */
    CHECK(!nv2a_tex_filter_linear(0x00050000u, 0));     /* MIN box-tent */
    CHECK(nv2a_tex_filter_mip(0x00060000u) == 2 && nv2a_tex_filter_mip(0x00030000u) == 1);
    CHECK(NEAR(nv2a_tex_lod_bias(0x1F00u), -1.0f) && NEAR(nv2a_tex_lod_bias(0x0080u), 0.5f));

    puts("nv2a_tex: all passed");
    return 0;
}
