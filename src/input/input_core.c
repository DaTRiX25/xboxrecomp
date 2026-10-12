/**
 * xbox_input -- the public API over a host backend, with the per-port
 * connection cache.
 *
 * Connected ports are read at every call. A port last seen disconnected is
 * not asked again until XBOX_INPUT_PROBE_MS has passed since the last probe:
 * XInput's own guidance, because XInputGetState on an empty user index is
 * slow on Windows, and harmless elsewhere. xbox_InputIsConnected() answers
 * from the cache and refreshes it under the same rule, so a pad plugged in
 * later shows up within a second while the title polls the mask every frame.
 *
 * Rumble: a title's motor speeds pass through one curve here (rumble_curve.h:
 * peak, gate, strength, release) before any backend sees them, so the SDL
 * pads, Windows XInput and a handheld's own vibrator are all given the same
 * thing. The release needs a clock: xbox_InputRumbleTick, from the pad poll.
 *
 * Nothing here calls the platform but for a lock: the backend
 * (input_backend.h) does, and tests/input_map swaps in a fake one to count
 * the calls.
 */
#include "input_backend.h"
#include "rumble_curve.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static SRWLOCK g_rumble_lock = SRWLOCK_INIT;
#define RUMBLE_LOCK()   AcquireSRWLockExclusive(&g_rumble_lock)
#define RUMBLE_UNLOCK() ReleaseSRWLockExclusive(&g_rumble_lock)
#else
#include <pthread.h>
static pthread_mutex_t g_rumble_lock = PTHREAD_MUTEX_INITIALIZER;
#define RUMBLE_LOCK()   pthread_mutex_lock(&g_rumble_lock)
#define RUMBLE_UNLOCK() pthread_mutex_unlock(&g_rumble_lock)
#endif

static const xbox_input_backend *g_be = &xbox_input_host_backend;
static int      g_inited;
static BOOL     g_connected[XBOX_MAX_CONTROLLERS];
static uint64_t g_last_probe[XBOX_MAX_CONTROLLERS];
static int      g_probed[XBOX_MAX_CONTROLLERS];   /* ever probed */

/* ── rumble ───────────────────────────────────────────────────────────── */

/* A held rumble is given to the host again this often: SDL's rumble runs for
 * a set time (xinput_device.c), and a lost report ends with it. */
#define RUMBLE_REFRESH_MS 150
/* A falling level is given to the host no more often than this (a pad's
 * output report is a write to the device); a rise and the stop go at once. */
#define RUMBLE_SEND_MS 16

/* All under g_rumble_lock: the title's thread sets, the pad poll ticks, the
 * window's thread mutes. The lock is held across the backend's call. */
static struct rumble_port {
    WORD     target[2];         /* the title's last speeds, left and right */
    xbox_rumble_motor m[2];
    WORD     out[2];            /* what the host was last given */
    uint64_t step_ms, out_ms;
    int      live;              /* stepped before: step_ms means something */
} g_rumble[XBOX_MAX_CONTROLLERS];
static xbox_rumble_params g_rumble_params = XBOX_RUMBLE_PARAMS_DEFAULT;
static int g_rumble_params_read;
static int g_background;        /* the window is not in front */

/* The in-game overlay: while open the title reads neutral state; after it
 * closes, a port reads neutral until it was seen with nothing held. */
static volatile int g_blocked;
static volatile unsigned g_latched;   /* bit per port */

