/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "apu_speakers.h"
#include "recomp_env.h"
#include "fpconv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * RECOMP_APU_DSP_ACK=auto finds the doorbell from what the title programs.
 * The XDK DSOUND GP program takes its commands through the first page of
 * the GP's scratch space: a 2 KB command FIFO, then a header whose dword at
 * +0x810 is the doorbell the host writes and the DSP program clears (BLiNX 2
 * sub_003341BE spins on <block>+0x810; Burnout 3 sub_002F805E likewise). The
 * scratch pages are what NV_PAPU_GPSADDR's scatter-gather table maps -- 8
 * bytes an entry, the physical page in the first dword, as xemu's apu.c
 * scratch_rw walks it -- so the doorbell is SGE[0] + 0x810 wherever the
 * contiguous allocator put the block. A fixed address moves with allocation
 * order (reserving physical page 0 moved BLiNX 2's from 0x80A1C810 to
 * 0x80A20810); this does not. Wreckless fits too: its doorbell 0x014F8810 is
 * the first page of the scratch its GPSADDR table (0x01504000) maps, not
 * the table itself, which is where the earlier note here looked.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 */
#define APU_DSP_ACK_MAX 8
#define GP_DOORBELL_OFFSET 0x810u
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;
static int s_dsp_ack_auto;                /* =auto: derive from GPSADDR */
static uint32_t s_dsp_ack_auto_sge;       /* the GPSADDR it was derived from */
static uint32_t *s_dsp_ack_auto_slot;     /* host address of the doorbell */

static void dsp_ack_init(void)
{
    const char *spec = recomp_env(RENV_APU_DSP_ACK);
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    if (!strcmp(spec, "auto")) {
        s_dsp_ack_auto = 1;
        fprintf(stderr, "[APU] DSP doorbell ack: auto (GP scratch page 0"
                        " + 0x%X, from GPSADDR)\n", GP_DOORBELL_OFFSET);
        return;
    }
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_APU_MIXDOWN_ALL);
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* The doorbell for =auto: SGE[0] of the GP scratch table + 0x810, resolved
 * once GPSADDR is programmed and the table's first entry names a page, and
 * again whenever the title reprograms GPSADDR. NULL until then. */
