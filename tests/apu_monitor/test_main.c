/* mcpx_apu_monitor_frame must hand the device what the DSP stage produced.
 *
 * It used to clear frame_buf and render only the test tone and the software
 * mixer into it, so every sample the VP and DSP made was thrown away before
 * the device saw it, and it rendered a 1024-sample device buffer for every
 * 256-sample EP period. Here the device is a fake that records what it is
 * given, and the frame thread's sequence (dsp_frame then monitor_frame, eight
 * times per period) is driven by hand. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERIOD 256
#define SUB    NUM_SAMPLES_PER_FRAME      /* 32 */

extern volatile int g_audio_muted;

/* ---- fake device: every xa2_* symbol, so apu_xaudio2.o is not linked ---- */
static int16_t got[PERIOD * 8][2];
static int got_samples, got_calls;

int  xa2_init(void)          { return 1; }
void xa2_shutdown(void)      {}
int  xa2_is_active(void)     { return 1; }
int  xa2_get_buffer_size(void) { return PERIOD; }
int  xa2_submit_samples(const int16_t *s, int n)
{
    got_calls++;
    if (got_samples + n <= PERIOD * 8)
        memcpy(got[got_samples], s, (size_t)n * 2 * sizeof(int16_t));
    got_samples += n;
    return 1;
}

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

/* What dsp_frame writes for bin value v: (int16_t)(v * 32767). Bin 0 is
 * left and bin 1 right in both mixdown modes. */
static float left_of(int k, int i)  { return (float)(k * SUB + i) / 4096.0f; }
static float right_of(int k, int i) { return -(float)(k * SUB + i) / 8192.0f; }

/* One EP period the way the frame thread runs it. */
static void run_period(MCPXAPUState *d)
{
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    int calls0 = got_calls;
    for (int k = 0; k < 8; k++) {
        memset(mixbins, 0, sizeof(mixbins));
        for (int i = 0; i < SUB; i++) {
            mixbins[0][i] = left_of(k, i);
            mixbins[1][i] = right_of(k, i);
        }
        d->ep_frame_div = k;
        mcpx_apu_dsp_frame(d, mixbins);
        mcpx_apu_monitor_frame(d);
        if (k < 7)
            CHECK("nothing submitted mid-period", got_calls == calls0);
    }
}

static int16_t want_l(int s) { return (int16_t)(left_of(s / SUB, s % SUB) * 32767.0f); }
static int16_t want_r(int s) { return (int16_t)(right_of(s / SUB, s % SUB) * 32767.0f); }

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

int main(void)
{
    /* Zeroed: ram_ptr NULL keeps dsp_ack_frame out of guest memory, and
     * monitor.point 0 is MON_AC97, so dsp_frame writes frame_buf. */
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    int ok, i;
    if (!d) return 2;
    printf("apu_monitor: running\n");

    /* 1. The DSP's period reaches the device unchanged, once, 256 samples. */
    got_calls = got_samples = 0;
    run_period(d);
    CHECK("one submit per period", got_calls == 1);
    CHECK("exactly 256 samples", got_samples == PERIOD);
    for (ok = 1, i = 0; i < PERIOD; i++)
        if (got[i][0] != want_l(i) || got[i][1] != want_r(i)) ok = 0;
    CHECK("DSP frame reaches the device unchanged", ok);
    CHECK("signal is not silence", got[PERIOD - 1][0] != 0 && got[PERIOD - 1][1] != 0);
    for (ok = 1, i = 0; i < PERIOD; i++)
        if (d->monitor.frame_buf[i][0] || d->monitor.frame_buf[i][1]) ok = 0;
    CHECK("frame_buf starts the next period silent", ok);

    /* 2. With a software-mixer voice playing, the device gets the sum. */
    {
        static int16_t pcm[64][2];
        int slot = apu_mixer_alloc_voice();
        APUMixerVoice *v = apu_mixer_get_voice(slot);
        CHECK("mixer voice allocated", slot >= 0 && v);
        for (i = 0; i < 64; i++) { pcm[i][0] = 1000; pcm[i][1] = -2000; }
        v->pcm_data = &pcm[0][0];
        v->pcm_bytes = sizeof(pcm);
        v->num_channels = 2;
        v->sample_rate = 48000;
        v->volume = 1.0f;
        apu_mixer_play(slot, 1);

        got_calls = got_samples = 0;
        run_period(d);
        CHECK("one submit with the mixer on", got_calls == 1 && got_samples == PERIOD);
        for (ok = 1, i = 0; i < PERIOD; i++)
            if (got[i][0] != clamp16(want_l(i) + 1000)
                    || got[i][1] != clamp16(want_r(i) - 2000)) ok = 0;
        CHECK("mixer is added to the DSP frame", ok);

        /* 3. Muted: silence, still one period. */
        g_audio_muted = 1;
        got_calls = got_samples = 0;
        run_period(d);
        for (ok = 1, i = 0; i < PERIOD; i++)
            if (got[i][0] || got[i][1]) ok = 0;
        CHECK("muted period is silent", ok && got_samples == PERIOD);
        g_audio_muted = 0;
        apu_mixer_free_voice(slot);
    }

    /* 4. Ten periods: 2560 samples, no more, no fewer. */
    got_calls = got_samples = 0;
    for (i = 0; i < 10; i++) run_period(d);
    CHECK("ten periods, ten submits", got_calls == 10);
    CHECK("ten periods, 2560 samples", got_samples == 10 * PERIOD);

    free(d);
    if (failures == 0) { printf("apu_monitor: ALL PASS\n"); return 0; }
    printf("apu_monitor: %d FAILURE(S)\n", failures);
    return 1;
}
