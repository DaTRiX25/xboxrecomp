/*
 * overlay_shot - the overlay on Metal, off screen: no window is shown and no
 * game runs. SDL's dummy video driver gives ImGui a window to read its size
 * from; the frames are drawn into a texture the tool makes, with synthetic
 * key and controller events, and written as BMPs. Checks the pause and input
 * routing on the way (the open overlay pauses and blocks, closing undoes it).
 *
 *   overlay_shot <out prefix> [width height]
 */
#include "overlay.h"
#include "overlay_metal.h"
#include "xbox_settings.h"
#include "apu_bus.h"

#import <Metal/Metal.h>
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not the headers: their Windows type names clash with Objective-C's BOOL. */
extern "C" int xbox_InputBlocked(void);
extern "C" int xbox_HostPaused(void);
extern "C" void xbox_HostPauseHold(void);

/* The kernel library wants the title's function table; there is no title. */
extern "C" void *recomp_lookup(unsigned va) { (void)va; return NULL; }
extern "C" void *recomp_lookup_manual(unsigned va) { (void)va; return NULL; }

/* The bug report's maker (xbox_report.h), stood in for: the overlay's two
 * actions call it and show where the archive went, the home folder as ~. */
extern "C" void xbox_BugReportSetMaker(int (*fn)(int screenshot, char *out_path, unsigned n));
static int s_reports, s_report_shot;
static int fake_report(int screenshot, char *out, unsigned n)
{
    const char *home = getenv("HOME");
    s_reports++;
    s_report_shot = screenshot;
    snprintf(out, n, "%s/Library/Application Support/Test/reports/test-report-20260101-000000.zip",
             home ? home : "");
    return 1;
}

extern "C" void xbox_InputOfferControlScheme(int on);    /* input_map.h */

static int s_fail;
#define CHECK(c) do { if (!(c)) { s_fail++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static id<MTLDevice> dev;
static id<MTLCommandQueue> q;
static id<MTLTexture> tex;
static int W = 1280, H = 720;

static void key(SDL_Keycode k, int mod = 0, bool down = true)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.sym = k;
    e.key.keysym.mod = (Uint16)mod;
    xbox_OverlayEvent(&e);
}

static void tap(SDL_Keycode k)
{
    key(k);
    key(k, 0, false);
}

static void pad(int button, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.cbutton.which = 0;
    e.cbutton.button = (Uint8)button;
    xbox_OverlayEvent(&e);
}

static void frame(const char *save)
{
    @autoreleasepool {
        xbox_OverlayPoll();
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        if (!tex)
            tex = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.4, 0.55, 1);   /* a stand-in for the game */
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (xbox_OverlayWants())
            xbox_OverlayMetalDraw((__bridge void *)dev, (__bridge void *)cb, (__bridge void *)rp, (__bridge void *)enc);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (save) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
            [tex getBytes:s->pixels bytesPerRow:(NSUInteger)s->pitch fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            SDL_SaveBMP(s, save);
            SDL_FreeSurface(s);
            fprintf(stderr, "shot %s\n", save);
        }
    }
}

static void run(int n)
{
    for (int i = 0; i < n; i++) {
        frame(NULL);
        SDL_Delay(20);
    }
}

/* OVERLAY_SHOT_MIXER=<preset file>: the Mixer page (BO-114) instead of the
 * tour of the pages: thirteen buses as a title registers them, a little
 * signal for the meters, and the page driven with keys. */
static void frame(const char *save);
static void run(int n);

