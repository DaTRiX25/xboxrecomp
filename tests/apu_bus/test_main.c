/* The host mixer (src/apu/apu_bus.c, BO-114).
 *
 * 1. Neutral is today: on a deterministic set of voices the mixer on with
 *    the neutral configuration gives the mixbins of apu_vp.c's own loop and
 *    the s16 output of the DSP stage with the mixer off, byte for byte,
 *    stereo and 5.1.
 * 2. Each control does what it says, in dB, ms and Hz.
 * 3. The preset file parses, writes and reads back.
 *
 * Run with: ctest --test-dir <build> */
#include "common.h"

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint32_t s_seed = 12345;
static float frand(void)                /* -1..1 */
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(int32_t)s_seed / 2147483648.0f;
}

/* The bin sets the title really uses (the BO-114 study). */
static const int k_bins[5][8] = {
    { 0, 1, 4, 5, 2, 3, 6, 7 },             /* a stream on the speaker bins */
    { 0, 1, 4, 5, 2, 17, 3, 6 },            /* 2D: the player's engine */
    { 19, 20, 16, 24, 14, 15, 11, 0 },      /* positioned, + polarity */
    { 25, 13, 23, 24, 21, 22, 11, 0 },      /* positioned, - polarity */
    { 0, 1, 2, 3, 4, 5, 6, 7 },
};

static void make_voice(tvoice *v, int k)
{
    v->channels = (k % 3 == 0) ? 2 : 1;
    memcpy(v->bin, k_bins[k % 5], sizeof(v->bin));
    for (int b = 0; b < 8; b++) {
        float a = frand();
        v->g[b] = (k % 7 == 6 || a < -0.5f) ? 0.0f : 0.25f * (a + 1.0f);
    }
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        v->samples[i][0] = frand();
        v->samples[i][1] = v->channels == 2 ? frand() : v->samples[i][0];
    }
}

static double bin_rms(const float b[NUM_SAMPLES_PER_FRAME])
{
    double s = 0;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) s += (double)b[i] * b[i];
    return sqrt(s / NUM_SAMPLES_PER_FRAME);
}

static double db(double x) { return x > 1e-12 ? 20.0 * log10(x) : -240.0; }

/* A voice through the mixer with cfg, one frame, into bins. */
static void run_one(const apu_bus_config *cfg, int channels, int bus, const tvoice *v,
                    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    struct apu_bus_rt *rt;
    apu_bus_set_config(cfg);
    apu_bus_enable(1);
    rt = apu_bus_frame_begin(channels, k_map);
    apu_bus_mix_voice(rt, bus, bins, (const float (*)[2])v->samples, v->channels, v->bin, v->g);
    apu_bus_frame_end(rt, bins);
}

static void test_identity(void)
{
    static float A[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], B[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    static tvoice vv[24];
    apu_bus_config cfg;
    unsigned long bad_bins = 0, bad_st = 0, bad_51 = 0, clipped = 0;

    apu_bus_config_neutral(&cfg);
    CHECK(apu_bus_config_is_neutral(&cfg), "the neutral configuration is not neutral");
    apu_bus_set_config(&cfg);
    for (int frame = 0; frame < 600; frame++) {
        int16_t oa[NUM_SAMPLES_PER_FRAME * 6], ob[NUM_SAMPLES_PER_FRAME * 6];
        for (int k = 0; k < 24; k++) make_voice(&vv[k], k + frame);
        memset(A, 0, sizeof(A));
        for (int k = 0; k < 24; k++) old_mix(A, &vv[k]);
        for (int six = 0; six < 2; six++) {
            struct apu_bus_rt *rt;
            apu_bus_enable(1);
            rt = apu_bus_frame_begin(six ? 6 : 2, k_map);
            memset(B, 0, sizeof(B));
            for (int k = 0; k < 24; k++)
                apu_bus_mix_voice(rt, k % 13, B, (const float (*)[2])vv[k].samples,
                                  vv[k].channels, vv[k].bin, vv[k].g);
            apu_bus_frame_end(rt, B);
            if (memcmp(A, B, sizeof(A))) bad_bins++;
            dsp_out(B, six, ob);
            apu_bus_enable(0);
            dsp_out(A, six, oa);
            if (memcmp(oa, ob, sizeof(int16_t) * (six ? 6 : 2) * NUM_SAMPLES_PER_FRAME)) {
                if (six) bad_51++; else bad_st++;
            }
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME * (six ? 6 : 2); i++)
                clipped += oa[i] == 32767 || oa[i] == -32767;
        }
    }
    {
        /* The control: 0.1 dB on every bus must show, or the comparison
         * above compares nothing. */
        struct apu_bus_rt *rt;
        for (int k = 0; k < 13; k++) cfg.bus[k].gain_db = -0.1f;
        apu_bus_set_config(&cfg);
        apu_bus_enable(1);
        rt = apu_bus_frame_begin(6, k_map);
        memset(B, 0, sizeof(B));
        for (int k = 0; k < 24; k++)
            apu_bus_mix_voice(rt, k % 13, B, (const float (*)[2])vv[k].samples,
                              vv[k].channels, vv[k].bin, vv[k].g);
        apu_bus_frame_end(rt, B);
        CHECK(memcmp(A, B, sizeof(A)) != 0, "a 0.1 dB gain leaves the mixbins identical: the comparison is blind");
        apu_bus_config_neutral(&cfg);
        apu_bus_set_config(&cfg);
        apu_bus_enable(0);
    }
    CHECK(!bad_bins, "neutral mixer: mixbins differ from the loop's in %lu frames", bad_bins);
    CHECK(!bad_st, "neutral mixer: stereo s16 output differs in %lu frames", bad_st);
    CHECK(!bad_51, "neutral mixer: 5.1 s16 output differs in %lu frames", bad_51);
    /* The set must drive the clamp too, or the clamp is not covered. */
    CHECK(clipped > 100, "the test voices never reach full scale (%lu)", clipped);
    printf("identity: 600 frames x 24 voices, stereo and 5.1: %lu / %lu / %lu frames differ"
           " (mixbins / stereo / 5.1), %lu clamped samples covered\n",
           bad_bins, bad_st, bad_51, clipped);
}

