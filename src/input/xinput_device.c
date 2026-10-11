/**
 * Xbox Input Compatibility Layer
 *
 * The host backends behind input_core.c (input_backend.h): each one reads
 * a host pad and maps it to the Duke layout through the table in
 * input_map.c, so both hosts map controls the same way.
 *
 *   _WIN32 -> Windows XInput (also Proton/Wine)
 *   POSIX  -> SDL2 GameController
 */

#include "input_backend.h"
#include "recomp_env.h"
#include "input_map.h"
#include "keyboard.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32) && !defined(XBOXRECOMP_WIN_VULKAN)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

/* The keyboard on port 0 is shared with the SDL backend (keyboard.c); the
 * window procedures in fb_present.c and nv2a_pb_d3d11.c record the keys. */

_Static_assert(sizeof(XINPUT_GAMEPAD) == sizeof(XBOX_HOST_XINPUT_GAMEPAD)
               && offsetof(XINPUT_GAMEPAD, bLeftTrigger) == offsetof(XBOX_HOST_XINPUT_GAMEPAD, bLeftTrigger)
               && offsetof(XINPUT_GAMEPAD, sThumbRY) == offsetof(XBOX_HOST_XINPUT_GAMEPAD, sThumbRY),
               "XBOX_HOST_XINPUT_GAMEPAD must match XINPUT_GAMEPAD");

static DWORD xi_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    XINPUT_STATE xi;
    XBOX_HOST_XINPUT_GAMEPAD pad;
    DWORD rc = XInputGetState(port, &xi);

    if (rc != ERROR_SUCCESS)
        return rc;
    memcpy(&pad, &xi.Gamepad, sizeof pad);
    out->dwPacketNumber = xi.dwPacketNumber;
    xbox_InputMapXInput(&pad, &out->Gamepad);
    return ERROR_SUCCESS;
}

static DWORD xi_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    XINPUT_VIBRATION xv;
    xv.wLeftMotorSpeed  = v->wLeftMotorSpeed;
    xv.wRightMotorSpeed = v->wRightMotorSpeed;
    return XInputSetState(port, &xv);
}

static uint64_t xi_now_ms(void) { return GetTickCount64(); }

/* Under Wine XInputGetState has been seen to return ERROR_SUCCESS and an
 * idle pad for an empty user index. If a Wine/Proton build does that, the
 * connection probe has to move to XInputGetCapabilities. */
const xbox_input_backend xbox_input_host_backend = {
    NULL, xi_get_state, xi_set_state, xbox_InputKeyboardPoll, xi_now_ms
};

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* Port -> controller. Read by the pad thread (OHCI) under g_pads_lock;
 * changed only by the thread that pumps SDL events (or, headless, by the
 * pad thread itself). */
static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static pthread_mutex_t g_pads_lock = PTHREAD_MUTEX_INITIALIZER;

/* Set by the window (src/video/fb_present_sdl.c) when the process main thread
 * runs the SDL event loop. Its SDL_PumpEvents already updates the
 * controllers, so this side only reads them: one event loop, not two threads
 * racing to pump. */
static volatile int g_main_pump;
void xbox_InputSetMainPump(int on) { g_main_pump = on; }

/* Whether a controller is open, i.e. whether SDL may still be in use here. */
int xbox_InputAnyOpen(void)
{
    int any = 0;
    pthread_mutex_lock(&g_pads_lock);
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i])
            any = 1;
    pthread_mutex_unlock(&g_pads_lock);
    return any;
}
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

_Static_assert(SDL_CONTROLLER_BUTTON_A == XBOX_SDL_BUTTON_A
               && SDL_CONTROLLER_BUTTON_START == XBOX_SDL_BUTTON_START
               && SDL_CONTROLLER_BUTTON_LEFTSHOULDER == XBOX_SDL_BUTTON_LEFTSHOULDER
               && SDL_CONTROLLER_BUTTON_DPAD_RIGHT == XBOX_SDL_BUTTON_DPAD_RIGHT,
               "XBOX_SDL_BUTTON_* must match SDL_GameControllerButton");
_Static_assert(SDL_CONTROLLER_AXIS_LEFTX == XBOX_SDL_AXIS_LEFTX
               && SDL_CONTROLLER_AXIS_RIGHTY == XBOX_SDL_AXIS_RIGHTY
               && SDL_CONTROLLER_AXIS_TRIGGERRIGHT == XBOX_SDL_AXIS_TRIGGERRIGHT,
               "XBOX_SDL_AXIS_* must match SDL_GameControllerAxis");

