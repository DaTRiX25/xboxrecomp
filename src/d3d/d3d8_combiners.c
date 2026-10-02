/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator - Implementation
 *
 * Translates Xbox NV2A register combiner configurations into HLSL pixel
 * shaders compiled for D3D11. See d3d8_combiners.h for the full model
 * description.
 *
 * Implementation overview:
 *
 * 1. STATE TRACKING
 *    The game sets combiner configuration through either:
 *    (a) SetPixelShader(DWORD token) - a packed DWORD encoding combiner
 *        count and texture modes, with actual stage config in render states
 *    (b) Direct render state writes (D3DRS_PSALPHAINPUTS0..7, etc.)
 *    We parse either path into an NV2ACombinerState structure.
 *
 * 2. HLSL GENERATION
 *    From the combiner state, we emit a complete HLSL pixel shader that:
 *    - Samples textures based on tex_mode per stage
 *    - Walks each active general combiner stage performing AB*CD math
 *    - Executes the final combiner (lerp + add)
 *    - Handles alpha test and fog
 *
 * 3. SHADER CACHE
 *    We hash the full NV2ACombinerState and maintain a fixed-size cache
 *    (128 entries) of compiled ID3D11PixelShader objects. Most Xbox games
 *    use fewer than 20 unique combiner configurations, so this is ample.
 *
 * 4. DRAW INTEGRATION
 *    d3d8_combiners_prepare_draw() is called before each draw. It checks
 *    if state is dirty, rebuilds/looks up the shader, uploads constants,
 *    and binds everything to the D3D11 pipeline.
 */

#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * Internal State
 * ================================================================ */

/** Current pixel shader token (0 = no combiner shader / fixed-function). */
static DWORD g_ps_token = 0;

/** Current parsed combiner state. */
static NV2ACombinerState g_combiner_state;

/** Dirty flag - set when any PS render state changes. */
static BOOL g_dirty = TRUE;

/** PS constant buffer (uploaded to GPU each draw). */
static ID3D11Buffer *g_combiner_cb = NULL;

/* ================================================================
 * Shader Cache
 *
 * Simple open-addressing hash table with linear probing.
 * 128 entries is generous - most games use <20 unique PS configs.
 * On a full table, the oldest entry is evicted (LRU approximation
 * via frame counter).
 * ================================================================ */

#define COMBINER_CACHE_SIZE 128

typedef struct CombinerCacheEntry {
    BOOL                in_use;
    uint32_t            hash;
    NV2ACombinerState   state;
    ID3D11PixelShader  *shader;
    uint32_t            last_used_frame;
} CombinerCacheEntry;

static CombinerCacheEntry g_cache[COMBINER_CACHE_SIZE];
static uint32_t g_frame_counter = 0;

/* ================================================================
 * Hashing
 *
 * FNV-1a over the combiner state structure. This is fast enough
 * for our purposes and produces good distribution.
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t combiner_state_hash(const NV2ACombinerState *state)
{
    return fnv1a_hash(state, sizeof(NV2ACombinerState));
}

static BOOL combiner_state_equal(const NV2ACombinerState *a,
                                 const NV2ACombinerState *b)
{
    return memcmp(a, b, sizeof(NV2ACombinerState)) == 0;
}

/* ================================================================
 * Color Helpers
 *
 * Convert D3DCOLOR (ARGB packed DWORD) to float4 (RGBA).
 * D3DCOLOR byte layout in memory: BGRA (little-endian ARGB).
 * ================================================================ */