static void test_controls(void)
{
    static float ref[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], out[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    apu_bus_config cfg;
    tvoice v;
    double r;

    memset(&v, 0, sizeof(v));
    v.channels = 1;
    memcpy(v.bin, k_bins[2], sizeof(v.bin));        /* FL FR C LFE SL SR + reverb */
    for (int b = 0; b < 6; b++) v.g[b] = 0.5f;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        v.samples[i][0] = v.samples[i][1] = sinf((float)i * 0.7f);
    memset(ref, 0, sizeof(ref));
    old_mix(ref, &v);

    /* Gain: -6 dB on the voice's bus, nothing on another. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[9].gain_db = -6.0f;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 9, &v, out);
    r = db(bin_rms(out[19]) / bin_rms(ref[19]));
    CHECK(fabs(r + 6.0) < 0.01, "gain -6 dB gave %.3f dB", r);
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 3, &v, out);
    CHECK(!memcmp(out, ref, sizeof(ref)), "a gain on one bus changed another");

    /* Mute, and solo of another bus. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[9].mute = 1;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 9, &v, out);
    CHECK(bin_rms(out[19]) == 0.0, "mute is not silent");
    apu_bus_config_neutral(&cfg);
    cfg.bus[2].solo = 1;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 9, &v, out);
    CHECK(bin_rms(out[19]) == 0.0, "a bus sounds while another is soloed");
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 2, &v, out);
    CHECK(!memcmp(out, ref, sizeof(ref)), "the soloed bus changed");

    /* Trims, route title: rear -3 dB, centre off, front and LFE as they were. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[9].trim_db[APU_BUS_TRIM_REAR] = -3.0f;
    cfg.bus[9].trim_db[APU_BUS_TRIM_CENTRE] = APU_BUS_OFF_DB;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 9, &v, out);
    CHECK(!memcmp(out[19], ref[19], sizeof(ref[0])) && !memcmp(out[24], ref[24], sizeof(ref[0])),
          "a rear trim changed the front or the LFE");
    r = db(bin_rms(out[14]) / bin_rms(ref[14]));
    CHECK(fabs(r + 3.0) < 0.01, "rear trim -3 dB gave %.3f dB", r);
    CHECK(bin_rms(out[16]) == 0.0, "centre trim off is not silent");

    /* Sends: the voice's level (its loudest send, 0.5) on the speakers
     * asked for and nowhere else. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[4].route = APU_BUS_ROUTE_SENDS;
    cfg.bus[4].send_db[0] = -6.0f;                  /* FL */
    cfg.bus[4].send_db[1] = APU_BUS_OFF_DB;
    cfg.bus[4].send_db[2] = 0.0f;                   /* C */
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 4, &v, out);
    CHECK(fabs(db(bin_rms(out[2]) / bin_rms(ref[19]))) < 0.01, "send C 0 dB is not the voice's level");
    CHECK(fabs(db(bin_rms(out[0]) / bin_rms(ref[19])) + 6.0) < 0.01, "send FL -6 dB is off");
    for (int b = 0; b < NUM_MIXBINS; b++)
        if (b != 0 && b != 2)
            CHECK(bin_rms(out[b]) == 0.0, "route sends left signal in bin %d", b);
    /* The same sends on a stereo output: C at -3 dB into both sides. */
    memset(out, 0, sizeof(out));
    run_one(&cfg, 2, 4, &v, out);
    CHECK(fabs(db(bin_rms(out[1]) / bin_rms(ref[19])) + 3.0) < 0.02, "stereo fold of the centre send is off");
    CHECK(bin_rms(out[0]) > bin_rms(out[1]), "stereo fold lost the FL send");

    /* LFE send with an 80 Hz crossover: 40 Hz passes, 1 kHz does not. */
    for (int tone = 0; tone < 2; tone++) {
        const double hz = tone ? 1000.0 : 40.0;
        double in_sq = 0, out_sq = 0;
        apu_bus_config_neutral(&cfg);
        cfg.bus[9].lfe_send_db = 0.0f;
        cfg.bus[9].lfe_crossover_hz = 80;
        apu_bus_set_config(&cfg);
        apu_bus_enable(1);
        for (int frame = 0; frame < 1500; frame++) {
            struct apu_bus_rt *rt = apu_bus_frame_begin(6, k_map);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                v.samples[i][0] = v.samples[i][1] =
                    (float)sin(2.0 * M_PI * hz * (frame * 32 + i) / 48000.0);
            memset(out, 0, sizeof(out));
            v.g[3] = 0.0f;                          /* the title's own LFE send shut */
            apu_bus_mix_voice(rt, 9, out, (const float (*)[2])v.samples, 1, v.bin, v.g);
            apu_bus_frame_end(rt, out);
            if (frame >= 750) {
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                    in_sq += 0.25 * (double)v.samples[i][0] * v.samples[i][0];
                    out_sq += (double)out[3][i] * out[3][i];
                }
            }
        }
        r = 10.0 * log10(out_sq / in_sq + 1e-30);
        if (tone) CHECK(r < -80.0, "80 Hz crossover leaves 1 kHz at %.1f dB", r);
        else      CHECK(fabs(r) < 0.5, "80 Hz crossover has 40 Hz at %.2f dB", r);
        /* Let the filter and the send run out before the next tone. */
        for (int frame = 0; frame < 3000; frame++) {
            struct apu_bus_rt *rt = apu_bus_frame_begin(6, k_map);
            memset(out, 0, sizeof(out));
            apu_bus_frame_end(rt, out);
        }
    }
    v.g[3] = 0.5f;
}