static int any_held(const XBOX_GAMEPAD *g)
{
    int i;
    if (g->wButtons)
        return 1;
    for (i = 0; i < 8; i++)
        if (g->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            return 1;
    return 0;
}

static void neutral(XBOX_GAMEPAD *g)
{
    memset(g, 0, sizeof *g);
}

static volatile int g_combo_guard;

/* Pads a script drives (src/usb/usb_gamepad.c), for the overlay's note. */
static volatile unsigned g_scripted;

/* What each port's last read was made of (XBOX_INPUT_SRC_*). */
static volatile unsigned g_sources[XBOX_MAX_CONTROLLERS];

unsigned xbox_InputSources(DWORD dwPort)
{
    return dwPort < XBOX_MAX_CONTROLLERS ? g_sources[dwPort] : 0;
}

void xbox_InputSetScriptedPads(unsigned mask) { g_scripted = mask; }
unsigned xbox_InputScriptedPads(void) { return g_scripted; }

void xbox_InputSetComboGuard(int on) { g_combo_guard = on != 0; }

int xbox_InputBlocked(void) { return g_blocked; }

/* RECOMP_INPUT_RUMBLE_GAIN and RECOMP_INPUT_RUMBLE_PEAK, once; the setters
 * below come after and replace them. A value that is no number is left at
 * the default. Lock held. */
static void rumble_params_read(void)
{
    const char *v;
    char *end;
    double d;

    if (g_rumble_params_read)
        return;
    g_rumble_params_read = 1;
    v = recomp_env(RENV_INPUT_RUMBLE_GAIN);
    if (v && *v) {
        d = strtod(v, &end);
        if (end != v && !*end)
            g_rumble_params.strength = d < 0.0 ? 0.0 : d;
    }
    v = recomp_env(RENV_INPUT_RUMBLE_PEAK);
    if (v && *v) {
        d = strtod(v, &end);
        if (end != v && !*end && d >= 1.0)
            g_rumble_params.peak = d > 65535.0 ? 65535.0 : d;
    }
}

void xbox_InputSetRumbleStrength(double strength)
{
    RUMBLE_LOCK();
    rumble_params_read();
    g_rumble_params.strength = strength > 0.0 ? strength : 0.0;
    RUMBLE_UNLOCK();
}

void xbox_InputSetRumblePeak(double peak)
{
    RUMBLE_LOCK();
    rumble_params_read();
    g_rumble_params.peak = peak >= 1.0 && peak <= 65535.0 ? peak : XBOX_RUMBLE_PEAK;
    RUMBLE_UNLOCK();
}

void xbox_InputSetRumbleGate(unsigned off, unsigned on)
{
    RUMBLE_LOCK();
    g_rumble_params.gate_off = off < on ? off : on;
    g_rumble_params.gate_on = on;
    RUMBLE_UNLOCK();
}

static int rumble_muted(void) { return g_blocked || g_background; }

static WORD rumble_word(double level)
{
    level = level * 65535.0 + 0.5;
    return level >= 65535.0 ? 65535 : level <= 0.0 ? 0 : (WORD)level;
}

/* Move a port's motors on to now and give the host what changed. Runs for a
 * port with no pad too (nothing is sent), so the levels are right when one
 * appears and RECOMP_TRACE=input_diag shows them on a machine without a pad.
 * Lock held. */
static DWORD rumble_step(DWORD port)
{
    struct rumble_port *r = &g_rumble[port];
    uint64_t now = g_be->now_ms();
    double dt = r->live ? (double)(now - r->step_ms) : 0.0;
    XBOX_VIBRATION v;
    WORD o[2];
    DWORD rc = ERROR_SUCCESS;

    rumble_params_read();
    for (int i = 0; i < 2; i++)
        o[i] = rumble_word(xbox_RumbleStep(&r->m[i], r->target[i], &g_rumble_params, dt));
    r->step_ms = now;
    r->live = 1;
    if (rumble_muted())
        return ERROR_SUCCESS;

    if (o[0] == r->out[0] && o[1] == r->out[1]) {
        if (!(o[0] | o[1]) || now - r->out_ms < RUMBLE_REFRESH_MS)
            return ERROR_SUCCESS;
    } else if (o[0] <= r->out[0] && o[1] <= r->out[1] && (o[0] | o[1])
               && now - r->out_ms < RUMBLE_SEND_MS) {
        return ERROR_SUCCESS;               /* still falling: not yet */
    }
    if (recomp_env(RENV_INPUT_DIAG) && (o[0] != r->out[0] || o[1] != r->out[1])) {
        fprintf(stderr, "  [RUMBLE] port %lu t=%llu ms title %u/%u -> host %u/%u%s\n",
                (unsigned long)port, (unsigned long long)now, r->target[0], r->target[1],
                o[0], o[1], g_connected[port] ? "" : " (no pad)");
        fflush(stderr);
    }
    v.wLeftMotorSpeed = o[0];
    v.wRightMotorSpeed = o[1];
    if (g_connected[port])
        rc = g_be->set_state(port, &v);
    r->out[0] = o[0];
    r->out[1] = o[1];
    r->out_ms = now;
    return rc;
}

/* Every motor off at the host, now. Lock held. */
static void rumble_silence(void)
{
    static const XBOX_VIBRATION off = { 0, 0 };

    if (!g_inited)
        return;
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        if (g_connected[p] || g_rumble[p].out[0] || g_rumble[p].out[1])
            g_be->set_state(p, &off);
        g_rumble[p].out[0] = g_rumble[p].out[1] = 0;
    }
}