static void d3dcolor_to_float4(DWORD color, float out[4])
{
    out[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    out[1] = ((color >>  8) & 0xFF) / 255.0f; /* G */
    out[2] = ((color >>  0) & 0xFF) / 255.0f; /* B */
    out[3] = ((color >> 24) & 0xFF) / 255.0f; /* A */
}

/* ================================================================
 * Combiner Input Parsing
 *
 * Each combiner input is 8 bits:
 *   [3:0] register select (NV2ACombinerRegister value)
 *   [4]   alpha channel (else RGB, or blue in an alpha portion)
 *   [7:5] input mapping mode (NV2AInputMapping value)
 *
 * The XDK's D3DRS_PS* render states hold the words the hardware
 * registers take, so both paths parse the same layout. It follows
 * xemu's psh.c and the CPU evaluator, rc_eval in nv2a_pb_exec.c.
 * ================================================================ */

static void parse_combiner_input(DWORD packed, NV2ACombinerInput *input)
{
    input->reg       = (NV2ACombinerRegister)(packed & 0xF);
    input->alpha_rep = (packed >> 4) & 1;
    input->mapping   = (NV2AInputMapping)((packed >> 5) & 0x7);
}

/** An input word: [31:24]=A [23:16]=B [15:8]=C [7:0]=D. */
static void parse_four_inputs(DWORD dword, NV2ACombinerInput inputs[4])
{
    parse_combiner_input((dword >> 24) & 0xFF, &inputs[0]); /* A */
    parse_combiner_input((dword >> 16) & 0xFF, &inputs[1]); /* B */
    parse_combiner_input((dword >>  8) & 0xFF, &inputs[2]); /* C */
    parse_combiner_input((dword >>  0) & 0xFF, &inputs[3]); /* D */
}

/** RGB output word: see NV2ACombinerOutput. */
static void parse_rgb_output(DWORD dword, NV2ACombinerOutput *output)
{
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->cd_dot     = (dword >> 12) & 1;
    output->ab_dot     = (dword >> 13) & 1;
    output->mux_sum    = (dword >> 14) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 15) & 0x7);
    output->cd_blue_to_alpha = (dword >> 18) & 1;
    output->ab_blue_to_alpha = (dword >> 19) & 1;
}

/** Alpha output word: the same destinations, mux [12], mapping [15:13]. */
static void parse_alpha_output(DWORD dword, NV2ACombinerOutput *output)
{
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->mux_sum    = (dword >> 12) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 13) & 0x7);
}

/* ================================================================
 * Token & Render State Parsing
 * ================================================================ */

void d3d8_combiners_from_regs(const uint32_t cicw[8], const uint32_t aicw[8],
                              const uint32_t cocw[8], const uint32_t aocw[8],
                              uint32_t control, uint32_t fcw0, uint32_t fcw1,
                              uint32_t stage_modes, NV2ACombinerState *state)
{
    int i;

    memset(state, 0, sizeof(*state));
    state->num_stages = (int)(control & 0xFF);
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    state->mux_msb = (control >> 8) & 1;
    state->uniq_c0 = (control >> 12) & 1;
    state->uniq_c1 = (control >> 16) & 1;

    /* Only the active stages count: the rest stay zero, so states that
     * differ only in unused stages share a shader. */
    for (i = 0; i < state->num_stages; i++) {
        parse_four_inputs(cicw[i], state->stages[i].rgb_input);
        parse_four_inputs(aicw[i], state->stages[i].alpha_input);
        parse_rgb_output(cocw[i], &state->stages[i].rgb_output);
        parse_alpha_output(aocw[i], &state->stages[i].alpha_output);
    }

    parse_combiner_input((fcw0 >> 24) & 0xFF, &state->final_input[0]); /* A */
    parse_combiner_input((fcw0 >> 16) & 0xFF, &state->final_input[1]); /* B */
    parse_combiner_input((fcw0 >>  8) & 0xFF, &state->final_input[2]); /* C */
    parse_combiner_input((fcw0 >>  0) & 0xFF, &state->final_input[3]); /* D */
    parse_combiner_input((fcw1 >> 24) & 0xFF, &state->final_input[4]); /* E */
    parse_combiner_input((fcw1 >> 16) & 0xFF, &state->final_input[5]); /* F */
    parse_combiner_input((fcw1 >>  8) & 0xFF, &state->final_input[6]); /* G */
    state->final_flags = fcw1 & 0xE0;

    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        state->tex_mode[i] = (NV2ATextureMode)((stage_modes >> (5 * i)) & 0x1F);
}

void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state)
{
    uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
    uint32_t control = rs[D3DRS_PSCOMBINERCOUNT];
    int i;

    for (i = 0; i < 8; i++) {
        cicw[i] = rs[D3DRS_PSRGBINPUTS0 + i];
        aicw[i] = rs[D3DRS_PSALPHAINPUTS0 + i];
        cocw[i] = rs[D3DRS_PSRGBOUTPUTS0 + i];
        aocw[i] = rs[D3DRS_PSALPHAOUTPUTS0 + i];
    }
    if ((control & 0xFF) == 0)
        control |= 1;
    d3d8_combiners_from_regs(cicw, aicw, cocw, aocw, control,
                             rs[D3DRS_PSFINALCOMBINERINPUTSABCD],
                             rs[D3DRS_PSFINALCOMBINERINPUTSEFG],
                             rs[D3DRS_PSTEXTUREMODES], state);

    /* The legacy path keeps the constants in the state */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        state->c0[i] = rs[D3DRS_PSCONSTANT0_0 + i];
        state->c1[i] = rs[D3DRS_PSCONSTANT1_0 + i];
    }
    state->final_c0 = state->c0[state->num_stages > 0 ? state->num_stages - 1 : 0];
    state->final_c1 = state->c1[state->num_stages > 0 ? state->num_stages - 1 : 0];
}

