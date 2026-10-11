/**
 * xbox_input -- the seam between the public API (input_core.c) and a host
 * backend (xinput_device.c: XInput on Windows, SDL2 GameController
 * elsewhere). Internal to src/input and its tests.
 *
 * input_core.c owns the per-port connection cache and the probe backoff;
 * a backend only answers "what is on port N now". tests/input_map installs
 * a fake backend with xbox_InputSetBackend() and counts the calls.
 */
#ifndef XBOX_INPUT_BACKEND_H
#define XBOX_INPUT_BACKEND_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xbox_input_backend {
    /* Called once from xbox_InputInit(); may be NULL. */
    void  (*init)(void);
    /* Read port's pad. ERROR_SUCCESS with *out filled (already mapped
     * through input_map), or ERROR_DEVICE_NOT_CONNECTED. */
    DWORD (*get_state)(DWORD port, XBOX_INPUT_STATE *out);
    /* Set port's motors: host levels 0..65535, after input_core.c's rumble
     * curve. The backend applies no curve of its own. */
    DWORD (*set_state)(DWORD port, const XBOX_VIBRATION *vib);
    /* Port 0 keyboard, merged on top of the pad when RECOMP_KEYBOARD is on.
     * Returns 1 when the keyboard is enabled and *out was filled; NULL when
     * the backend has no keyboard. */
    int   (*keyboard)(XBOX_INPUT_STATE *out);
    /* Monotonic milliseconds. */
    uint64_t (*now_ms)(void);
} xbox_input_backend;

/* Rumble (input_core.c, rumble_curve.h), every backend alike. The strength
 * is the curve's exponent: 1.0 the title's speeds as the console played
 * them, 0 off, above 1 the low speeds lifted; RECOMP_INPUT_RUMBLE_GAIN until
 * this is called (the overlay's "Rumble strength"). The peak is the title's
 * own full speed: RECOMP_INPUT_RUMBLE_PEAK, 65535 unless set. The gate's two
 * speeds (off below, on from): 800 and 1200; on = 0 is no gate. Any thread. */
void xbox_InputSetRumbleStrength(double strength);
void xbox_InputSetRumblePeak(double peak);
void xbox_InputSetRumbleGate(unsigned off, unsigned on);

/* The window is not in front (or the app is in the background): the motors
 * stop and nothing is sent, the refresh included, until it is back. The
 * title's speeds are kept and play again then. Any thread. */
void xbox_InputSetBackground(int on);

/* The in-game overlay is open (input_core.c): the title reads neutral pads
 * and keys, and its rumble is silenced, until it closes. After it closes a
 * port stays neutral until its buttons have been let go once, so the press
 * that closed the overlay is not also the title's. Any thread. */
void xbox_InputSetBlocked(int on);

/* The overlay's hotkey is Back+Start: with the guard on, a title never sees
 * Start while Back is held, so pressing Back first (View, then Menu on a
 * Steam Deck) does not also pause the game. Start first still reaches it. */
void xbox_InputSetComboGuard(int on);
int  xbox_InputBlocked(void);

/* The host backend this build uses (xinput_device.c). */
extern const xbox_input_backend xbox_input_host_backend;

/* Swap the backend and reset the cache and the init flag (tests). NULL
 * restores the host backend. */
void xbox_InputSetBackend(const xbox_input_backend *b);

/* Disconnected ports are probed at most this often (XInput's guidance). */
#define XBOX_INPUT_PROBE_MS 1000

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_BACKEND_H */