/* ── With a window: every game controller open, ports by first input ──
 *
 * The window's thread pumps SDL events, and everything that opens, closes
 * or enumerates controllers happens there (xbox_InputPadEvent): under
 * sdl2-compat a joystick call from a second thread races the event pump
 * (fb_present_sdl.c, BO-76). Every attached game controller is opened, and
 * a port goes to a controller when it first presses a button: player 1 is
 * the pad the player picked up. (Not an axis: a device whose trigger rests
 * at full, a mapping off by a sign, would take port 1 by doing nothing.)
 * Ports in SDL's device order put some other device on port 1 on the
 * Bazzite and the Steam Controller (Steam Input's virtual pad) on port 2,
 * while the title reads player 1. Hotplug: a controller added is opened,
 * one removed frees its port.
 *
 * Without a window (headless, nothing pumps) the pad thread opens the
 * controllers present at its first read, in device order, as before. */
#define PAD_OPEN_MAX 8
static struct {
    SDL_GameController *c;
    SDL_JoystickID id;
    int port;                         /* -1: no input yet */
} g_open[PAD_OPEN_MAX];

static void pad_describe(SDL_GameController *c, char *out, size_t n)
{
    SDL_Joystick *j = SDL_GameControllerGetJoystick(c);
    const char *name = SDL_GameControllerName(c);
    snprintf(out, n, "\"%s\" %04x:%04x player %d", name ? name : "?",
             SDL_GameControllerGetVendor(c), SDL_GameControllerGetProduct(c),
             j ? SDL_JoystickGetPlayerIndex(j) : -1);
}

/* Window thread: open the controller at device index i if it is not open. */
static void pad_open_index(int i)
{
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
    int k, free_k = -1;
    char d[160];

    if (!SDL_IsGameController(i))
        return;
    for (k = 0; k < PAD_OPEN_MAX; k++) {
        if (g_open[k].c && g_open[k].id == id)
            return;
        if (!g_open[k].c && free_k < 0)
            free_k = k;
    }
    if (free_k < 0)
        return;
    g_open[free_k].c = SDL_GameControllerOpen(i);
    if (!g_open[free_k].c)
        return;
    g_open[free_k].id = id;
    g_open[free_k].port = -1;
    pad_describe(g_open[free_k].c, d, sizeof d);
    fprintf(stderr, "[INPUT] controller %s opened; it gets a port at its first input\n", d);
}

/* Window thread: the lowest free port for the controller with this
 * instance id, if it has none yet. */
static void pad_claim(SDL_JoystickID id)
{
    char d[160];

    for (int k = 0; k < PAD_OPEN_MAX; k++) {
        if (!g_open[k].c || g_open[k].id != id || g_open[k].port >= 0)
            continue;
        pthread_mutex_lock(&g_pads_lock);
        for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
            if (!g_pads[p]) {
                g_pads[p] = g_open[k].c;
                g_open[k].port = p;
                break;
            }
        }
        pthread_mutex_unlock(&g_pads_lock);
        if (g_open[k].port >= 0) {
            pad_describe(g_open[k].c, d, sizeof d);
            fprintf(stderr, "[INPUT] controller %s -> port %d (player %d), at its first input\n",
                    d, g_open[k].port, g_open[k].port + 1);
        }
        return;
    }
}

void xbox_InputPadEvent(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_CONTROLLERDEVICEADDED:
        pad_open_index(ev->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        for (int k = 0; k < PAD_OPEN_MAX; k++) {
            if (!g_open[k].c || g_open[k].id != ev->cdevice.which)
                continue;
            pthread_mutex_lock(&g_pads_lock);
            if (g_open[k].port >= 0)
                g_pads[g_open[k].port] = NULL;
            SDL_GameControllerClose(g_open[k].c);
            pthread_mutex_unlock(&g_pads_lock);
            if (g_open[k].port >= 0)
                fprintf(stderr, "[INPUT] controller on port %d removed; the port is free\n",
                        g_open[k].port);
            else
                fprintf(stderr, "[INPUT] controller removed (it had no port)\n");
            g_open[k].c = NULL;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        pad_claim(ev->cbutton.which);
        break;
    default:
        break;
    }
}

/* Headless: up to XBOX_MAX_CONTROLLERS attached game controllers, in
 * device order, on the pad thread (nothing else uses SDL then). */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot])
            g_pads[slot] = SDL_GameControllerOpen(i);
        slot++;
    }
}

static void sdl_init(void)
{
    /* Game controllers pull in SDL events; keep SIGINT/SIGTERM default. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (g_main_pump) {
        /* The window's thread (xbox_InputInit from fb_present_sdl.c): open
         * what is attached now; the ADDED events SDL also sends for these
         * find them open. */
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            pad_open_index(i);
        return;
    }
    pthread_mutex_lock(&g_pads_lock);
    open_controllers();
    pthread_mutex_unlock(&g_pads_lock);
}