void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state)
{
    int i;

    d3d8_combiners_from_render_states(rs, state);

    /* The legacy token: [3:0] stage count, then 4 bits of NV2ATextureMode
     * per stage from bit 8, then flags. */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    for (i = 0; i < NV2A_MAX_TEXTURES; i++)
        state->tex_mode[i] = (NV2ATextureMode)((token >> (8 + 4 * i)) & 0xF);
    state->flags = (token >> 24) & 0xFF;
}

/* ================================================================
 * HLSL Code Generation
 *
 * The shader mirrors rc_eval in nv2a_pb_exec.c, so the GPU and CPU
 * paths agree:
 *   - registers x0..x15 by register code: 0 zero, 1 C0, 2 C1, 3 fog,
 *     4 V0, 5 V1, 8-11 T0-T3, 12 R0 (alpha starts as T0's), 13 R1,
 *     14 V1+R0 and 15 E*F (final combiner)
 *   - in each stage all inputs are read before any output is written;
 *     the RGB writes go AB, CD, sum, then alpha AB, CD, sum, then the
 *     blue-to-alpha copies
 *   - outputs are mapped, then clamped to [-1, 1]
 *   - the final combiner computes A*B + (1-A)*C + D with clamped,
 *     optionally inverted inputs, and alpha G
 *   - the alpha test compares 8-bit values
 * ================================================================ */

#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/* An input as float3 (RGB portion) or float (alpha portion), before
 * mapping. */
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
    "float3 rc_map3(float3 x, int m) {\n"
    "    return float3(rc_map(x.r, m), rc_map(x.g, m), rc_map(x.b, m));\n"
    "}\n"
    "float rc_op(float x, int op) {\n"
    "    if (op == 1) x -= 0.5;\n"
    "    else if (op == 2) x *= 2.0;\n"
    "    else if (op == 3) x = (x - 0.5) * 2.0;\n"
    "    else if (op == 4) x *= 4.0;\n"
    "    else if (op == 6) x *= 0.5;\n"
    "    return clamp(x, -1.0, 1.0);\n"
    "}\n"
    "float3 rc_op3(float3 x, int op) {\n"
    "    return float3(rc_op(x.r, op), rc_op(x.g, op), rc_op(x.b, op));\n"
    "}\n";

