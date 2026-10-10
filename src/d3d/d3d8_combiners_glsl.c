/**
 * NV2A register combiners -> GLSL fragment shader for Vulkan (see
 * d3d8_glsl.h). Line for line d3d8_combiners_msl.c; only the syntax differs
 * (textures are set-1 bindings, texture() with a bias, discard).
 */

#include "d3d8_glsl.h"
#include <stdio.h>
#include <string.h>

/* The shader mirrors nv2a_rc_eval_regs (src/kernel/nv2a_combiner.c), as the
 * MSL does; see the comment block at the top of d3d8_combiners_msl.c. */

#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/* An input as vec3 (RGB portion) or float (alpha portion), before mapping. */
static void input_src(const NV2ACombinerInput *in, int alpha_portion,
                      char *out, size_t n)
{
    int reg = (int)in->reg & 0xF;
    if (alpha_portion)
        snprintf(out, n, "x%d.%s", reg, in->alpha_rep ? "a" : "b");
    else
        snprintf(out, n, "x%d.%s", reg, in->alpha_rep ? "aaa" : "rgb");
}

static const char s_rc_helpers[] =
    "float rc_map(float x, int m) {\n"
    "    if (m == 0) return max(x, 0.0);\n"
    "    if (m == 1) return 1.0 - saturate(x);\n"
    "    if (m == 2) return 2.0 * max(x, 0.0) - 1.0;\n"
    "    if (m == 3) return 1.0 - 2.0 * max(x, 0.0);\n"
    "    if (m == 4) return max(x, 0.0) - 0.5;\n"
    "    if (m == 5) return 0.5 - max(x, 0.0);\n"
    "    if (m == 6) return x;\n"
    "    return -x;\n"
    "}\n"
    "vec3 rc_map3(vec3 x, int m) {\n"
    "    return vec3(rc_map(x.r, m), rc_map(x.g, m), rc_map(x.b, m));\n"
    "}\n"
    "float rc_op(float x, int op) {\n"
    "    if (op == 1) x -= 0.5;\n"
    "    else if (op == 2) x *= 2.0;\n"
    "    else if (op == 3) x = (x - 0.5) * 2.0;\n"
    "    else if (op == 4) x *= 4.0;\n"
    "    else if (op == 6) x *= 0.5;\n"
    "    return clamp(x, -1.0, 1.0);\n"
    "}\n"
    "vec3 rc_op3(vec3 x, int op) {\n"
    "    return vec3(rc_op(x.r, op), rc_op(x.g, op), rc_op(x.b, op));\n"
    "}\n";