static void mixer_signal(void)
{
    static const int8_t map[32] = { 0, 1, 2, 3, 4, 5, 0, 1, 4, 5, 0, 1, 0, 1, 4, 5, 2, 0, 1,
                                    0, 1, 4, 5, 2, 3, 0, -1, 1, 0, 1, 0, 1 };
    static float bins[32][32], feeds[6][32], smp[32][2];
    static const int bin[8] = { 0, 1, 4, 5, 2, 3, 6, 7 };
    static unsigned t;
    for (int f = 0; f < 400; f++) {
        struct apu_bus_rt *rt = apu_bus_frame_begin(6, map);
        if (!rt) return;
        memset(bins, 0, sizeof bins);
        for (int bus = 0; bus < 12; bus++) {
            float g[8] = { 0.05f * (float)(bus + 1), 0.04f * (float)(bus + 1), 0.02f, 0.02f, 0, 0.01f, 0, 0 };
            for (int i = 0; i < 32; i++)
                smp[i][0] = smp[i][1] = sinf((float)(t + (unsigned)i) * 0.05f * (float)(bus + 1));
            apu_bus_mix_voice(rt, bus, bins, (const float (*)[2])smp, 1, bin, g);
        }
        apu_bus_frame_end(rt, bins);
        for (int c = 0; c < 6; c++)
            memcpy(feeds[c], bins[c], sizeof feeds[c]);
        apu_bus_output(feeds, 6, 32);
        t += 32;
    }
}