int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    static const char *const in_name[4] = { "A", "B", "C", "D" };
    char src[32];
    int off = 0;
    int i, s, k;

    /* ---- Textures ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE || m == NV2A_TEXMODE_PASSTHRU)
            continue;
        EMIT("%s tex%d : register(t%d);\n",
             m == NV2A_TEXMODE_3D ? "Texture3D" :
             m == NV2A_TEXMODE_CUBEMAP ? "TextureCube" : "Texture2D", i, i);
        EMIT("SamplerState samp%d : register(s%d);\n", i, i);
    }

    /* ---- Constants: NV2APSConstants ---- */
    EMIT("cbuffer CombinerCB : register(b0) {\n"
         "    float4 c0[8]; float4 c1[8]; float4 fc0; float4 fc1; float4 fog_color;\n"
         "    float alpha_ref; uint alpha_func; uint alpha_test_enable; uint fog_enable;\n"
         "    uint4 alpha_only; float4 tex_scale[4]; uint4 tex_mode;\n"
         "};\n");

    /* ---- Input: a prefix of d3d8_vsh.c's VS_OUT ---- */
    EMIT("struct PS_IN {\n"
         "    float4 pos : SV_POSITION;\n"
         "    float4 d0  : COLOR0;\n"
         "    float4 d1  : COLOR1;\n"
         "    float4 tc0 : TEXCOORD0;\n"
         "    float4 tc1 : TEXCOORD1;\n"
         "    float4 tc2 : TEXCOORD2;\n"
         "    float4 tc3 : TEXCOORD3;\n"
         "};\n");
    EMIT("%s", s_rc_helpers);

    EMIT("float4 main(PS_IN i) : SV_Target {\n");
    for (k = 0; k < 16; k++)
        EMIT("    float4 x%d = 0;\n", k);
    EMIT("    x3 = fog_color;\n");
    EMIT("    x4 = i.d0;\n");
    EMIT("    x5 = i.d1;\n");

    /* ---- Texture stages ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = (int)state->tex_mode[i];
        if (m == NV2A_TEXMODE_NONE)
            continue;
        if (m == NV2A_TEXMODE_PASSTHRU) {
            EMIT("    x%d = saturate(i.tc%d);\n", 8 + i, i);
            continue;
        }
        if (m == NV2A_TEXMODE_3D || m == NV2A_TEXMODE_CUBEMAP) {
            EMIT("    x%d = tex%d.Sample(samp%d, i.tc%d.xyz);\n", 8 + i, i, i, i);
        } else {
            EMIT("    {\n");
            EMIT("        float2 uv = i.tc%d.xy;\n", i);
            if (m == NV2A_TEXMODE_2D)
                EMIT("        if (i.tc%d.w != 0.0 && i.tc%d.w != 1.0) uv /= i.tc%d.w;\n",
                     i, i, i);
            EMIT("        x%d = tex%d.Sample(samp%d, uv * tex_scale[%d].xy);\n",
                 8 + i, i, i, i);
            EMIT("    }\n");
        }
        EMIT("    if (alpha_only[%d] != 0u) x%d.rgb = 1.0;\n", i, 8 + i);
    }
    EMIT("    x12.a = x8.a;\n");

    /* ---- General combiner stages ---- */
    for (s = 0; s < state->num_stages; s++) {
        const NV2ACombinerStage *st = &state->stages[s];
        const NV2ACombinerOutput *ro = &st->rgb_output, *ao = &st->alpha_output;

        EMIT("    {   /* stage %d */\n", s);
        EMIT("        x1 = c0[%d]; x2 = c1[%d];\n",
             state->uniq_c0 ? s : 0, state->uniq_c1 ? s : 0);
        if (ro->mux_sum || ao->mux_sum) {
            if (state->mux_msb)
                EMIT("        bool sel = x12.a >= 0.5;\n");
            else
                EMIT("        bool sel = (((uint)(saturate(x12.a) * 255.0 + 0.5)) & 1u) != 0u;\n");
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->rgb_input[k], 0, src, sizeof src);
            EMIT("        float3 %s = rc_map3(%s, %d);\n",
                 in_name[k], src, (int)st->rgb_input[k].mapping);
        }
        for (k = 0; k < 4; k++) {
            input_src(&st->alpha_input[k], 1, src, sizeof src);
            EMIT("        float a%s = rc_map(%s, %d);\n",
                 in_name[k], src, (int)st->alpha_input[k].mapping);
        }
        EMIT("        float3 ab = %s;\n", ro->ab_dot ? "(float3)dot(A, B)" : "A * B");
        EMIT("        float3 cd = %s;\n", ro->cd_dot ? "(float3)dot(C, D)" : "C * D");
        EMIT("        float3 sum = %s;\n", ro->mux_sum ? "sel ? cd : ab" : "ab + cd");
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
    EMIT("    float4 res;\n");
    EMIT("    {   /* final combiner */\n");
    EMIT("        x1 = fc0; x2 = fc1;\n");
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
                EMIT("        float3 fv1 = %ssaturate(x5.rgb);\n",
                     (state->final_flags & 0x40) ? "1.0 - " : "");
                EMIT("        float3 fr0 = %ssaturate(x12.rgb);\n",
                     (state->final_flags & 0x20) ? "1.0 - " : "");
                EMIT("        x14 = float4(%s(fv1 + fr0), 0.0);\n",
                     (state->final_flags & 0x80) ? "saturate" : "");
                EMIT("        x15 = float4(E * F, 0.0);\n");
                continue;
            }
            in = &state->final_input[k];
            input_src(in, 0, src, sizeof src);
            EMIT("        float3 %s = %ssaturate(%s);\n", fname[k],
                 ((int)in->mapping & 1) ? "1.0 - " : "", src);
        }
        input_src(&state->final_input[6], 1, src, sizeof src);
        EMIT("        float G = %ssaturate(%s);\n",
             ((int)state->final_input[6].mapping & 1) ? "1.0 - " : "", src);
    }
    EMIT("        res = float4(saturate(A * B + (1.0 - A) * C + D), G);\n");
    EMIT("    }\n");

    /* ---- Alpha test: D3DCMP numbering on 8-bit values ---- */
    EMIT("    if (alpha_test_enable != 0u) {\n"
         "        uint a = (uint)(saturate(res.a) * 255.0 + 0.5), r = (uint)(alpha_ref * 255.0 + 0.5);\n"
         "        bool ok = true;\n"
         "        if      (alpha_func == 1u) ok = false;\n"
         "        else if (alpha_func == 2u) ok = a <  r;\n"
         "        else if (alpha_func == 3u) ok = a == r;\n"
         "        else if (alpha_func == 4u) ok = a <= r;\n"
         "        else if (alpha_func == 5u) ok = a >  r;\n"
         "        else if (alpha_func == 6u) ok = a != r;\n"
         "        else if (alpha_func == 7u) ok = a >= r;\n"
         "        if (!ok) discard;\n"
         "    }\n");
    EMIT("    return res;\n");
    EMIT("}\n");
    return off;
}

