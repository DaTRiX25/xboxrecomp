/**
 * SDL2 Audio Output Backend (POSIX hosts)
 *
 * The xa2_* contract of apu_xaudio2.h on macOS and Linux: the APU frame
 * thread hands over one EP period (256 stereo frames) at a time, and the
 * device queue paces it (mcpx_apu_pace_step reads xa2_queued_samples).
 * SDL's push API (SDL_QueueAudio) is the queue; its device thread pulls one
 * device buffer per callback and pads a short pull with silence, which is
 * why the pacing marks keep at least one device buffer queued.
 *
 * Headless runs (RECOMP_HEADLESS=1) use SDL's "dummy" driver unless
 * SDL_AUDIODRIVER is already set: it drains the queue in real time without
 * a device, so pacing and the WAV tap behave as on a real device and nothing
 * reaches the speakers. SDL_AUDIODRIVER=coreaudio (or pulseaudio, pipewire)
 * with RECOMP_HEADLESS=1 is the way to hear a headless run.
 *
 * 5.1: the device is opened with six channels when the title mixes 5.1 and
 * the default output takes six (or cannot say: SDL and the sound server
 * convert then), with two when the default output is known to be stereo, in
 * which case apu_core.c folds the period down first. SDL_GetDefaultAudioInfo
 * (SDL >= 2.24) is the probe; PipeWire, PulseAudio and CoreAudio answer it
 * with the channel count the system's output profile is set to. An audio
 * device added or removed (SDL_AUDIODEVICEADDED/REMOVED, seen by an event
 * watch on whichever thread SDL posts them from) makes the frame thread ask
 * again at its next period and reopen the device if the answer changed.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_speakers.h"
#include "apu_xaudio2.h"
#include "recomp_env.h"

#if !defined(_WIN32) || defined(XBOXRECOMP_WIN_VULKAN)

#include <SDL.h>
#include <time.h>

#define SDLA_SAMPLE_RATE   48000
#define SDLA_CHANNELS_MAX  6
/* Device buffer size and count: the same defaults and knobs as the XAudio2
 * backend (512 x 4: 10.7 ms per buffer, 43 ms queued at the high mark). The
 * count is a pacing mark here, not a slot limit: SDL's queue is unbounded. */
#define SDLA_BUF_SAMPLES_DEFAULT 512
#define SDLA_NUM_BUFS_DEFAULT    4
#define SDLA_BUF_SAMPLES_MIN     128
#define SDLA_BUF_SAMPLES_MAX     4096
#define SDLA_NUM_BUFS_MIN        2
#define SDLA_NUM_BUFS_MAX        8

static SDL_AudioDeviceID g_dev;
static int       g_buf_samples = SDLA_BUF_SAMPLES_DEFAULT;
static int       g_num_bufs    = SDLA_NUM_BUFS_DEFAULT;
static int       g_initialized;
static int       g_audio_opened;    /* this backend brought SDL audio up */
static int       g_frames_written;  /* periods handed to the queue */
static uint64_t  g_samples_queued;  /* samples handed to the queue, ever */
static unsigned  g_underruns;
static int       g_dry;
static int64_t   g_underrun_logged_ms;
static int       g_channels = 2;    /* the open device's, 2 or 6 */
static int       g_title_channels = 2;
static SDL_atomic_t g_hotplug;      /* a device came or went: probe again */
static int       g_watching;

static int env_int(recomp_env_id id, int def, int lo, int hi)
{
    int v = recomp_env_int(id, def);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The audio subsystem, brought up once by whichever of probe or init comes
 * first. */
static int audio_subsystem_up(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO))
        return 1;
    /* Headless: no speakers unless the caller names a driver. Must be set
     * before the audio subsystem starts; 0 keeps an existing value.
     * SDL_AUDIODRIVER is SDL's own variable, not one of ours. */
    if (recomp_env_on(RENV_HEADLESS))
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);

    /* Audio pulls in SDL's event subsystem, which would trap SIGINT and
     * SIGTERM into SDL_QUIT; nothing pumps events in a headless run, so
     * Ctrl-C would be swallowed. Keep the process defaults. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[SDL2] audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    g_audio_opened = 1;
    return 1;
}

int xa2_probe_channels(void)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_AudioSpec spec;
    char *name = NULL;

    if (!audio_subsystem_up())
        return 0;
    if (SDL_GetDefaultAudioInfo(&name, &spec, 0) != 0)
        return 0;
    SDL_free(name);
    return spec.channels;
#else
    return 0;
#endif
}

void xa2_request_channels(int title_channels)
{
    g_title_channels = title_channels == SDLA_CHANNELS_MAX ? SDLA_CHANNELS_MAX : 2;
}

/* SDL calls this from the thread posting the event; only a flag is set
 * here, the frame thread does the rest (xa2_channels). */