static int mixer_tour(const char *pre, const char *file)
{
    static const char *const names[13] = {
        "Music", "DJ", "Film", "Streams", "Player engine", "Player exhaust", "Other cars",
        "Traffic", "Tyres/road/wind", "Impacts/crash", "Boost", "UI/HUD", "Other" };
    char path[512];
    apu_bus_config c;

    remove(file);
    apu_bus_set_names(names, 13);
    apu_bus_add_preset("Factory", "The game's own mix.", "");
    apu_bus_add_preset("Music front", "Music on the front pair only.",
                       "[bus.music]\nroute = \"sends\"\nsend_fl_db = 0\nsend_fr_db = 0\n");
    apu_bus_set_file(file);

    tap(SDLK_F1);
    run(12);
    tap(SDLK_PAGEDOWN); run(2); tap(SDLK_PAGEDOWN); run(3);       /* Audio */
    for (int i = 0; i < 5; i++) { tap(SDLK_DOWN); run(1); }        /* the Mixer row */
    snprintf(path, sizeof path, "%smixer_0_audio.bmp", pre); frame(path);
    tap(SDLK_RETURN); run(3);
    CHECK(!apu_bus_enabled());
    snprintf(path, sizeof path, "%smixer_1_off.bmp", pre); frame(path);
    tap(SDLK_RETURN); run(3);                                      /* Mixer: On */
    CHECK(apu_bus_enabled() && xbox_SettingValue(XS_AUDIO_MIXER) != 0);
    mixer_signal();
    for (int i = 0; i < 5; i++) { tap(SDLK_DOWN); run(1); }        /* Music */
    for (int i = 0; i < 4; i++) { tap(SDLK_RIGHT); run(1); }       /* +2 dB */
    apu_bus_get_config(&c);
    CHECK(c.bus[0].gain_db == 2.0f);
    tap(SDLK_DOWN); run(1); tap(SDLK_PAGEUP); run(1);              /* DJ: mute */
    tap(SDLK_DOWN); run(1); tap(SDLK_PAGEDOWN); run(1);            /* Film: solo */
    apu_bus_get_config(&c);
    CHECK(c.bus[1].mute && c.bus[2].solo);
    tap(SDLK_PAGEDOWN); run(1); tap(SDLK_UP); run(1); tap(SDLK_PAGEUP); run(1);
    apu_bus_get_config(&c);
    CHECK(!c.bus[1].mute && !c.bus[2].solo);
    mixer_signal();
    snprintf(path, sizeof path, "%smixer_2_main.bmp", pre); frame(path);
    tap(SDLK_UP); run(1);                                          /* Music */
    tap(SDLK_RETURN); run(3);                                      /* its page */
    for (int i = 0; i < 3; i++) { tap(SDLK_DOWN); run(1); }        /* Route */
    snprintf(path, sizeof path, "%smixer_3_bus_title.bmp", pre); frame(path);
    tap(SDLK_RIGHT); run(2);
    apu_bus_get_config(&c);
    CHECK(c.bus[0].route == APU_BUS_ROUTE_SENDS);
    for (int i = 0; i < 3; i++) { tap(SDLK_DOWN); run(1); }        /* send to centre: off -> -60 -> up */
    tap(SDLK_RIGHT); run(1);
    apu_bus_get_config(&c);
    CHECK(c.bus[0].send_db[2] == -60.0f);
    tap(SDLK_LEFT); run(1);
    apu_bus_get_config(&c);
    CHECK(c.bus[0].send_db[2] <= APU_BUS_OFF_DB);
    snprintf(path, sizeof path, "%smixer_4_bus_sends.bmp", pre); frame(path);
    tap(SDLK_ESCAPE); run(3);                                      /* back to the list */
    CHECK(xbox_OverlayOpen());
    /* A/B: B is Factory. */
    tap(SDLK_F7); run(2);
    apu_bus_get_config(&c);
    CHECK(apu_bus_config_is_neutral(&c));
    snprintf(path, sizeof path, "%smixer_5_b.bmp", pre); frame(path);
    tap(SDLK_F7); run(2);
    apu_bus_get_config(&c);
    CHECK(c.bus[0].gain_db == 2.0f && c.bus[0].route == APU_BUS_ROUTE_SENDS);
    /* The file follows, a moment after the last change. */
    SDL_Delay(500); run(3);
    {
        apu_bus_config f;
        char err[160];
        apu_bus_config_neutral(&f);
        CHECK(apu_bus_load_file(file, &f, err, sizeof err) == 0);
        CHECK(f.bus[0].gain_db == 2.0f && f.bus[0].route == APU_BUS_ROUTE_SENDS);
    }
    /* Master rows, the speakers and the duck views. */
    for (int i = 0; i < 13; i++) { tap(SDLK_DOWN); run(1); }       /* Limiter */
    tap(SDLK_RIGHT); tap(SDLK_RIGHT); run(2);
    apu_bus_get_config(&c);
    CHECK(c.limiter == APU_BUS_LIM_PEAK);
    snprintf(path, sizeof path, "%smixer_6_master.bmp", pre); frame(path);
    for (int i = 0; i < 4; i++) { tap(SDLK_DOWN); run(1); }        /* Speakers */
    tap(SDLK_RETURN); run(3);
    snprintf(path, sizeof path, "%smixer_7_speakers.bmp", pre); frame(path);
    tap(SDLK_ESCAPE); run(2);
    tap(SDLK_DOWN); run(1); tap(SDLK_RETURN); run(3);              /* Ducking */
    tap(SDLK_RETURN); run(2);
    apu_bus_get_config(&c);
    CHECK(c.duck.enabled);
    snprintf(path, sizeof path, "%smixer_8_duck.bmp", pre); frame(path);
    tap(SDLK_ESCAPE); run(2);
    tap(SDLK_ESCAPE); run(2);                                      /* the Audio page */
    CHECK(xbox_OverlayOpen());
    tap(SDLK_ESCAPE); run(12);
    CHECK(!xbox_OverlayOpen());
    fprintf(stderr, s_fail ? "overlay_shot mixer: %d failed\n" : "overlay_shot mixer: ok\n", s_fail);
    return s_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *pre = argc > 1 ? argv[1] : "overlay_";
    char path[512];

    if (argc > 3) { W = atoi(argv[2]); H = atoi(argv[3]); }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("RECOMP_SETTINGS", "none", 1);
    setenv("RECOMP_OVERLAY_PERF", "1", 1);
    xbox_InputOfferControlScheme(1);     /* a title with a second driving layout: the Input page shows the row */
    xbox_SettingsInit("Test");
    xbox_SettingsFinish();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 2; }
    SDL_Window *win = SDL_CreateWindow("shot", 0, 0, W, H, 0);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 2; }
    dev = MTLCreateSystemDefaultDevice();
    q = [dev newCommandQueue];
    xbox_OverlaySetWindow(win);
    xbox_BugReportSetMaker(fake_report);
    if (getenv("OVERLAY_SHOT_MIXER"))
        return mixer_tour(pre, getenv("OVERLAY_SHOT_MIXER"));

    /* Flips for the graph: mostly 60 Hz, a few long ones. */
    for (int i = 0; i < 150; i++) {
        xbox_HostPauseHold();
        SDL_Delay(i % 37 == 5 ? 33 : i % 53 == 9 ? 24 : 16);
    }
    run(3);
    snprintf(path, sizeof path, "%sclosed_perf.bmp", pre);
    frame(path);

    CHECK(!xbox_HostPaused() && !xbox_InputBlocked());
    tap(SDLK_F1);
    CHECK(xbox_OverlayOpen() && xbox_HostPaused() && xbox_InputBlocked());
    run(12);
    snprintf(path, sizeof path, "%sdisplay.bmp", pre);
    frame(path);

    tap(SDLK_DOWN); tap(SDLK_DOWN); tap(SDLK_RIGHT);          /* filter: linear */
    run(2);
    CHECK(xbox_SettingValue(XS_DISPLAY_FILTER) == 1);
    tap(SDLK_DOWN); tap(SDLK_RETURN);                         /* overscan: crop */
    run(2);
    CHECK(xbox_SettingValue(XS_DISPLAY_OVERSCAN) == 1);
    tap(SDLK_UP); tap(SDLK_UP); tap(SDLK_RIGHT);  /* aspect: 16:9 (restart) */
    run(2);
    CHECK(xbox_SettingValue(XS_DISPLAY_ASPECT) == 1 && xbox_SettingsNeedRestart());
    snprintf(path, sizeof path, "%sdisplay2.bmp", pre);
    frame(path);

    for (int p = 1; p < 6; p++) {
        pad(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
        pad(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, false);
        run(3);
        if (p == 2) {                                         /* audio */
            pad(SDL_CONTROLLER_BUTTON_DPAD_LEFT, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_LEFT, false);
            run(2);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);   /* speakers */
            run(2);
            for (int i = 0; i < 3; i++) {
                pad(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, true);
                pad(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, false);
                run(1);
            }
            CHECK(xbox_SettingValue(XS_AUDIO_SPEAKERS) != 2);   /* 5.1 cannot be picked */
        }
        snprintf(path, sizeof path, "%spage%d.bmp", pre, p);
        frame(path);
        if (p == 5) {                                         /* advanced: the bug report */
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);       /* past the info line */
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            run(2);
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(2);
            CHECK(s_reports == 1 && s_report_shot == 0);
            snprintf(path, sizeof path, "%sreport.bmp", pre);
            frame(path);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
            pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
            run(2);
            pad(SDL_CONTROLLER_BUTTON_A, true);
            pad(SDL_CONTROLLER_BUTTON_A, false);
            run(2);
            CHECK(s_reports == 2 && s_report_shot == 1);
        }
    }
    CHECK(xbox_SettingValue(XS_AUDIO_VOLUME) < 1.0);

    /* B closes: the title is let go. */
    pad(SDL_CONTROLLER_BUTTON_B, true);
    pad(SDL_CONTROLLER_BUTTON_B, false);
    xbox_OverlayPoll();
    CHECK(!xbox_OverlayOpen() && !xbox_HostPaused() && !xbox_InputBlocked());
    run(10);
    snprintf(path, sizeof path, "%sclosed.bmp", pre);
    frame(path);

    /* Back + Start opens it; Esc closes. */
    pad(SDL_CONTROLLER_BUTTON_BACK, true);
    CHECK(!xbox_OverlayOpen());
    pad(SDL_CONTROLLER_BUTTON_START, true);
    CHECK(xbox_OverlayOpen() && xbox_HostPaused());
    pad(SDL_CONTROLLER_BUTTON_START, false);
    pad(SDL_CONTROLLER_BUTTON_BACK, false);
    tap(SDLK_ESCAPE);
    xbox_OverlayPoll();
    CHECK(!xbox_OverlayOpen() && !xbox_HostPaused());
    /* Cmd+, opens it. */
    key(SDLK_COMMA, KMOD_LGUI);
    CHECK(xbox_OverlayOpen());
    key(SDLK_F1, 0, false);
    tap(SDLK_F1);
    CHECK(!xbox_OverlayOpen());

    fprintf(stderr, "overlay_shot: %s\n", s_fail ? "FAILED" : "ok");
    xbox_OverlayShutdown();
    return s_fail ? 1 : 0;
}
