/* apu_bus_replay <recording> : a title's mix loop, recorded with
 * RECOMP_MIX_TRACE_REC while the mixer was off, summed again three ways:
 *
 *   A  the loop of apu_vp.c as written (common.h)
 *   B  the host mixer on with the neutral configuration (apu_bus.c)
 *
 * and compared: A's mixbins with the ones the game itself produced (the
 * recording's 'F' records: that A is the game's sum), B's mixbins with A's,
 * and the DSP stage's s16 output of A with the mixer off against B's with
 * the mixer on, stereo and 5.1. Bytes, not levels. */
#include "common.h"

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    static float A[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], B[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    static float R[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    struct { uint8_t tag, channels; uint16_t v; int32_t bin[8]; float g[8]; } h;
    static tvoice vv[512];
    unsigned long frames = 0, voices = 0, bad_game = 0, bad_bins = 0, bad_st = 0, bad_51 = 0;
    unsigned long sounding = 0;
    double peak = 0;
    int nv = 0, c;
    apu_bus_config cfg;

    if (!f) { fprintf(stderr, "usage: apu_bus_replay <recording>\n"); return 2; }
    dsp_setup();
    apu_bus_set_names(k_names, 13);
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);

    while ((c = fgetc(f)) != EOF) {
        if (c == 'V') {
            ungetc(c, f);
            if (fread(&h, sizeof(h), 1, f) != 1) break;
            if (nv >= 512) { fprintf(stderr, "too many voices in a frame\n"); return 2; }
            vv[nv].channels = h.channels;
            for (int b = 0; b < 8; b++) { vv[nv].bin[b] = h.bin[b]; vv[nv].g[b] = h.g[b]; }
            if (fread(vv[nv].samples, sizeof(float) * 2, NUM_SAMPLES_PER_FRAME, f) != NUM_SAMPLES_PER_FRAME) break;
            for (int b = 0; b < 8; b++) if (h.g[b] > 0) { sounding++; break; }
            nv++;
            voices++;
        } else if (c == 'F') {
            int16_t oa[NUM_SAMPLES_PER_FRAME * 6], ob[NUM_SAMPLES_PER_FRAME * 6];
            struct apu_bus_rt *rt;
            if (fread(R, sizeof(R), 1, f) != 1) break;
            memset(A, 0, sizeof(A));
            memset(B, 0, sizeof(B));
            for (int i = 0; i < nv; i++) old_mix(A, &vv[i]);
            if (memcmp(A, R, sizeof(A))) bad_game++;
            for (int six = 0; six < 2; six++) {
                apu_bus_enable(1);
                rt = apu_bus_frame_begin(six ? 6 : 2, k_map);
                memset(B, 0, sizeof(B));
                for (int i = 0; i < nv; i++)
                    apu_bus_mix_voice(rt, (int)((frames + (unsigned)i) % 13), B,
                                      (const float (*)[2])vv[i].samples, vv[i].channels,
                                      vv[i].bin, vv[i].g);
                apu_bus_frame_end(rt, B);
                if (memcmp(A, B, sizeof(A))) bad_bins++;
                dsp_out(B, six, ob);
                apu_bus_enable(0);
                dsp_out(A, six, oa);
                if (memcmp(oa, ob, sizeof(int16_t) * (six ? 6 : 2) * NUM_SAMPLES_PER_FRAME)) {
                    if (six) bad_51++; else bad_st++;
                }
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME * (six ? 6 : 2); i++)
                    if (abs(oa[i]) > peak) peak = abs(oa[i]);
            }
            frames++;
            nv = 0;
        } else {
            fprintf(stderr, "bad record tag %d\n", c);
            return 2;
        }
    }
    printf("%lu frames (%.1f s), %lu voice frames (%lu sounding), output peak %.0f of 32767\n",
           frames, frames / 1500.0, voices, sounding, peak);
    printf("frames where the loop's mixbins differ from the game's own:      %lu\n", bad_game);
    printf("frames where the mixer's mixbins (on, neutral) differ from it:   %lu\n", bad_bins);
    printf("frames where the s16 output differs, mixer off vs on, stereo:    %lu\n", bad_st);
    printf("frames where the s16 output differs, mixer off vs on, 5.1:       %lu\n", bad_51);
    return frames && !bad_game && !bad_bins && !bad_st && !bad_51 ? 0 : 1;
}