static int SDLCALL hotplug_watch(void *ud, SDL_Event *e)
{
    (void)ud;
    if ((e->type == SDL_AUDIODEVICEADDED || e->type == SDL_AUDIODEVICEREMOVED) &&
        !e->adevice.iscapture)
        SDL_AtomicSet(&g_hotplug, 1);
    return 1;
}

static int open_device(int channels)
{
    SDL_AudioSpec want, have;

    memset(&want, 0, sizeof(want));
    want.freq     = SDLA_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples  = (Uint16)g_buf_samples;
    want.callback = NULL;                       /* push: SDL_QueueAudio */

    /* No format changes allowed: the mixer's 48 kHz s16 is what the queue
     * takes, and SDL converts for the device if it has to. */
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_dev == 0) {
        fprintf(stderr, "[SDL2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    if (have.samples > 0 && have.samples != g_buf_samples) {
        fprintf(stderr, "[SDL2] device buffer is %d samples (asked %d)\n",
                have.samples, g_buf_samples);
        g_buf_samples = have.samples;
    }
    g_channels = channels;
    g_dry = 0;
    SDL_PauseAudioDevice(g_dev, 0);
    return 1;
}

int xa2_init(void)
{
    const char *driver;
    int probed;

    if (g_initialized) return 1;

    g_buf_samples = env_int(RENV_AUDIO_BUF_SAMPLES, SDLA_BUF_SAMPLES_DEFAULT,
                            SDLA_BUF_SAMPLES_MIN, SDLA_BUF_SAMPLES_MAX);
    g_num_bufs = env_int(RENV_AUDIO_BUF_COUNT, SDLA_NUM_BUFS_DEFAULT,
                         SDLA_NUM_BUFS_MIN, SDLA_NUM_BUFS_MAX);

    if (!audio_subsystem_up())
        return 0;

    probed = g_title_channels == SDLA_CHANNELS_MAX ? xa2_probe_channels() : 0;
    if (!open_device(apu_spk_device_channels(g_title_channels, probed))) {
        if (g_audio_opened) { SDL_QuitSubSystem(SDL_INIT_AUDIO); g_audio_opened = 0; }
        return 0;
    }
    g_frames_written = 0;
    g_initialized = 1;
    /* A stereo title plays on anything: only a 5.1 one follows the default
     * output around. */
    if (g_title_channels == SDLA_CHANNELS_MAX && !g_watching) {
        SDL_AtomicSet(&g_hotplug, 0);
        SDL_AddEventWatch(hotplug_watch, NULL);
        g_watching = 1;
    }

    driver = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "[SDL2] audio initialized: driver %s (%d Hz %s 16-bit, %d x %d-sample buffers, %.1f ms queued max)%s\n",
            driver ? driver : "?", SDLA_SAMPLE_RATE,
            g_channels == SDLA_CHANNELS_MAX ? "5.1" : "stereo",
            g_num_bufs, g_buf_samples,
            1000.0 * g_num_bufs * g_buf_samples / SDLA_SAMPLE_RATE,
            driver && !strcmp(driver, "dummy") ? " [silent: no device output]" : "");
    return 1;
}

void xa2_shutdown(void)
{
    if (g_watching) {
        SDL_DelEventWatch(hotplug_watch, NULL);
        g_watching = 0;
    }
    if (g_dev) {
        SDL_PauseAudioDevice(g_dev, 1);
        SDL_ClearQueuedAudio(g_dev);
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_audio_opened) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_opened = 0;
    }
    if (g_initialized)
        fprintf(stderr, "[SDL2] shut down (%d periods written, %u underruns)\n",
                g_frames_written, g_underruns);
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