static void test_output(void)
{
    static float feeds[6][APU_BUS_FRAME];
    apu_bus_config cfg;
    float mx;

    /* Neutral: the stage is not active and does not touch the feeds. */
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
    apu_bus_enable(1);
    CHECK(!apu_bus_output_active(), "the neutral output stage says it is active");

    /* Speaker trim -6 dB on FR, 1 ms delay on SL. */
    cfg.speaker_trim_db[1] = -6.0f;
    cfg.speaker_delay_ms[4] = 1.0f;
    apu_bus_set_config(&cfg);
    CHECK(apu_bus_output_active(), "trim and delay do not activate the stage");
    {
        int at = -1;
        for (int frame = 0; frame < 4 && at < 0; frame++) {
            memset(feeds, 0, sizeof(feeds));
            if (frame == 0) { feeds[1][0] = 1.0f; feeds[4][0] = 1.0f; }
            apu_bus_output(feeds, 6, APU_BUS_FRAME);
            if (frame == 0)
                CHECK(fabsf(feeds[1][0] - 0.5012f) < 0.001f, "speaker trim -6 dB gave %f", feeds[1][0]);
            for (int i = 0; i < APU_BUS_FRAME; i++)
                if (feeds[4][i] != 0.0f) at = frame * APU_BUS_FRAME + i;
        }
        CHECK(at == 48, "1 ms of delay is %d samples, not 48", at);
    }

    /* Soft clip and the peak limiter: a 6 dB over-full-scale sine stays
     * under the ceiling; a quiet one passes at its level. */
    for (int mode = APU_BUS_LIM_SOFT; mode <= APU_BUS_LIM_PEAK; mode++) {
        double sq_in = 0, sq_out = 0;
        apu_bus_config_neutral(&cfg);
        cfg.limiter = mode;
        cfg.ceiling_db = -1.0f;
        apu_bus_set_config(&cfg);
        mx = 0.0f;
        for (int frame = 0; frame < 600; frame++) {
            for (int i = 0; i < APU_BUS_FRAME; i++)
                feeds[0][i] = feeds[1][i] =
                    2.0f * (float)sin(2.0 * M_PI * 440.0 * (frame * 32 + i) / 48000.0);
            apu_bus_output(feeds, 2, APU_BUS_FRAME);
            for (int i = 0; i < APU_BUS_FRAME; i++)
                if (fabsf(feeds[0][i]) > mx) mx = fabsf(feeds[0][i]);
        }
        CHECK(mx <= 0.8913f + 1e-4f, "limiter mode %d let %.4f through a -1 dB ceiling", mode, mx);
        CHECK(mx > 0.6f, "limiter mode %d output is too low (%.4f)", mode, mx);
        for (int frame = 0; frame < 3000; frame++) {    /* release, then measure */
            for (int i = 0; i < APU_BUS_FRAME; i++)
                feeds[0][i] = feeds[1][i] =
                    0.1f * (float)sin(2.0 * M_PI * 440.0 * (frame * 32 + i) / 48000.0);
            if (frame >= 2000)
                for (int i = 0; i < APU_BUS_FRAME; i++) sq_in += (double)feeds[0][i] * feeds[0][i];
            apu_bus_output(feeds, 2, APU_BUS_FRAME);
            if (frame >= 2000)
                for (int i = 0; i < APU_BUS_FRAME; i++) sq_out += (double)feeds[0][i] * feeds[0][i];
        }
        CHECK(fabs(10.0 * log10(sq_out / sq_in)) < 0.05,
              "limiter mode %d changes a -20 dBFS signal by %.3f dB", mode, 10.0 * log10(sq_out / sq_in));
    }

    /* LFE low-pass 120 Hz: 1 kHz on the LFE feed is gone, FL untouched. */
    {
        double sq = 0, sq_fl = 0;
        apu_bus_config_neutral(&cfg);
        cfg.lfe_lowpass_hz = 120;
        apu_bus_set_config(&cfg);
        for (int frame = 0; frame < 1500; frame++) {
            for (int i = 0; i < APU_BUS_FRAME; i++)
                feeds[0][i] = feeds[3][i] = (float)sin(2.0 * M_PI * 1000.0 * (frame * 32 + i) / 48000.0);
            apu_bus_output(feeds, 6, APU_BUS_FRAME);
            if (frame >= 750)
                for (int i = 0; i < APU_BUS_FRAME; i++) {
                    sq += (double)feeds[3][i] * feeds[3][i];
                    sq_fl += (double)feeds[0][i] * feeds[0][i];
                }
        }
        CHECK(10.0 * log10(sq / sq_fl) < -70.0, "LFE low-pass leaves 1 kHz at %.1f dB", 10.0 * log10(sq / sq_fl));
        CHECK(fabs(10.0 * log10(sq_fl / (750.0 * 32 * 0.5))) < 0.01, "LFE low-pass touched FL");
    }
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
}