#undef EMIT

/* ================================================================
 * Shader Compilation & Cache
 * ================================================================ */

static ID3D11PixelShader *compile_combiner_shader(const NV2ACombinerState *state)
{
    /* 16KB should be more than enough for any combiner shader */
    char hlsl[16384];
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    int len;

    len = d3d8_combiners_generate_hlsl(state, hlsl, sizeof(hlsl));
    if (len < 0) {
        fprintf(stderr, "NV2A combiners: HLSL generation failed (buffer overflow)\n");
        return NULL;
    }

    hr = D3DCompile(hlsl, (SIZE_T)len, "ps_combiner",
                    NULL, NULL, "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: HLSL compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors)
                       : "unknown error");
        /* Dump the generated source for debugging */
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End HLSL ---\n", hlsl);
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);

    hr = ID3D11Device_CreatePixelShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &ps);
    ID3D10Blob_Release(code);

    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: CreatePixelShader failed: 0x%08lX\n", hr);
        return NULL;
    }

    return ps;
}

ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *state)
{
    uint32_t hash = combiner_state_hash(state);
    uint32_t idx = hash & (COMBINER_CACHE_SIZE - 1);
    int probe;

    /* Linear probe lookup */
    for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
        uint32_t slot = (idx + probe) & (COMBINER_CACHE_SIZE - 1);
        CombinerCacheEntry *entry = &g_cache[slot];

        if (!entry->in_use) {
            /* Cache miss - compile and insert */
            ID3D11PixelShader *ps = compile_combiner_shader(state);
            if (!ps) return NULL;

            entry->in_use = TRUE;
            entry->hash = hash;
            memcpy(&entry->state, state, sizeof(NV2ACombinerState));
            entry->shader = ps;
            entry->last_used_frame = g_frame_counter;
            return ps;
        }

        if (entry->hash == hash && combiner_state_equal(&entry->state, state)) {
            /* Cache hit */
            entry->last_used_frame = g_frame_counter;
            return entry->shader;
        }
    }

    /*
     * Table is full - evict the least recently used entry.
     * This is rare in practice (most games use <20 configs).
     */
    {
        uint32_t lru_slot = idx;
        uint32_t lru_frame = UINT32_MAX;
        ID3D11PixelShader *ps;
        CombinerCacheEntry *entry;

        for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
            if (g_cache[probe].last_used_frame < lru_frame) {
                lru_frame = g_cache[probe].last_used_frame;
                lru_slot = probe;
            }
        }

        entry = &g_cache[lru_slot];
        if (entry->shader) {
            ID3D11PixelShader_Release(entry->shader);
        }

        ps = compile_combiner_shader(state);
        if (!ps) return NULL;

        entry->hash = hash;
        memcpy(&entry->state, state, sizeof(NV2ACombinerState));
        entry->shader = ps;
        entry->last_used_frame = g_frame_counter;
        return ps;
    }
}

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

