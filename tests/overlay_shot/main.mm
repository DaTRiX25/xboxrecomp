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

#import <Metal/Metal.h>
#include <SDL.h>
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

int main(int argc, char **argv)
{
    const char *pre = argc > 1 ? argv[1] : "overlay_";
    char path[512];

    if (argc > 3) { W = atoi(argv[2]); H = atoi(argv[3]); }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("RECOMP_SETTINGS", "none", 1);
    setenv("RECOMP_OVERLAY_PERF", "1", 1);
    xbox_SettingsInit("Test");
    xbox_SettingsFinish();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 2; }
    SDL_Window *win = SDL_CreateWindow("shot", 0, 0, W, H, 0);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 2; }
    dev = MTLCreateSystemDefaultDevice();
    q = [dev newCommandQueue];
    xbox_OverlaySetWindow(win);

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