static void test_duck(void)
{
    static float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    apu_bus_config cfg;
    apu_bus_meters m;
    tvoice src, tgt;
    double level_at[4] = { 0, 0, 0, 0 };

    memset(&src, 0, sizeof(src));
    src.channels = 1;
    memcpy(src.bin, k_bins[2], sizeof(src.bin));
    src.g[0] = 0.5f;
    tgt = src;
    memcpy(tgt.bin, k_bins[0], sizeof(tgt.bin));
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        src.samples[i][0] = src.samples[i][1] = (i & 1) ? 0.5f : -0.5f;
        tgt.samples[i][0] = tgt.samples[i][1] = (i & 2) ? 0.5f : -0.5f;
    }
    apu_bus_config_neutral(&cfg);
    cfg.duck.enabled = 1;
    cfg.duck.source = 9;
    cfg.duck.target = 0;
    cfg.duck.threshold_db = -30.0f;
    cfg.duck.depth_db = 6.0f;
    cfg.duck.attack_ms = 20.0f;
    cfg.duck.release_ms = 200.0f;
    apu_bus_set_config(&cfg);
    apu_bus_enable(1);
    /* 0.2 s of both (the source at -18 dBFS: over the threshold), then
     * 2 s of the target alone. */
    for (int frame = 0; frame < 3300; frame++) {
        struct apu_bus_rt *rt = apu_bus_frame_begin(6, k_map);
        memset(bins, 0, sizeof(bins));
        apu_bus_mix_voice(rt, 0, bins, (const float (*)[2])tgt.samples, 1, tgt.bin, tgt.g);
        if (frame < 300)
            apu_bus_mix_voice(rt, 9, bins, (const float (*)[2])src.samples, 1, src.bin, src.g);
        apu_bus_frame_end(rt, bins);
        if (frame == 0)    level_at[0] = bin_rms(bins[0]);
        if (frame == 299)  level_at[1] = bin_rms(bins[0]);
        if (frame == 330)  level_at[2] = bin_rms(bins[0]);     /* 20 ms after: still down */
        if (frame == 3299) level_at[3] = bin_rms(bins[0]);
    }
    CHECK(fabs(db(level_at[1] / level_at[0]) + 6.0) < 0.1,
          "ducking: target at %.2f dB after 200 ms of the source, not -6", db(level_at[1] / level_at[0]));
    CHECK(db(level_at[2] / level_at[0]) < -4.5, "ducking released at once (%.2f dB)", db(level_at[2] / level_at[0]));
    CHECK(fabs(db(level_at[3] / level_at[0])) < 0.05, "ducking did not release (%.2f dB)", db(level_at[3] / level_at[0]));
    apu_bus_get_meters(&m);
    CHECK(m.duck_db < 0.1f, "the duck meter is stuck at %.2f dB", m.duck_db);
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
}

