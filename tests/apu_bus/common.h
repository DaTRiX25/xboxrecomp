/* Shared by test_main.c and replay.c: the mix loop of apu_vp.c as it is
 * with the mixer off (voice_process, "Mix into bins"), and the DSP stage's
 * output for a set of mixbins, stereo or 5.1, through the real
 * mcpx_apu_dsp_frame. */
#include "apu_state.h"
#include "apu.h"
#include "apu_bus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

typedef struct {
    unsigned channels;
    int bin[8];
    float g[8];
    float samples[NUM_SAMPLES_PER_FRAME][2];
} tvoice;

static void old_mix(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], const tvoice *v)
{
    for (int b = 0; b < 8; b++) {
        float g = v->g[b];
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            mixbins[v->bin[b]][i] += g * v->samples[i][b % v->channels];
        }
    }
}

/* Burnout 3's 5.1 wiring (its main.c), so the 5.1 arm mixes what a title
 * really sets up: a speaker map and an effects level on one send. */
static const int8_t k_map[32] = {
    0, 1, 2, 3, 4, 5, 0, 1, 4, 5, 0, 1, 0, 1, 4, 5, 2, 0, 1,
    0, 1, 4, 5, 2, 3, 0, -1, 1, 0, 1, 0, 1,
};

static MCPXAPUState *s_d;

static void dsp_setup(void)
{
    s_d = (MCPXAPUState *)calloc(1, sizeof(*s_d));
    mcpx_apu_set_speaker_map(k_map);
    mcpx_apu_set_fx_bins(1u << 11);
}

/* One frame of mixbins to s16: out[32][6] (5.1) or out[32][2]. The DSP
 * stage may write into the bins, so it gets a copy. */
static void dsp_out(const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], int six, int16_t *out)
{
    static float copy[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memcpy(copy, mixbins, sizeof(copy));
    s_d->monitor.channels = six ? 6 : 2;
    s_d->ep_frame_div = 0;
    mcpx_apu_dsp_frame(s_d, copy);
    if (six) memcpy(out, s_d->monitor.frame_buf6, sizeof(int16_t) * 6 * NUM_SAMPLES_PER_FRAME);
    else     memcpy(out, s_d->monitor.frame_buf, sizeof(int16_t) * 2 * NUM_SAMPLES_PER_FRAME);
}

static const char *const k_names[13] = {
    "Music", "DJ", "Film", "Streams", "Player engine", "Player exhaust", "Other cars",
    "Traffic", "Tyres/road/wind", "Impacts/crash", "Boost", "UI/HUD", "Other",
};