int xa2_channels(void)
{
    int want;

    if (!g_initialized)
        return g_channels;
    if (!SDL_AtomicGet(&g_hotplug))
        return g_channels;
    SDL_AtomicSet(&g_hotplug, 0);
    want = apu_spk_device_channels(g_title_channels, xa2_probe_channels());
    /* A device we had open that is gone plays nothing (SDL drains its
     * queue in time, so pacing carries on): open the default again. */
    if (want == g_channels && g_dev &&
        SDL_GetAudioDeviceStatus(g_dev) != SDL_AUDIO_STOPPED)
        return g_channels;
    fprintf(stderr, "[SDL2] default output changed: reopening for %s\n",
            want == SDLA_CHANNELS_MAX ? "5.1" : "stereo (5.1 folded down)");
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (!open_device(want))
        g_initialized = 0;
    return g_channels;
}

/* Hand one EP period to the queue. SDL copies the samples, so the caller's
 * buffer is free on return. Nothing is ever refused by depth: the queue is
 * unbounded and pacing by queue depth holds it at the marks. */
static volatile float g_gain = 1.0f;
static float g_gain_applied = 1.0f;     /* the submit side's, last period */

void xa2_set_gain(float gain)
{
    g_gain = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int16_t scaled[1024 * SDLA_CHANNELS_MAX];
    const int ch = g_channels;
    float target = g_gain;

    if (!g_initialized || !g_dev || num_samples <= 0) return 0;
    if (target != 1.0f || g_gain_applied != 1.0f) {
        /* Scaled in chunks, the gain stepping linearly from the last
         * period's to the new one across this call. */
        int done = 0;
        float from = g_gain_applied;
        while (done < num_samples) {
            int n = num_samples - done < 1024 ? num_samples - done : 1024, i;
            for (i = 0; i < n; i++) {
                float g = from + (target - from) * (float)(done + i + 1) / (float)num_samples;
                int c;
                for (c = 0; c < ch; c++) {
                    int16_t v = samples[(size_t)(done + i) * ch + c];
                    scaled[i * ch + c] = (int16_t)((float)v * g);
                }
            }
            if (SDL_QueueAudio(g_dev, scaled,
                               (Uint32)n * ch * sizeof(int16_t)) != 0) {
                fprintf(stderr, "[SDL2] SDL_QueueAudio failed: %s\n", SDL_GetError());
                return 0;
            }
            done += n;
        }
        g_gain_applied = target;
        g_frames_written++;
        g_samples_queued += (uint64_t)num_samples;
        return 1;
    }
    if (SDL_QueueAudio(g_dev, samples,
                       (Uint32)num_samples * ch * sizeof(int16_t)) != 0) {
        fprintf(stderr, "[SDL2] SDL_QueueAudio failed: %s\n", SDL_GetError());
        return 0;
    }
    g_frames_written++;
    g_samples_queued += (uint64_t)num_samples;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return g_buf_samples;
}

int xa2_get_buffer_count(void)
{
    return g_num_bufs;
}

/* Samples queued and not yet pulled by the device thread. The device holds
 * up to one more buffer in flight, which this does not count, the same way
 * XAudio2's BuffersQueued does not count the one playing. Also the underrun
 * detector: an empty queue after audio has started means the device's last
 * pull was short and padded with silence; counted once per dry spell and
 * reported at most once a second. A drain announced by xa2_expect_drain
 * (the frame thread pausing) is not one. */
int xa2_queued_samples(void)
{
    Uint32 bytes;

    if (!g_initialized || !g_dev) return 0;
    bytes = SDL_GetQueuedAudioSize(g_dev);
    if (bytes == 0 && g_frames_written > 0) {
        if (!g_dry) {
            int64_t now = now_ms();
            g_dry = 1;
            g_underruns++;
            if (now - g_underrun_logged_ms >= 1000) {
                g_underrun_logged_ms = now;
                fprintf(stderr, "[SDL2] underrun x%u\n", g_underruns);
            }
        }
    } else {
        g_dry = 0;
    }
    return (int)(bytes / (g_channels * sizeof(int16_t)));
}

void xa2_expect_drain(void)
{
    g_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_underruns;
}

/* What the device thread has pulled off the queue; it pulls one device
 * buffer per callback, so this leads the speaker by up to one buffer. */
uint64_t xa2_samples_played(void)
{
    uint64_t queued;

    if (!g_initialized || !g_dev) return 0;
    queued = SDL_GetQueuedAudioSize(g_dev) / (g_channels * sizeof(int16_t));
    return g_samples_queued > queued ? g_samples_queued - queued : 0;
}

#endif /* !_WIN32 */