static void test_preset(void)
{
    static char text[32768], text2[32768];
    apu_bus_config a, b;
    char err[160];

    apu_bus_config_neutral(&a);
    a.bus[0].route = APU_BUS_ROUTE_SENDS;
    a.bus[0].send_db[4] = APU_BUS_OFF_DB;
    a.bus[0].send_db[5] = -12.5f;
    a.bus[8].gain_db = -3.5f;
    a.bus[8].trim_db[APU_BUS_TRIM_REAR] = 2.0f;
    a.bus[9].lfe_send_db = -6.0f;
    a.bus[9].lfe_crossover_hz = 100;
    a.bus[5].mute = 1;
    a.bus[4].route = APU_BUS_ROUTE_PAN;
    a.bus[4].pan_x = -0.25f; a.bus[4].pan_y = 0.5f; a.bus[4].pan_width = 0.75f; a.bus[4].pan_divergence = 0.5f;
    a.bus[7].field_width = 1.5f; a.bus[7].field_divergence = -0.25f;
    a.speaker_trim_db[2] = -1.5f;
    a.speaker_delay_ms[5] = 3.5f;
    a.limiter = APU_BUS_LIM_PEAK;
    a.ceiling_db = -1.0f;
    a.lfe_lowpass_hz = 120;
    a.duck.enabled = 1;
    a.duck.source = 9;
    a.duck.target = 0;
    a.duck.depth_db = 4.0f;
    apu_bus_config_write(&a, text, sizeof(text));
    apu_bus_config_neutral(&b);
    CHECK(apu_bus_config_parse(text, &b, err, sizeof(err)) == 0 && !*err, "written preset does not parse: %s", err);
    apu_bus_config_write(&b, text2, sizeof(text2));
    CHECK(!strcmp(text, text2), "a preset does not read back as written");
    CHECK(b.bus[0].send_db[4] <= APU_BUS_OFF_DB && b.bus[0].send_db[5] == -12.5f, "sends did not read back");
    CHECK(b.duck.source == 9 && b.duck.target == 0 && b.duck.enabled, "the duck slot did not read back");
    CHECK(!apu_bus_config_is_neutral(&b), "a changed preset reads as neutral");
    CHECK(b.bus[4].route == APU_BUS_ROUTE_PAN && b.bus[4].pan_x == -0.25f && b.bus[4].pan_width == 0.75f
          && b.bus[7].field_width == 1.5f && b.bus[7].field_divergence == -0.25f, "pan and field keys did not read back");

    /* By hand: comments, an unknown key (reported, not fatal), a bad line. */
    apu_bus_config_neutral(&b);
    CHECK(apu_bus_config_parse("# mine\n[bus.tyres_road_wind]\ngain_db = -2   # quieter\nwobble = 3\n"
                               "[speaker.LFE]\ntrim_db = 4\n", &b, err, sizeof(err)) == 0,
          "a hand-written preset does not parse");
    CHECK(b.bus[8].gain_db == -2.0f && b.speaker_trim_db[3] == 4.0f, "hand-written values not taken");
    CHECK(strstr(err, "wobble") != NULL, "an unknown key is not reported (%s)", err);
    apu_bus_config_neutral(&a);
    a.bus[1].gain_db = 1.0f;
    b = a;
    CHECK(apu_bus_config_parse("[master]\nlimiter soft\n", &b, err, sizeof(err)) < 0, "a line without = parses");
    CHECK(!memcmp(&a, &b, sizeof(a)), "a failed parse changed the configuration");
    CHECK(apu_bus_find("Tyres/road/wind") == 8 && apu_bus_find("impacts_crash") == 9 &&
          apu_bus_find("nothing") < 0, "bus keys");
}