int d3d8_combiners_generate_glsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    static const char *const in_name[4] = { "A", "B", "C", "D" };
    char src[32];
    int off = 0;
    int i, s, k;

    EMIT("#version 450\n"
         "/* Auto-generated NV2A register combiners (GLSL, Vulkan) */\n");
    EMIT("%s", D3D8_GLSL_SATURATE);
    EMIT("%s", D3D8_GLSL_PSCONSTS);
    EMIT("%s", D3D8_GLSL_FS_IN);
    EMIT("layout(location = 0) out vec4 o_color;\n");
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE || m == NV2A_TEXMODE_PASSTHRU
                || m == NV2A_TEXMODE_CLIPPLANE)
            continue;
        EMIT("layout(set = 1, binding = %d) uniform %s tex%d;\n", i,
             m == NV2A_TEXMODE_3D ? "sampler3D" :
             m == NV2A_TEXMODE_CUBEMAP ? "samplerCube" : "sampler2D", i);
    }
    EMIT("%s", s_rc_helpers);

    EMIT("void main() {\n");
    for (k = 0; k < 16; k++)
        EMIT("    vec4 x%d = vec4(0.0);\n", k);
    if (state->fog_input)
        EMIT("    x3 = vec4(pc.fog_color.rgb, saturate(i_fog));\n");
    else
        EMIT("    x3 = pc.fog_color;\n");
    EMIT("    x4 = i_d0;\n");
    EMIT("    x5 = i_d1;\n");

    /* ---- Texture stages ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE) {
            /* (0, 0, 0, 1), as xemu: see NV2ATextureMode */
            EMIT("    x%d = vec4(0.0, 0.0, 0.0, 1.0);\n", 8 + i);
            continue;
        }
        if (m == NV2A_TEXMODE_CLIPPLANE)
            continue;
        if (m == NV2A_TEXMODE_PASSTHRU) {
            EMIT("    x%d = saturate(i_t%d);\n", 8 + i, i);
            continue;
        }
        if (m == NV2A_TEXMODE_3D || m == NV2A_TEXMODE_CUBEMAP) {
            EMIT("    x%d = texture(tex%d, i_t%d.xyz, pc.tex_bias[%d]);\n", 8 + i, i, i, i);
        } else {
            EMIT("    {\n");
            EMIT("        vec2 uv = i_t%d.xy;\n", i);
            if (m == NV2A_TEXMODE_2D)
                /* As the MSL: only true 2D stages divide by q. */
                EMIT("        if (pc.tex_mode[%d] == 1u && i_t%d.w != 0.0 && i_t%d.w != 1.0)"
                     " uv /= i_t%d.w;\n", i, i, i, i);
            EMIT("        x%d = texture(tex%d, uv * pc.tex_scale[%d].xy, pc.tex_bias[%d]);\n",
                 8 + i, i, i, i);
            EMIT("    }\n");
        }
        EMIT("    if (pc.alpha_only[%d] != 0u) x%d.rgb = vec3(1.0);\n", i, 8 + i);
    }
    EMIT("    x12.a = x8.a;\n");

    /* ---- General combiner stages ---- */
    for (s = 0; s < state->num_stages; s++) {
        const NV2ACombinerStage *st = &state->stages[s];
        const NV2ACombinerOutput *ro = &st->rgb_output, *ao = &st->alpha_output;

        EMIT("    {   /* stage %d */\n", s);
        EMIT("        x1 = pc.c0[%d]; x2 = pc.c1[%d];\n",
             state->uniq_c0 ? s : 0, state->uniq_c1 ? s : 0);
        if (ro->mux_sum || ao->mux_sum) {
            if (state->mux_msb)
                EMIT("        bool sel = x12.a >= 0.5;\n");
            else
                EMIT("        bool sel = ((uint(saturate(x12.a) * 255.0 + 0.5)) & 1u) != 0u;\n");
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->rgb_input[k], 0, src, sizeof src);
            EMIT("        vec3 %s = rc_map3(%s, %d);\n",
                 in_name[k], src, (int)st->rgb_input[k].mapping);
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->alpha_input[k], 1, src, sizeof src);
            EMIT("        float a%s = rc_map(%s, %d);\n",
                 in_name[k], src, (int)st->alpha_input[k].mapping);
        }
        EMIT("        vec3 ab = %s;\n", ro->ab_dot ? "vec3(dot(A, B))" : "A * B");
        EMIT("        vec3 cd = %s;\n", ro->cd_dot ? "vec3(dot(C, D))" : "C * D");
        EMIT("        vec3 sum = %s;\n", ro->mux_sum ? "sel ? cd : ab" : "ab + cd");
        EMIT("        ab = rc_op3(ab, %d); cd = rc_op3(cd, %d); sum = rc_op3(sum, %d);\n",
             (int)ro->output_map, (int)ro->output_map, (int)ro->output_map);
        EMIT("        float aab = aA * aB, acd = aC * aD;\n");
        EMIT("        float asum = %s;\n", ao->mux_sum ? "sel ? acd : aab" : "aab + acd");
        if (ro->ab_dst)  EMIT("        x%d.rgb = ab;\n",  (int)ro->ab_dst);
        if (ro->cd_dst)  EMIT("        x%d.rgb = cd;\n",  (int)ro->cd_dst);
        if (ro->sum_dst) EMIT("        x%d.rgb = sum;\n", (int)ro->sum_dst);
        if (ao->ab_dst)  EMIT("        x%d.a = rc_op(aab, %d);\n",  (int)ao->ab_dst,  (int)ao->output_map);
        if (ao->cd_dst)  EMIT("        x%d.a = rc_op(acd, %d);\n",  (int)ao->cd_dst,  (int)ao->output_map);
        if (ao->sum_dst) EMIT("        x%d.a = rc_op(asum, %d);\n", (int)ao->sum_dst, (int)ao->output_map);
        if (ro->ab_blue_to_alpha && ro->ab_dst) EMIT("        x%d.a = ab.b;\n", (int)ro->ab_dst);
        if (ro->cd_blue_to_alpha && ro->cd_dst) EMIT("        x%d.a = cd.b;\n", (int)ro->cd_dst);
        EMIT("    }\n");
    }

    /* ---- Final combiner ---- */
    EMIT("    vec4 res;\n");
    EMIT("    {   /* final combiner */\n");
    EMIT("        x1 = pc.fc0; x2 = pc.fc1;\n");
    {
        /* E and F first, then the special registers, then A..D and G,
         * which can read them. */
        static const char *const fname[7] = { "A", "B", "C", "D", "E", "F", "G" };
        static const int order[7] = { 4, 5, -1, 0, 1, 2, 3 };
        for (i = 0; i < 7; i++) {
            const NV2ACombinerInput *in;
            k = order[i];
            if (k < 0) {
                /* COMPLEMENT_V1 0x40, COMPLEMENT_R0 0x20 (xemu psh_regs.h). */
                EMIT("        vec3 fv1 = %ssaturate(x5.rgb);\n",
                     (state->final_flags & 0x40) ? "1.0 - " : "");
                EMIT("        vec3 fr0 = %ssaturate(x12.rgb);\n",
                     (state->final_flags & 0x20) ? "1.0 - " : "");
                EMIT("        x14 = vec4(%s(fv1 + fr0), 0.0);\n",
                     (state->final_flags & 0x80) ? "saturate" : "");
                EMIT("        x15 = vec4(E * F, 0.0);\n");
                continue;
            }
            in = &state->final_input[k];
            input_src(in, 0, src, sizeof src);
            EMIT("        vec3 %s = %ssaturate(%s);\n", fname[k],
                 ((int)in->mapping & 1) ? "1.0 - " : "", src);
        }
        input_src(&state->final_input[6], 1, src, sizeof src);
        EMIT("        float G = %ssaturate(%s);\n",
             ((int)state->final_input[6].mapping & 1) ? "1.0 - " : "", src);
    }
    EMIT("        res = vec4(saturate(A * B + (1.0 - A) * C + D), G);\n");
    EMIT("    }\n");

    /* ---- Alpha test: D3DCMP numbering on 8-bit values ---- */
    EMIT("    if (pc.atest.z != 0u) {\n"
         "        float ref = uintBitsToFloat(pc.atest.x);\n"
         "        uint a = uint(saturate(res.a) * 255.0 + 0.5), r = uint(ref * 255.0 + 0.5);\n"
         "        bool ok = true;\n"
         "        uint f = pc.atest.y;\n"
         "        if      (f == 1u) ok = false;\n"
         "        else if (f == 2u) ok = a <  r;\n"
         "        else if (f == 3u) ok = a == r;\n"
         "        else if (f == 4u) ok = a <= r;\n"
         "        else if (f == 5u) ok = a >  r;\n"
         "        else if (f == 6u) ok = a != r;\n"
         "        else if (f == 7u) ok = a >= r;\n"
         "        if (!ok) discard;\n"
         "    }\n");
    EMIT("    o_color = res;\n");
    EMIT("}\n");
    return off;
}

#undef EMIT