void xbox_InputSetBlocked(int on)
{
    on = on != 0;
    RUMBLE_LOCK();
    if (g_blocked != on) {
        if (!on)
            g_latched = (1u << XBOX_MAX_CONTROLLERS) - 1u;
        if (on && !rumble_muted())
            rumble_silence();
        g_blocked = on;
    }
    RUMBLE_UNLOCK();
}

void xbox_InputSetBackground(int on)
{
    on = on != 0;
    RUMBLE_LOCK();
    if (g_background != on) {
        if (on && !rumble_muted())
            rumble_silence();
        g_background = on;
    }
    RUMBLE_UNLOCK();
}

void xbox_InputRumbleTick(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !g_inited || !g_rumble[dwPort].live)
        return;
    RUMBLE_LOCK();
    rumble_step(dwPort);
    RUMBLE_UNLOCK();
}

static void shutdown_atexit(void) { xbox_InputShutdown(); }

static DWORD probe(DWORD port, XBOX_INPUT_STATE *st)
{
    DWORD rc = g_be->get_state(port, st);
    g_last_probe[port] = g_be->now_ms();
    g_probed[port] = 1;
    g_connected[port] = (rc == ERROR_SUCCESS);
    return rc;
}

/* Whether a probe of a port is due. */
static int due(DWORD port)
{
    return !g_probed[port] || g_connected[port]
        || g_be->now_ms() - g_last_probe[port] >= XBOX_INPUT_PROBE_MS;
}

void xbox_InputInit(void)
{
    static int atexit_done;
    XBOX_INPUT_STATE st;

    if (g_inited)
        return;
    g_inited = 1;
    if (g_be->init)
        g_be->init();
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        probe(p, &st);
    /* A crash or an exit while the title has the motors on must not leave a
     * pad buzzing. */
    if (!atexit_done && g_be == &xbox_input_host_backend) {
        atexit_done = 1;
        atexit(shutdown_atexit);
    }
}