/* The pan law, route "pan" and the field shaping. */
static void test_pan(void)
{
    static float out[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    apu_bus_config cfg;
    tvoice v;
    float g[5], g2[2];
    double worst = 0.0;

    /* The law: the squares sum to 1 everywhere, for any divergence. */
    for (int ix = -10; ix <= 10; ix++)
        for (int iy = -10; iy <= 10; iy++)
            for (int id = 0; id <= 4; id++) {
                double p = 0;
                apu_bus_pan_gains(ix / 10.0f, iy / 10.0f, id / 4.0f, g);
                for (int k = 0; k < 5; k++) p += (double)g[k] * g[k];
                if (fabs(db(sqrt(p))) > worst) worst = fabs(db(sqrt(p)));
            }
    CHECK(worst < 0.001, "pan law: the power is off by %.4f dB somewhere on the grid", worst);
    /* The places that have a name. Order: FL FR C SL SR. */
    apu_bus_pan_gains(0, 1, 0, g);
    CHECK(fabsf(g[2] - 1) < 1e-5f && g[0] < 1e-5f && g[1] < 1e-5f && g[3] < 1e-5f, "front centre is not the centre speaker");
    apu_bus_pan_gains(-1, 1, 0, g);
    CHECK(fabsf(g[0] - 1) < 1e-5f && g[2] < 1e-4f && g[3] < 1e-4f, "front left corner is not FL");
    apu_bus_pan_gains(1, -1, 0, g);
    CHECK(fabsf(g[4] - 1) < 1e-5f && g[1] < 1e-4f && g[3] < 1e-4f, "rear right corner is not SR");
    apu_bus_pan_gains(0, -1, 0, g);
    CHECK(fabsf(g[3] - 0.70711f) < 1e-4f && fabsf(g[4] - 0.70711f) < 1e-4f && g[2] < 1e-5f, "rear centre is not SL + SR at -3 dB");
    apu_bus_pan_gains(1, 0, 0, g);
    CHECK(fabsf(g[1] - 0.70711f) < 1e-4f && fabsf(g[4] - 0.70711f) < 1e-4f, "the right wall's middle is not FR + SR at -3 dB");
    apu_bus_pan_gains(0, 0, 0, g);
    for (int k = 0; k < 5; k++) CHECK(fabsf(g[k] - 0.44721f) < 1e-4f, "the middle of the room is not all five at -7 dB");
    apu_bus_pan_gains(0, 1, 1, g);
    CHECK(g[2] < 1e-5f && fabsf(g[0] - 0.70711f) < 1e-4f && fabsf(g[1] - 0.70711f) < 1e-4f, "divergence 1 is not a phantom centre");
    apu_bus_pan_gains(0, 1, 0.5f, g);
    CHECK(fabsf(g[2] - 0.70711f) < 1e-4f && fabsf(g[0] - 0.5f) < 1e-4f, "divergence 0.5 does not halve the centre's power");
    apu_bus_pan_gains(0.5f, 1, 0, g);
    CHECK(g[2] > 0.3f && g[1] > 0.3f && g[0] < 1e-5f && g[4] < 1e-5f, "between C and FR other speakers play");
    for (int ix = -10; ix <= 10; ix++) {
        apu_bus_pan_gains_stereo(ix / 10.0f, g2);
        CHECK(fabs(db(sqrt((double)g2[0] * g2[0] + (double)g2[1] * g2[1]))) < 0.001, "stereo pan law power at %d", ix);
    }
    apu_bus_pan_gains_stereo(-1, g2);
    CHECK(fabsf(g2[0] - 1) < 1e-6f && fabsf(g2[1]) < 1e-6f, "stereo pan: hard left");

    /* Through the mixer. A stereo voice whose channels are orthogonal (a
     * sine and a cosine, whole periods in a frame), so powers add; its
     * loudest send is 0.5. */
    memset(&v, 0, sizeof(v));
    v.channels = 2;
    memcpy(v.bin, k_bins[0], sizeof(v.bin));
    v.g[0] = 0.5f; v.g[1] = 0.5f; v.g[2] = 0.3f; v.g[3] = 0.3f; v.g[5] = 0.1f;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        v.samples[i][0] = (float)sin(2.0 * M_PI * 2.0 * i / 32.0);
        v.samples[i][1] = (float)cos(2.0 * M_PI * 2.0 * i / 32.0);
    }
    for (int mono = 0; mono < 2; mono++) {
        double want = 0.5 * sqrt(mono ? 0.5 : 1.0), worst_p = 0;      /* rms of both channels, or of one */
        tvoice t = v;
        if (mono) {
            t.channels = 1;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) t.samples[i][1] = t.samples[i][0];
        }
        for (int six = 0; six < 2; six++)
            for (int ix = -4; ix <= 4; ix++)
                for (int iy = -4; iy <= 4; iy++)
                    for (int iw = 0; iw <= 4; iw++) {
                        double p = 0, d;
                        apu_bus_config_neutral(&cfg);
                        cfg.bus[0].route = APU_BUS_ROUTE_PAN;
                        cfg.bus[0].pan_x = ix / 4.0f;
                        cfg.bus[0].pan_y = iy / 4.0f;
                        cfg.bus[0].pan_width = iw / 4.0f;
                        cfg.bus[0].pan_divergence = (float)((ix + iy + 8) % 3) / 2.0f;
                        memset(out, 0, sizeof(out));
                        run_one(&cfg, six ? 6 : 2, 0, &t, out);
                        for (int b = 0; b < NUM_MIXBINS; b++) p += bin_rms(out[b]) * bin_rms(out[b]);
                        d = fabs(db(sqrt(p) / want));
                        if (d > worst_p) worst_p = d;
                        if (six) CHECK(bin_rms(out[3]) == 0.0, "route pan feeds the LFE");
                    }
        CHECK(worst_p < 0.1, "route pan (%s source): total power off by %.3f dB somewhere", mono ? "mono" : "stereo", worst_p);
    }
    /* Front centre, full width: the left channel is FL and nothing else,
     * the right FR: the stereo picture as it is. Width 0: both in C. */
    apu_bus_config_neutral(&cfg);
    cfg.bus[0].route = APU_BUS_ROUTE_PAN;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 0, &v, out);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        CHECK(fabsf(out[0][i] - 0.5f * v.samples[i][0]) < 1e-6f, "pan, width 1: FL is not the left channel");
        CHECK(fabsf(out[1][i] - 0.5f * v.samples[i][1]) < 1e-6f, "pan, width 1: FR is not the right channel");
    }
    CHECK(bin_rms(out[2]) < 1e-6 && bin_rms(out[4]) < 1e-6, "pan, width 1 at the front: centre or rears play");
    cfg.bus[0].pan_width = 0.0f;
    memset(out, 0, sizeof(out));
    run_one(&cfg, 6, 0, &v, out);
    CHECK(bin_rms(out[0]) < 1e-6 && fabs(db(bin_rms(out[2]) / 0.5)) < 0.01, "pan, width 0: not the two channels in the centre");
    /* Stereo output, x 0, width 1: left to L, right to R, as shipped; and
     * its mono sum is the source's mono sum (nothing cancels or doubles). */
    cfg.bus[0].pan_width = 1.0f;
    cfg.bus[0].pan_y = -0.5f;                           /* ignored on a stereo output */
    memset(out, 0, sizeof(out));
    run_one(&cfg, 2, 0, &v, out);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        CHECK(fabsf(out[0][i] - 0.5f * v.samples[i][0]) < 1e-6f, "stereo output: L is not the left channel");
        CHECK(fabsf(out[0][i] + out[1][i] - 0.5f * (v.samples[i][0] + v.samples[i][1])) < 1e-6f,
              "stereo output: the mono sum is not the source's");
    }
    /* A mono voice with width: on the neighbours too, in phase. */
    {
        tvoice t = v;
        t.channels = 1;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) t.samples[i][1] = t.samples[i][0];
        apu_bus_config_neutral(&cfg);
        cfg.bus[4].route = APU_BUS_ROUTE_PAN;
        cfg.bus[4].pan_width = 0.5f;
        memset(out, 0, sizeof(out));
        run_one(&cfg, 6, 4, &t, out);
        CHECK(bin_rms(out[0]) > 0.05 && bin_rms(out[1]) > 0.05 && bin_rms(out[2]) > 0.05,
              "a mono voice with width 0.5 at the front does not use FL, C and FR");
        CHECK(fabs(bin_rms(out[0]) - bin_rms(out[1])) < 1e-6, "its FL and FR differ");
    }

    /* Field shaping: the pair's width and the centre's divergence, power
     * kept, over a grid of pans. */
    {
        double worst_w = 0, worst_d = 0;
        for (int il = 0; il <= 8; il++)
            for (int ir = 0; ir <= 8; ir++)
                for (int iw = 0; iw <= 8; iw++) {
                    float l = il / 8.0f, r = ir / 8.0f, c = (float)((il + ir) % 5) / 4.0f;
                    float l2 = l, r2 = r, c2 = c;
                    const double p0 = (double)l * l + (double)r * r;
                    if (p0 == 0) continue;
                    apu_bus_field_width(iw / 4.0f, &l2, &r2);
                    if (fabs(db(sqrt(((double)l2 * l2 + (double)r2 * r2) / p0))) > worst_w)
                        worst_w = fabs(db(sqrt(((double)l2 * l2 + (double)r2 * r2) / p0)));
                    CHECK(l2 >= 0 && r2 >= 0, "field width made a negative gain");
                    if (iw == 4) CHECK(fabsf(l2 - l) < 1e-6f && fabsf(r2 - r) < 1e-6f, "field width 1 changes the pair");
                    if (iw == 0) CHECK(fabsf(l2 - r2) < 1e-6f, "field width 0 is not the centre line");
                    if (iw > 4 && l > r) CHECK(l2 >= l - 1e-6f && r2 <= r + 1e-6f, "field width > 1 does not widen");
                    l2 = l; r2 = r;
                    apu_bus_field_divergence((iw - 4) / 4.0f, &l2, &r2, &c2);
                    {
                        const double p1 = p0 + (double)c * c, p2 = (double)l2 * l2 + (double)r2 * r2 + (double)c2 * c2;
                        if (fabs(db(sqrt(p2 / p1))) > worst_d) worst_d = fabs(db(sqrt(p2 / p1)));
                    }
                    if (iw == 8) CHECK(c2 < 1e-6f, "field divergence 1 leaves a centre send");
                    if (iw == 0) CHECK(l2 < 1e-3f || r2 < 1e-3f, "field divergence -1 leaves a common part in FL and FR");
                }
        CHECK(worst_w < 0.01 && worst_d < 0.01, "field shaping: power off by %.4f / %.4f dB", worst_w, worst_d);
    }
    /* Through the mixer: a positioned voice, mostly front left. */
    memset(&v, 0, sizeof(v));
    v.channels = 1;
    memcpy(v.bin, k_bins[2], sizeof(v.bin));            /* FL FR C LFE SL SR, reverb */
    v.g[0] = 0.5f; v.g[1] = 0.1f; v.g[2] = 0.2f; v.g[3] = 0.05f; v.g[4] = 0.3f; v.g[5] = 0.1f;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        v.samples[i][0] = v.samples[i][1] = sinf((float)i * 0.7f);
    {
        static float ref[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
        double p_ref = 0, p = 0;
        memset(ref, 0, sizeof(ref));
        old_mix(ref, &v);
        for (int b = 0; b < NUM_MIXBINS; b++) p_ref += bin_rms(ref[b]) * bin_rms(ref[b]);
        apu_bus_config_neutral(&cfg);
        cfg.bus[9].field_width = 0.0f;
        memset(out, 0, sizeof(out));
        run_one(&cfg, 6, 9, &v, out);
        for (int b = 0; b < NUM_MIXBINS; b++) p += bin_rms(out[b]) * bin_rms(out[b]);
        CHECK(fabs(bin_rms(out[19]) - bin_rms(out[20])) < 1e-6 && fabs(bin_rms(out[14]) - bin_rms(out[15])) < 1e-6,
              "field width 0: the pairs are not centred");
        CHECK(!memcmp(out[16], ref[16], sizeof(ref[0])) && !memcmp(out[24], ref[24], sizeof(ref[0])),
              "field width touched the centre or the LFE");
        CHECK(fabs(db(sqrt(p / p_ref))) < 0.01, "field width 0 changed the voice's power by %.3f dB", db(sqrt(p / p_ref)));
        apu_bus_config_neutral(&cfg);
        cfg.bus[9].field_divergence = 1.0f;
        memset(out, 0, sizeof(out));
        p = 0;
        run_one(&cfg, 6, 9, &v, out);
        for (int b = 0; b < NUM_MIXBINS; b++) p += bin_rms(out[b]) * bin_rms(out[b]);
        CHECK(bin_rms(out[16]) < 1e-6 && bin_rms(out[19]) > bin_rms(ref[19]), "field divergence 1: the centre send is not in FL/FR");
        CHECK(fabs(db(sqrt(p / p_ref))) < 0.01, "field divergence changed the voice's power by %.3f dB", db(sqrt(p / p_ref)));
        /* Neutral values are not on the path at all: the same bytes as
         * the loop, whatever the pan settings of the unused route say. */
        apu_bus_config_neutral(&cfg);
        cfg.bus[9].pan_x = -0.7f; cfg.bus[9].pan_y = 0.2f; cfg.bus[9].pan_width = 0.3f; cfg.bus[9].pan_divergence = 1.0f;
        CHECK(apu_bus_config_is_neutral(&cfg), "pan settings of a bus on the title's route make it non-neutral");
        memset(out, 0, sizeof(out));
        run_one(&cfg, 6, 9, &v, out);
        CHECK(!memcmp(out, ref, sizeof(ref)), "a neutral bus with pan settings is not the loop's sum");
        cfg.bus[9].field_width = 1.01f;
        CHECK(!apu_bus_config_is_neutral(&cfg), "a field width of 1.01 reads as neutral");
    }
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
}