static uint32_t *dsp_ack_auto_slot(MCPXAPUState *d)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;

    if (!sge)
        return NULL;
    if (sge == s_dsp_ack_auto_sge)
        return s_dsp_ack_auto_slot;
    page = *(const uint32_t *)mcpx_apu_phys(sge);
    if (!page || (page & 0xFFF))
        return NULL;                       /* table not filled in yet */
    s_dsp_ack_auto_sge = sge;
    s_dsp_ack_auto_slot = (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
    fprintf(stderr, "[APU] DSP doorbell: GP scratch SGE table 0x%08X maps page"
                    " 0x%08X (%s); doorbell at physical 0x%08X\n",
            sge, page, mcpx_apu_phys_class(page), page + GP_DOORBELL_OFFSET);
    return s_dsp_ack_auto_slot;
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    if (s_dsp_ack_auto) {
        uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot && *slot) {
            static int shown;
            if (shown++ < 3)
                fprintf(stderr, "[APU] DSP doorbell (auto): command 0x%08X"
                                " acknowledged\n", *slot);
            *slot = 0;
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

/* The bin -> speaker map of the 5.1 path (apu_speakers.c): DirectSound's
 * layout unless the title's wiring set its own. Read once a frame; set
 * before the APU starts. */
static apu_spk_map s_spk_map;
static int s_spk_map_set;

void mcpx_apu_set_speaker_map(const int8_t map[32])
{
    memcpy(s_spk_map, map, sizeof(s_spk_map));
    s_spk_map_set = 1;
}

void mcpx_apu_speaker_map_get(int8_t map[32])
{
    if (!s_spk_map_set) {
        apu_spk_default_map(s_spk_map);
        s_spk_map_set = 1;
    }
    memcpy(map, s_spk_map, sizeof(s_spk_map));
}

/* RECOMP_TRACE=audio_channels: what reaches each mixbin and each output
 * channel, once a second (1500 frames of 32 samples):
 *
 *   [AUDIO-CH] t=12s layout=5.1 bins 0=-31.2 1=-30.8 4=-40.1 ... (dBFS RMS)
 *   [AUDIO-CH] t=12s out FL=-29.0 FR=-28.7 FC=-90 LFE=-90 SL=-38.2 SR=-37.9
 *
 * Bins that stayed silent are left out. Off: one branch a frame. */
static int s_ch_trace = -1;
static double s_ch_bin_sq[NUM_MIXBINS];
static double s_ch_out_sq[APU_SPK_CHANNELS];
static unsigned s_ch_frames, s_ch_secs;

static double ch_db(double sq, double n)
{
    return sq > 0 ? 10.0 * log10(sq / n) : -90.0;
}

static void ch_trace(const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                     const int16_t *out, int nch)
{
    char line[1024];
    int len = 0;
    double n;

    for (int b = 0; b < NUM_MIXBINS; b++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            s_ch_bin_sq[b] += (double)mixbins[b][i] * mixbins[b][i];
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        for (int c = 0; c < nch; c++) {
            double x = out[i * nch + c] / 32768.0;
            s_ch_out_sq[c] += x * x;
        }
    if (++s_ch_frames < 1500)
        return;

    n = (double)s_ch_frames * NUM_SAMPLES_PER_FRAME;
    s_ch_secs++;
    for (int b = 0; b < NUM_MIXBINS; b++)
        if (s_ch_bin_sq[b] > 0 && len < (int)sizeof(line) - 16)
            len += snprintf(line + len, sizeof(line) - len, " %d=%.1f", b,
                            ch_db(s_ch_bin_sq[b], n));
    fprintf(stderr, "[AUDIO-CH] t=%us layout=%s bins%s\n", s_ch_secs,
            nch == APU_SPK_CHANNELS ? "5.1" : "stereo", len ? line : " (silent)");
    if (nch == APU_SPK_CHANNELS)
        fprintf(stderr, "[AUDIO-CH] t=%us out FL=%.1f FR=%.1f FC=%.1f LFE=%.1f"
                        " SL=%.1f SR=%.1f\n", s_ch_secs,
                ch_db(s_ch_out_sq[0], n), ch_db(s_ch_out_sq[1], n),
                ch_db(s_ch_out_sq[2], n), ch_db(s_ch_out_sq[3], n),
                ch_db(s_ch_out_sq[4], n), ch_db(s_ch_out_sq[5], n));
    else
        fprintf(stderr, "[AUDIO-CH] t=%us out L=%.1f R=%.1f\n", s_ch_secs,
                ch_db(s_ch_out_sq[0], n), ch_db(s_ch_out_sq[1], n));
    memset(s_ch_bin_sq, 0, sizeof(s_ch_bin_sq));
    memset(s_ch_out_sq, 0, sizeof(s_ch_out_sq));
    s_ch_frames = 0;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    if (s_ch_trace < 0)
        s_ch_trace = recomp_env_on(RENV_AUDIO_CHANNELS);

    /* 5.1: the six speaker feeds (apu_speakers.c), and the stereo fold
     * below is not run at all, so the stereo path is the same code it
     * always was. */
    if (d->monitor.channels == APU_SPK_CHANNELS &&
        d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        if (!s_spk_map_set) {
            apu_spk_default_map(s_spk_map);
            s_spk_map_set = 1;
        }
        apu_spk_mix51(&mixbins[0][0], NUM_SAMPLES_PER_FRAME, s_spk_map,
                      &d->monitor.frame_buf6[off], NUM_SAMPLES_PER_FRAME);
        if (s_ch_trace)
            ch_trace((const float (*)[NUM_SAMPLES_PER_FRAME])mixbins,
                     &d->monitor.frame_buf6[off][0], APU_SPK_CHANNELS);
        g_dbg.gp.cycles = 0;
        g_dbg.ep.cycles = 0;
        return;
    }

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
        if (s_ch_trace)
            ch_trace((const float (*)[NUM_SAMPLES_PER_FRAME])mixbins,
                     &d->monitor.frame_buf[off][0], 2);
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