static DWORD sdl_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    SDL_GameController *c;
    XBOX_HOST_SDL_GAMEPAD pad;

    /* Held across the reads: the window's thread closes a removed
     * controller only under the lock. */
    pthread_mutex_lock(&g_pads_lock);
    c = g_pads[port];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        pthread_mutex_unlock(&g_pads_lock);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!g_main_pump)
        SDL_GameControllerUpdate();

    for (int b = 0; b < XBOX_SDL_BUTTON_COUNT; b++)
        pad.button[b] = SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b) ? 1 : 0;
    for (int a = 0; a < XBOX_SDL_AXIS_COUNT; a++)
        pad.axis[a] = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a);
    pthread_mutex_unlock(&g_pads_lock);
    out->dwPacketNumber = ++g_packet[port];
    xbox_InputMapSdl(&pad, &out->Gamepad);
    return ERROR_SUCCESS;
}

/* The title's motor speeds were written for the Duke/S pad's motors; a
 * DualShock or the Deck's haptics barely move at the low values Burnout 3
 * decays through (18349 at an impact, then 5734, 1791...). A square-root
 * curve keeps zero at zero and full at full but lifts the middle, the way
 * the console motors' start-up threshold made small speeds felt.
 * RECOMP_INPUT_RUMBLE_GAIN scales it; "linear" passes the values through. */
static int g_rumble_init, g_rumble_linear;
static double g_rumble_gain = 1.0;

/* The strength, live (the overlay's Input page): gain 0 silences the
 * motors, a negative value passes the title's values through ("linear"). */
void xbox_InputSetRumbleGain(double gain)
{
    g_rumble_init = 1;
    g_rumble_linear = gain < 0.0;
    g_rumble_gain = gain < 0.0 ? 1.0 : gain;
}

static Uint16 rumble_level(WORD speed)
{
    double f;

    if (!g_rumble_init) {
        const char *g = recomp_env(RENV_INPUT_RUMBLE_GAIN);
        if (g && !strcmp(g, "linear"))
            g_rumble_linear = 1;
        else if (g && *g && atof(g) >= 0.0)
            g_rumble_gain = atof(g);
        g_rumble_init = 1;
    }
    if (!speed || g_rumble_linear)
        return speed;
    f = g_rumble_gain * sqrt(speed / 65535.0);
    return f >= 1.0 ? 65535 : (Uint16)(f * 65535.0);
}

static DWORD sdl_set_state(DWORD port, const XBOX_VIBRATION *v)
{
    SDL_GameController *c;

    pthread_mutex_lock(&g_pads_lock);
    c = g_pads[port];
    if (!c || !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        pthread_mutex_unlock(&g_pads_lock);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    /* SDL rumble needs a duration. The USB pad (usb_gamepad.c) re-issues a
     * held rumble every 150 ms, so 300 ms bridges one refresh and a lost
     * stop report ends within a third of a second. 0 stops it. */
#if defined(__ANDROID__)
    /* A handheld's built-in pad has no motors: the first pad's rumble goes
     * to the device's own vibrator then (android_vibration.c), and the pad
     * is told to stop. */
    if (port == 0) {
        extern int xbox_AndroidVibration(unsigned left, unsigned right, int pad_can_rumble);
        /* RECOMP_ANDROID_VIBRATION_FLOOR=<0..65535>: the title's motor
         * speeds under it are silence (its idle hum, lifted by the strength
         * setting, is otherwise a constant buzz in the hand). Default 0. */
        static int floor_v = -1;
        unsigned l = v->wLeftMotorSpeed, r = v->wRightMotorSpeed;
        if (floor_v < 0) {
            const char *f = getenv("RECOMP_ANDROID_VIBRATION_FLOOR");
            floor_v = f ? atoi(f) : 0;
        }
        if ((int)l < floor_v) l = 0;
        if ((int)r < floor_v) r = 0;
        if (!xbox_AndroidVibration(rumble_level((WORD)l), rumble_level((WORD)r),
                                   SDL_GameControllerHasRumble(c) == SDL_TRUE)) {
            SDL_GameControllerRumble(c, 0, 0, 0);
            pthread_mutex_unlock(&g_pads_lock);
            return ERROR_SUCCESS;
        }
    }
#endif
    SDL_GameControllerRumble(c, rumble_level(v->wLeftMotorSpeed),
                             rumble_level(v->wRightMotorSpeed),
                             (v->wLeftMotorSpeed || v->wRightMotorSpeed) ? 300 : 0);
    pthread_mutex_unlock(&g_pads_lock);
    return ERROR_SUCCESS;
}

static uint64_t sdl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

const xbox_input_backend xbox_input_host_backend = {
    sdl_init, sdl_get_state, sdl_set_state, xbox_InputKeyboardPoll, sdl_now_ms
};

#endif /* _WIN32 */