/* A voice's bus: what the classifier is told about a return voice (one
 * playing the GP model's export, apu_gp.h), and the answer for a voice that
 * is only the input of an in-place effect. */
static int s_seen_return;
static int classify_return(const apu_bus_voice *v)
{
    s_seen_return = v->gp_return;
    if (v->gp_return == 1 + 17) return 4;           /* Player engine */
    if (v->gp_return == 1 + 18) return 5;           /* Player exhaust */
    for (int b = 0; b < 8; b++)
        if (v->bins[b] == 17) return APU_BUS_BYPASS;
    return 99;                                      /* not a bus: "Other" */
}
static int fill_return(void *ctx, apu_bus_voice *v)
{
    const int *what = (const int *)ctx;
    v->gp_return = what[0];
    v->bins[5] = (uint8_t)what[1];
    return 1;
}

static void test_return_voices(void)
{
    int engine[2] = { 1 + 17, 19 }, exhaust[2] = { 1 + 18, 19 };
    int feeder[2] = { 0, 17 }, plain[2] = { 0, 19 };

    apu_bus_set_classifier(classify_return);
    apu_bus_reclassify();
    CHECK(apu_bus_voice_bus(68, 1, 0xBB8300, fill_return, engine) == 4 && s_seen_return == 18,
          "the return voice of bin 17 is not the engine bus");
    CHECK(apu_bus_voice_bus(69, 1, 0xBB8380, fill_return, exhaust) == 5,
          "the return voice of bin 18 is not the exhaust bus");
    CHECK(apu_bus_voice_bus(70, 1, 0x100000, fill_return, feeder) == APU_BUS_BYPASS,
          "an effect's input voice does not bypass the buses");
    /* Cached: not asked again for the same voice, serial and buffer. */
    s_seen_return = -1;
    CHECK(apu_bus_voice_bus(70, 1, 0x100000, fill_return, plain) == APU_BUS_BYPASS && s_seen_return == -1,
          "a bypassed voice was classified again");
    CHECK(apu_bus_voice_bus(71, 1, 0x200000, fill_return, plain) == 12,
          "an answer that is no bus is not Other");
    apu_bus_set_classifier(NULL);
    apu_bus_reclassify();
}

int main(void)
{
    dsp_setup();
    apu_bus_set_names(k_names, 13);
    test_identity();
    test_controls();
    test_output();
    test_duck();
    test_preset();
    test_pan();
    test_return_voices();
    /* And once more after everything else ran: neutral is still today. */
    test_identity();
    if (s_fail) { fprintf(stderr, "%d check(s) failed\n", s_fail); return 1; }
    puts("PASS: the neutral mixer is bit-identical; controls, output stage, ducking and presets as specified.");
    return 0;
}
