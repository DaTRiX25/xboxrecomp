/*
 * d3d8_hlsl_split: does the parse -> HLSL pipeline still emit the same text?
 *
 * Feeds NPROG pseudo-random vertex programs (LCG microcode, a random
 * instruction marked final) through d3d8_vsh_parse and
 * d3d8_vsh_generate_hlsl_ex (both flag settings), and NCOMB pseudo-random
 * combiner register sets through d3d8_combiners_from_regs and
 * d3d8_combiners_generate_hlsl. Every output (text, or the -1 of an
 * overflow) goes into one FNV-1a hash, compared with EXPECTED_HASH, which
 * was recorded from the toolkit before the parsers moved to their own,
 * API-free files. With HLSL_DUMP=<path> the text is also written out, so
 * two builds can be diffed directly.
 *
 * Only API present on both sides of the split is used.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_vsh.h"
#include "d3d8_combiners.h"

#define NPROG 400
#define NCOMB 400
#define EXPECTED_HASH 0xfd6a6e89u
#define EXPECTED_CHARS 7285284u

static uint32_t s_seed = 0x2f6b1d3u;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed ^ (s_seed >> 15);
}

static uint32_t s_hash = 0x811c9dc5u;
static unsigned long s_chars;
static FILE *s_dump;

static void absorb(const char *what, int idx, const char *text, int n)
{
    char head[64];
    int i, hn = snprintf(head, sizeof head, "// %s %d: %d\n", what, idx, n);
    for (i = 0; i < hn; i++) { s_hash ^= (uint8_t)head[i]; s_hash *= 0x01000193u; }
    if (s_dump) fputs(head, s_dump);
    if (n <= 0)
        return;
    for (i = 0; i < n; i++) { s_hash ^= (uint8_t)text[i]; s_hash *= 0x01000193u; }
    s_chars += (unsigned long)n;
    if (s_dump) { fwrite(text, 1, (size_t)n, s_dump); fputc('\n', s_dump); }
}

int main(void)
{
    static uint32_t prog[NV2A_VS_MAX_INSTRUCTIONS * 4];
    static NV2AVshProgram parsed;
    static char buf[256 * 1024];
    const char *dump = getenv("HLSL_DUMP");
    int i, k, n;

    if (dump && !(s_dump = fopen(dump, "wb"))) {
        fprintf(stderr, "cannot write %s\n", dump);
        return 2;
    }

    for (i = 0; i < NPROG; i++) {
        int len = 1 + (int)(rnd() % 40), fin = (int)(rnd() % (uint32_t)len);
        for (k = 0; k < len * 4; k++)
            prog[k] = rnd();
        for (k = 0; k < len; k++)
            prog[k * 4 + 3] &= ~1u;
        prog[fin * 4 + 3] |= 1u;
        d3d8_vsh_parse((const void *)prog, len, &parsed);
        n = d3d8_vsh_generate_hlsl_ex(&parsed, 0, buf, (int)sizeof buf);
        absorb("vsh", i, buf, n);
        n = d3d8_vsh_generate_hlsl_ex(&parsed, NV2A_VSH_HLSL_SCREEN_SPACE,
                                      buf, (int)sizeof buf);
        absorb("vsh-ss", i, buf, n);
    }

    for (i = 0; i < NCOMB; i++) {
        uint32_t cicw[8], aicw[8], cocw[8], aocw[8];
        uint32_t control, fcw0, fcw1, modes;
        NV2ACombinerState st;
        for (k = 0; k < 8; k++) {
            cicw[k] = rnd(); aicw[k] = rnd(); cocw[k] = rnd(); aocw[k] = rnd();
        }
        control = (rnd() & ~0xFFu) | (1 + rnd() % 8);
        fcw0 = rnd(); fcw1 = rnd(); modes = rnd();
        d3d8_combiners_from_regs(cicw, aicw, cocw, aocw, control, fcw0, fcw1,
                                 modes, &st);
        st.fog_input = (int)(rnd() & 1);
        n = d3d8_combiners_generate_hlsl(&st, buf, (int)sizeof buf);
        absorb("rc", i, buf, n);
    }

    if (s_dump)
        fclose(s_dump);
    printf("hlsl_split: %d programs, %d combiner sets, %lu chars, hash %08x\n",
           NPROG, NCOMB, s_chars, s_hash);
    if (s_hash != EXPECTED_HASH || s_chars != EXPECTED_CHARS) {
        printf("FAIL: expected %lu chars, hash %08x\n",
               (unsigned long)EXPECTED_CHARS, EXPECTED_HASH);
        return 1;
    }
    printf("done\n");
    return 0;
}