void xbox_InputSetBackend(const xbox_input_backend *b)
{
    g_be = b ? b : &xbox_input_host_backend;
    g_inited = 0;
    memset(g_connected, 0, sizeof g_connected);
    memset(g_last_probe, 0, sizeof g_last_probe);
    memset(g_probed, 0, sizeof g_probed);
    memset((void *)g_sources, 0, sizeof g_sources);
    RUMBLE_LOCK();
    memset(g_rumble, 0, sizeof g_rumble);
    g_rumble_params = (xbox_rumble_params)XBOX_RUMBLE_PARAMS_DEFAULT;
    g_rumble_params_read = 0;
    g_background = 0;
    RUMBLE_UNLOCK();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    DWORD rc;
    unsigned src;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_InputInit();

    rc = due(dwPort) ? probe(dwPort, pState) : ERROR_DEVICE_NOT_CONNECTED;
    src = rc == ERROR_SUCCESS ? XBOX_INPUT_SRC_PAD : 0;
    /* The title's pad poll is the rumble's clock. */
    xbox_InputRumbleTick(dwPort);

    /* The keyboard answers for port 0 with or without a pad there, and is
     * merged on top of one: a real pad keeps working, the keys only add. */
    if (dwPort == 0 && g_be->keyboard && g_be->keyboard(&kb)) {
        if (any_held(&kb.Gamepad) || kb.Gamepad.sThumbLX || kb.Gamepad.sThumbLY
                || kb.Gamepad.sThumbRX || kb.Gamepad.sThumbRY)
            src |= XBOX_INPUT_SRC_KEYS;
        if (rc != ERROR_SUCCESS) {
            g_sources[dwPort] = src;
            *pState = kb;
            return ERROR_SUCCESS;
        }
        XBOX_GAMEPAD *g = &pState->Gamepad;
        g->wButtons |= kb.Gamepad.wButtons;
        for (int i = 0; i < 8; i++)
            if (kb.Gamepad.bAnalogButtons[i] > g->bAnalogButtons[i])
                g->bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
        if (kb.Gamepad.sThumbLX) g->sThumbLX = kb.Gamepad.sThumbLX;
        if (kb.Gamepad.sThumbLY) g->sThumbLY = kb.Gamepad.sThumbLY;
        if (kb.Gamepad.sThumbRX) g->sThumbRX = kb.Gamepad.sThumbRX;
        if (kb.Gamepad.sThumbRY) g->sThumbRY = kb.Gamepad.sThumbRY;
        /* The title's input layer records edges, so an unchanged packet
         * number reads as the same state and a key press never happens. */
        pState->dwPacketNumber = kb.dwPacketNumber;
    }
    g_sources[dwPort] = src;
    if (rc == ERROR_SUCCESS && g_combo_guard
            && (pState->Gamepad.wButtons & XBOX_GAMEPAD_BACK))
        pState->Gamepad.wButtons &= (WORD)~XBOX_GAMEPAD_START;
    if (rc == ERROR_SUCCESS && (g_blocked || (g_latched & (1u << dwPort)))) {
        if (!g_blocked && !any_held(&pState->Gamepad))
            g_latched &= ~(1u << dwPort);
        else
            neutral(&pState->Gamepad);
    }
    return rc;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    DWORD rc;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_InputInit();
    RUMBLE_LOCK();
    /* The old speeds up to now, then the new ones from now: a fall starts
     * here, not at the last tick. Muted (the overlay, the background), the
     * speeds are kept for after. */
    rumble_step(dwPort);
    g_rumble[dwPort].target[0] = pVibration->wLeftMotorSpeed;
    g_rumble[dwPort].target[1] = pVibration->wRightMotorSpeed;
    rc = rumble_step(dwPort);
    RUMBLE_UNLOCK();
    return g_connected[dwPort] ? rc : ERROR_DEVICE_NOT_CONNECTED;
}

void xbox_InputShutdown(void)
{
    if (!g_inited)
        return;
    RUMBLE_LOCK();
    rumble_silence();
    for (DWORD p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        memset(g_rumble[p].target, 0, sizeof g_rumble[p].target);
        memset(g_rumble[p].m, 0, sizeof g_rumble[p].m);
    }
    RUMBLE_UNLOCK();
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    XBOX_INPUT_STATE st;

    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;
    xbox_InputInit();
    /* Connected ports are refreshed by the title's own reads; only a port
     * not read for a probe interval is asked here. */
    if (g_be->now_ms() - g_last_probe[dwPort] >= XBOX_INPUT_PROBE_MS)
        probe(dwPort, &st);
    return g_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!xbox_InputIsConnected(dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof *pCaps);
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return ERROR_SUCCESS;
}