HRESULT d3d8_combiners_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_cache, 0, sizeof(g_cache));
    memset(&g_combiner_state, 0, sizeof(g_combiner_state));
    g_ps_token = 0;
    g_dirty = TRUE;
    g_frame_counter = 0;

    /* Create the PS constant buffer for combiner shaders.
     * Size must match NV2APSConstants, rounded up to 16-byte alignment. */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = (sizeof(NV2APSConstants) + 15) & ~15;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL,
                                   &g_combiner_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: Failed to create constant buffer: "
                "0x%08lX\n", hr);
        return hr;
    }

    fprintf(stderr, "NV2A combiners: Initialized (cache size=%d)\n",
            COMBINER_CACHE_SIZE);
    return S_OK;
}

void d3d8_combiners_shutdown(void)
{
    int i;

    /* Release all cached shaders */
    for (i = 0; i < COMBINER_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && g_cache[i].shader) {
            ID3D11PixelShader_Release(g_cache[i].shader);
        }
    }
    memset(g_cache, 0, sizeof(g_cache));

    if (g_combiner_cb) {
        ID3D11Buffer_Release(g_combiner_cb);
        g_combiner_cb = NULL;
    }

    fprintf(stderr, "NV2A combiners: Shut down\n");
}

/* ================================================================
 * Draw Integration
 * ================================================================ */

void d3d8_combiners_set_pixel_shader(DWORD token)
{
    if (token != g_ps_token) {
        g_ps_token = token;
        g_dirty = TRUE;
    }
}

BOOL d3d8_combiners_active(void)
{
    return g_ps_token != 0;
}

void d3d8_combiners_mark_dirty(void)
{
    g_dirty = TRUE;
}

BOOL d3d8_combiners_prepare_draw(void)
{
    ID3D11DeviceContext *ctx;
    ID3D11PixelShader *ps;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const DWORD *rs;
    HRESULT hr;
    int i;

    /* Not using combiner shaders - fall back to fixed-function */
    if (g_ps_token == 0)
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx || !g_combiner_cb)
        return FALSE;

    rs = d3d8_GetRenderStates();

    /* Rebuild combiner state from token + render states if dirty */
    if (g_dirty) {
        d3d8_combiners_parse_token(g_ps_token, rs, &g_combiner_state);
        g_dirty = FALSE;
    }

    /* Get or compile the pixel shader for this combiner state */
    ps = d3d8_combiners_get_shader(&g_combiner_state);
    if (!ps) {
        fprintf(stderr, "NV2A combiners: Failed to get shader, "
                "falling back to FFP\n");
        return FALSE;
    }

    /* Bind the combiner pixel shader */
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

    /* Update the PS constant buffer with current values.
     *
     * Even though the shader structure doesn't change, the constant
     * values (C0, C1, fog, alpha ref) can change every frame via
     * render state writes. So we always re-upload. */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_combiner_cb,
                                0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        NV2APSConstants *cb = (NV2APSConstants *)mapped.pData;

        /* Per-stage constants */
        for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
            d3dcolor_to_float4(g_combiner_state.c0[i], cb->c0[i]);
            d3dcolor_to_float4(g_combiner_state.c1[i], cb->c1[i]);
        }

        /* Final combiner constants */
        d3dcolor_to_float4(g_combiner_state.final_c0, cb->final_c0);
        d3dcolor_to_float4(g_combiner_state.final_c1, cb->final_c1);

        /* Fog color from render state */
        d3dcolor_to_float4(rs[D3DRS_FOGCOLOR], cb->fog_color);

        /* Alpha test parameters */
        cb->alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
        cb->alpha_func = rs[D3DRS_ALPHAFUNC];
        cb->alpha_test_enable = rs[D3DRS_ALPHATESTENABLE] ? 1 : 0;
        cb->fog_enable = rs[D3DRS_FOGENABLE] ? 1 : 0;
        for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
            D3DFORMAT format = d3d8_base_format(d3d8_GetStageTexture(i));
            cb->alpha_only[i] = format == D3DFMT_A8 || format == D3DFMT_LIN_A8;
            cb->tex_scale[i][0] = cb->tex_scale[i][1] = 1.0f;
            cb->tex_scale[i][2] = cb->tex_scale[i][3] = 1.0f;
            cb->tex_mode[i] = (UINT)g_combiner_state.tex_mode[i];
        }

        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_combiner_cb, 0);
    }

    /* Bind the constant buffer to PS slot 0 */
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_combiner_cb);

    /* Advance frame counter for LRU tracking */
    g_frame_counter++;

    return TRUE;
}
