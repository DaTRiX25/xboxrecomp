/*
 * input_map test: see CMakeLists.txt.
 *
 * The expected mapping below is written out independently of the table in
 * src/input/input_map.c (it is the spec's: Black = RB, White = LB, faces 255,
 * triggers 0..255, sticks up/right positive), so a wrong table row fails here
 * rather than being compared with itself.
 */
#include "input_map.h"
#include "input_backend.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[128];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
    recomp_env_reload();   /* the runtime reads the environment once */
}

/* ── 1. every row, both helpers ────────────────────────────────────────── */

enum { F_BTN, F_ANALOG, F_STICK };
struct expect {
    const char *host;
    uint16_t xi_bit;            /* XInput button, or 0 for an axis */
    int      xi_axis;           /* XBOX_XI_SRC_* when xi_bit == 0 */
    int      sdl;               /* SDL button or axis */
    int      field;             /* F_* */
    int      index;             /* wButtons bit / bAnalogButtons idx / axis */
};

static const struct expect k_expect[] = {
    { "A", XBOX_XI_A, 0, XBOX_SDL_BUTTON_A, F_ANALOG, 0 },
    { "B", XBOX_XI_B, 0, XBOX_SDL_BUTTON_B, F_ANALOG, 1 },
    { "X", XBOX_XI_X, 0, XBOX_SDL_BUTTON_X, F_ANALOG, 2 },
    { "Y", XBOX_XI_Y, 0, XBOX_SDL_BUTTON_Y, F_ANALOG, 3 },
    { "Left shoulder (LB)",  0x0100, 0, 9,  F_ANALOG, 5 },   /* WHITE */
    { "Right shoulder (RB)", 0x0200, 0, 10, F_ANALOG, 4 },   /* BLACK */
    { "Start / Menu",      0x0010, 0, 6,  F_BTN, 0x0010 },
    { "Back / View",       0x0020, 0, 4,  F_BTN, 0x0020 },
    { "Left stick click",  0x0040, 0, 7,  F_BTN, 0x0040 },
    { "Right stick click", 0x0080, 0, 8,  F_BTN, 0x0080 },
    { "D-pad up",          0x0001, 0, 11, F_BTN, 0x0001 },
    { "D-pad down",        0x0002, 0, 12, F_BTN, 0x0002 },
    { "D-pad left",        0x0004, 0, 13, F_BTN, 0x0004 },
    { "D-pad right",       0x0008, 0, 14, F_BTN, 0x0008 },
    { "Left trigger",      0, XBOX_XI_SRC_LT, 4, F_ANALOG, 6 },
    { "Right trigger",     0, XBOX_XI_SRC_RT, 5, F_ANALOG, 7 },
    { "Left stick X",      0, XBOX_XI_SRC_LX, 0, F_STICK, 0 },
    { "Left stick Y",      0, XBOX_XI_SRC_LY, 1, F_STICK, 1 },
    { "Right stick X",     0, XBOX_XI_SRC_RX, 2, F_STICK, 2 },
    { "Right stick Y",     0, XBOX_XI_SRC_RY, 3, F_STICK, 3 },
};
#define N_EXPECT (sizeof k_expect / sizeof k_expect[0])

static SHORT stick(const XBOX_GAMEPAD *g, int a)
{
    return a == 0 ? g->sThumbLX : a == 1 ? g->sThumbLY
         : a == 2 ? g->sThumbRX : g->sThumbRY;
}

/* Everything in g is at rest except the one field e names, which is want.
 * At rest is 0, except the SDL helper's Y axes: SDL's 0 inverts to -1
 * (design D4 keeps (-1 - v) so -32768 and 32767 swap exactly). */
static void check_only(const char *who, const struct expect *e,
                       const XBOX_GAMEPAD *g, int want)
{
    XBOX_GAMEPAD z;
    memset(&z, 0, sizeof z);
    if (!strcmp(who, "sdl"))
        z.sThumbLY = z.sThumbRY = -1;
    switch (e->field) {
    case F_BTN:
        CHECK(g->wButtons == (WORD)e->index, "%s %s: wButtons 0x%04x, want 0x%04x",
              who, e->host, g->wButtons, e->index);
        z.wButtons = g->wButtons;
        break;
    case F_ANALOG:
        CHECK(g->bAnalogButtons[e->index] == want, "%s %s: bAnalogButtons[%d] = %d, want %d",
              who, e->host, e->index, g->bAnalogButtons[e->index], want);
        z.bAnalogButtons[e->index] = g->bAnalogButtons[e->index];
        break;
    default:
        CHECK(stick(g, e->index) == want, "%s %s: axis %d = %d, want %d",
              who, e->host, e->index, stick(g, e->index), want);
        if (e->index == 0) z.sThumbLX = g->sThumbLX;
        if (e->index == 1) z.sThumbLY = g->sThumbLY;
        if (e->index == 2) z.sThumbRX = g->sThumbRX;
        if (e->index == 3) z.sThumbRY = g->sThumbRY;
        break;
    }
    CHECK(!memcmp(&z, g, sizeof z), "%s %s: other guest fields touched", who, e->host);
}

static void test_rows(void)
{
    CHECK(xbox_input_map_count == N_EXPECT, "table has %u rows, want %u",
          (unsigned)xbox_input_map_count, (unsigned)N_EXPECT);
    for (size_t i = 0; i < N_EXPECT && i < xbox_input_map_count; i++) {
        const struct expect *e = &k_expect[i];
        XBOX_HOST_XINPUT_GAMEPAD xi;
        XBOX_HOST_SDL_GAMEPAD sd;
        XBOX_GAMEPAD g;

        CHECK(!strcmp(xbox_input_map[i].host, e->host), "row %u is '%s', want '%s'",
              (unsigned)i, xbox_input_map[i].host, e->host);

        memset(&xi, 0, sizeof xi);
        memset(&sd, 0, sizeof sd);
        if (e->field != F_STICK && e->xi_bit) {
            xi.wButtons = e->xi_bit;
            sd.button[e->sdl] = 1;
            xbox_InputMapXInput(&xi, &g);
            check_only("xinput", e, &g, 255);
            xbox_InputMapSdl(&sd, &g);
            check_only("sdl", e, &g, 255);
        } else if (e->field == F_ANALOG) {           /* triggers */
            static const struct { int xi, sdl, want; } t[] = {
                { 0, 0, 0 }, { 128, 16384, 128 }, { 255, 32767, 255 } };
            for (int k = 0; k < 3; k++) {
                memset(&xi, 0, sizeof xi);
                memset(&sd, 0, sizeof sd);
                if (e->xi_axis == XBOX_XI_SRC_LT) xi.bLeftTrigger = (uint8_t)t[k].xi;
                else xi.bRightTrigger = (uint8_t)t[k].xi;
                sd.axis[e->sdl] = (int16_t)t[k].sdl;
                xbox_InputMapXInput(&xi, &g);
                check_only("xinput", e, &g, t[k].want);
                xbox_InputMapSdl(&sd, &g);
                check_only("sdl", e, &g, t[k].want);
            }
        } else {                                      /* sticks */
            static const int vals[] = { 32767, -32768, 300, -200, 0 };
            int is_y = e->index == 1 || e->index == 3;
            for (int k = 0; k < 5; k++) {
                int v = vals[k];
                int16_t *xa = e->xi_axis == XBOX_XI_SRC_LX ? &xi.sThumbLX
                            : e->xi_axis == XBOX_XI_SRC_LY ? &xi.sThumbLY
                            : e->xi_axis == XBOX_XI_SRC_RX ? &xi.sThumbRX : &xi.sThumbRY;
                memset(&xi, 0, sizeof xi);
                memset(&sd, 0, sizeof sd);
                *xa = (int16_t)v;
                xbox_InputMapXInput(&xi, &g);
                check_only("xinput", e, &g, v);        /* XInput: Y already up */
                /* SDL Y points down: the host value for "guest v" is -1 - v. */
                sd.axis[e->sdl] = (int16_t)(is_y ? -1 - v : v);
                xbox_InputMapSdl(&sd, &g);
                check_only("sdl", e, &g, v);
            }
        }
    }

    /* The two that were swapped before: RB held -> BLACK 255, WHITE 0. */
    {
        XBOX_HOST_XINPUT_GAMEPAD xi = { XBOX_XI_RIGHT_SHOULDER, 0, 0, 0, 0, 0, 0 };
        XBOX_GAMEPAD g;
        xbox_InputMapXInput(&xi, &g);
        CHECK(g.bAnalogButtons[XBOX_BUTTON_BLACK] == 255
              && g.bAnalogButtons[XBOX_BUTTON_WHITE] == 0, "RB is not BLACK");
    }
    /* Full down on the right stick reaches the guest as -32768 (SDL +32767). */
    {
        XBOX_HOST_SDL_GAMEPAD sd;
        XBOX_GAMEPAD g;
        memset(&sd, 0, sizeof sd);
        sd.axis[XBOX_SDL_AXIS_RIGHTY] = 32767;
        xbox_InputMapSdl(&sd, &g);
        CHECK(g.sThumbRY == -32768, "SDL RY full down = %d, want -32768", g.sThumbRY);
        sd.axis[XBOX_SDL_AXIS_RIGHTY] = -32768;
        xbox_InputMapSdl(&sd, &g);
        CHECK(g.sThumbRY == 32767, "SDL RY full up = %d, want 32767", g.sThumbRY);
    }
}

/* ── 2. deadzone ───────────────────────────────────────────────────────── */

static void dz(int zone, int x, int y, int wx, int wy)
{
    SHORT sx = (SHORT)x, sy = (SHORT)y;
    xbox_InputApplyDeadzone(&sx, &sy, zone);
    CHECK(sx == wx && sy == wy, "deadzone %d (%d,%d) -> (%d,%d), want (%d,%d)",
          zone, x, y, sx, sy, wx, wy);
}

static void test_deadzone(void)
{
    dz(0, 300, -200, 300, -200);
    dz(0, -32768, 32767, -32768, 32767);
    dz(4000, 300, -200, 0, 0);
    dz(4000, 32767, 0, 32767, 0);
    dz(4000, 0, -32768, 0, -32768);
    dz(4000, -32768, 0, -32768, 0);
    dz(4000, 32767, 32767, 32767, 32767);      /* corners unchanged */
    dz(4000, 4000, 0, 0, 0);                   /* the edge reads 0 */
    {   /* rescaled: halfway between the edge and full reads half */
        SHORT x = (SHORT)(4000 + (32767 - 4000) / 2), y = 0;
        xbox_InputApplyDeadzone(&x, &y, 4000);
        CHECK(x >= 16382 && x <= 16385 && y == 0, "deadzone midpoint -> %d", x);
    }

    /* Through the helpers: unset -> identity; 4000 -> zeroed rest. */
    {
        XBOX_HOST_XINPUT_GAMEPAD xi = { 0, 0, 0, 300, -200, 300, -200 };
        XBOX_GAMEPAD g;
        xbox_InputSetDeadzone(0);
        xbox_InputMapXInput(&xi, &g);
        CHECK(g.sThumbLX == 300 && g.sThumbLY == -200, "no deadzone: (%d,%d)", g.sThumbLX, g.sThumbLY);
        xbox_InputSetDeadzone(4000);
        xbox_InputMapXInput(&xi, &g);
        CHECK(!g.sThumbLX && !g.sThumbLY && !g.sThumbRX && !g.sThumbRY,
              "deadzone 4000 through the helper: (%d,%d)", g.sThumbLX, g.sThumbLY);
    }

    /* RECOMP_PAD_DEADZONE is read once. */
    xbox_InputSetDeadzone(-1);
    set_env("RECOMP_PAD_DEADZONE", "4000");
    CHECK(xbox_InputDeadzone() == 4000, "env deadzone %d", xbox_InputDeadzone());
    set_env("RECOMP_PAD_DEADZONE", "100");
    CHECK(xbox_InputDeadzone() == 4000, "deadzone re-read from env");
    xbox_InputSetDeadzone(-1);
    set_env("RECOMP_PAD_DEADZONE", NULL);
    CHECK(xbox_InputDeadzone() == 0, "default deadzone %d", xbox_InputDeadzone());
    xbox_InputSetDeadzone(0);
}

/* ── 3. README ─────────────────────────────────────────────────────────── */

static void test_readme(const char *path)
{
    char line[512], want[512];
    FILE *f;
    size_t row = 0;
    int in = 0, found = 0;
    const char *out = getenv("INPUT_MAP_README_OUT");

    if (out) {
        FILE *o = fopen(out, "w");
        if (o) {
            fprintf(o, "%s\n", XBOX_INPUT_MAP_README_BEGIN);
            for (size_t i = 0; xbox_InputMapReadmeRow(i, want, sizeof want); i++)
                fprintf(o, "%s\n", want);
            fprintf(o, "%s\n", XBOX_INPUT_MAP_README_END);
            fclose(o);
        }
    }

    f = fopen(path, "r");
    CHECK(f != NULL, "cannot open %s", path);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!in) {
            if (!strcmp(line, XBOX_INPUT_MAP_README_BEGIN)) in = found = 1;
            continue;
        }
        if (!strcmp(line, XBOX_INPUT_MAP_README_END)) { in = 0; break; }
        if (!xbox_InputMapReadmeRow(row, want, sizeof want)) {
            CHECK(0, "README has an extra table line: %s", line);
            break;
        }
        CHECK(!strcmp(line, want), "README line %u:\n  have: %s\n  want: %s",
              (unsigned)row, line, want);
        row++;
    }
    fclose(f);
    CHECK(found, "README has no '%s' marker", XBOX_INPUT_MAP_README_BEGIN);
    CHECK(!in, "README table is not closed by '%s'", XBOX_INPUT_MAP_README_END);
    CHECK(!xbox_InputMapReadmeRow(row, want, sizeof want),
          "README table is missing line %u: %s", (unsigned)row, want);
}

/* ── 4. connection cache ───────────────────────────────────────────────── */

static uint64_t f_now;
static int f_plugged[4], f_get_calls[4], f_set_calls[4], f_init_calls, f_kb_on;
static XBOX_VIBRATION f_last_vib[4];

static void f_init(void) { f_init_calls++; }
static int f_hold_start = 1, f_hold_back;
static DWORD f_get(DWORD p, XBOX_INPUT_STATE *s)
{
    f_get_calls[p]++;
    if (!f_plugged[p]) return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, sizeof *s);
    s->Gamepad.wButtons = (f_hold_start ? XBOX_GAMEPAD_START : 0) | (f_hold_back ? XBOX_GAMEPAD_BACK : 0);
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v)
{
    f_set_calls[p]++;
    f_last_vib[p] = *v;
    return ERROR_SUCCESS;
}
static int f_kb(XBOX_INPUT_STATE *s)
{
    if (!f_kb_on) return 0;
    memset(s, 0, sizeof *s);
    s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
    s->dwPacketNumber = 77;
    return 1;
}
static uint64_t f_clock(void) { return f_now; }

static const xbox_input_backend k_fake = { f_init, f_get, f_set, f_kb, f_clock };

/* input_core.c's default; never called here. */
const xbox_input_backend xbox_input_host_backend = { NULL, f_get, f_set, NULL, f_clock };

static void test_cache(void)
{
    XBOX_INPUT_STATE st;
    XBOX_VIBRATION vib = { 2000, 3000 };  /* over the rumble gate */

    f_now = 5000;
    f_plugged[0] = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputInit();
    xbox_InputInit();                                 /* idempotent */
    CHECK(f_init_calls == 1, "backend init called %d times", f_init_calls);
    for (int p = 0; p < 4; p++)
        CHECK(f_get_calls[p] == 1, "init probed port %d %d times", p, f_get_calls[p]);
    CHECK(xbox_InputIsConnected(0) && !xbox_InputIsConnected(1), "init mask wrong");

    /* Connected: every call reaches the backend. */
    for (int i = 0; i < 5; i++)
        CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS, "port 0 read failed");
    CHECK(f_get_calls[0] == 6, "port 0 backend calls %d, want 6", f_get_calls[0]);
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_START, "port 0 state not passed on");

    /* Disconnected: no backend call until a second after the last probe. */
    for (int i = 0; i < 10; i++) {
        f_now += 99;
        CHECK(xbox_InputGetState(1, &st) == ERROR_DEVICE_NOT_CONNECTED, "port 1 connected?");
        CHECK(!xbox_InputIsConnected(1), "port 1 cache says connected");
    }
    CHECK(f_get_calls[1] == 1, "port 1 probed %d times inside the backoff", f_get_calls[1]);
    f_now += 10;                                      /* 1000 ms since init */
    xbox_InputGetState(1, &st);
    CHECK(f_get_calls[1] == 2, "port 1 not probed after 1 s (%d)", f_get_calls[1]);

    /* Plugged in: the mask follows within a second, through IsConnected. */
    f_plugged[1] = 1;
    f_now += 500;
    CHECK(!xbox_InputIsConnected(1), "port 1 probed before the backoff");
    CHECK(f_get_calls[1] == 2, "IsConnected probed inside the backoff");
    f_now += 500;
    CHECK(xbox_InputIsConnected(1), "port 1 not seen 1 s after plug-in");
    CHECK(f_get_calls[1] == 3, "port 1 probes %d, want 3", f_get_calls[1]);

    /* Unplugged: seen at the next read, then backed off again. */
    f_plugged[0] = 0;
    CHECK(xbox_InputGetState(0, &st) == ERROR_DEVICE_NOT_CONNECTED, "unplug not seen");
    CHECK(!xbox_InputIsConnected(0), "cache still connected after unplug");
    {
        int before = f_get_calls[0];
        f_now += 999;
        xbox_InputGetState(0, &st);
        CHECK(f_get_calls[0] == before, "probed an unplugged port inside the backoff");
    }

    /* Rumble: only to connected ports; shutdown stops it, twice is safe. */
    CHECK(xbox_InputSetState(2, &vib) == ERROR_DEVICE_NOT_CONNECTED && !f_set_calls[2],
          "rumble reached a disconnected port");
    CHECK(xbox_InputSetState(1, &vib) == ERROR_SUCCESS && f_set_calls[1] == 1, "rumble lost");
    xbox_InputShutdown();
    CHECK(f_set_calls[1] == 2 && !f_last_vib[1].wLeftMotorSpeed
          && !f_last_vib[1].wRightMotorSpeed, "shutdown did not stop port 1's motors");
    xbox_InputShutdown();

    /* Keyboard on port 0 answers with no pad there. */
    f_kb_on = 1;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 && st.dwPacketNumber == 77,
          "keyboard not merged on an empty port 0");
    f_kb_on = 0;

    xbox_InputSetBackend(NULL);
}

/* The overlay's block: neutral while open, and after it until the buttons
 * are let go; the motors silenced. */
static void test_block(void)
{
    XBOX_INPUT_STATE st;
    XBOX_VIBRATION vib = { 2000, 3000 };  /* over the rumble gate */

    f_now = 90000;
    f_plugged[0] = 1;
    f_kb_on = 0;
    f_hold_start = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputInit();
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && st.Gamepad.wButtons == XBOX_GAMEPAD_START,
          "block: no pass-through before");
    xbox_InputSetState(0, &vib);
    {
        int sets = f_set_calls[0];
        xbox_InputSetBlocked(1);
        CHECK(xbox_InputBlocked(), "block: not blocked");
        CHECK(f_set_calls[0] == sets + 1 && !f_last_vib[0].wLeftMotorSpeed,
              "block: motors not stopped");
        sets = f_set_calls[0];
        CHECK(xbox_InputSetState(0, &vib) == ERROR_SUCCESS && f_set_calls[0] == sets,
              "block: rumble reached the pad");
    }
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.wButtons
          && !st.Gamepad.bAnalogButtons[XBOX_BUTTON_A], "block: state not neutral");
    f_kb_on = 1;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.bAnalogButtons[XBOX_BUTTON_A],
          "block: keyboard got through");
    f_kb_on = 0;
    /* Closed with START still down: neutral until it is let go. */
    xbox_InputSetBlocked(0);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.wButtons,
          "block: the closing press reached the title");
    f_hold_start = 0;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && !st.Gamepad.wButtons, "block: released");
    f_hold_start = 1;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && st.Gamepad.wButtons == XBOX_GAMEPAD_START,
          "block: still neutral after the release");
    CHECK(xbox_InputSetState(0, &vib) == ERROR_SUCCESS && f_last_vib[0].wLeftMotorSpeed == 2000,
          "block: rumble stayed off after closing");
    xbox_InputSetBackend(NULL);
}

/* The rumble curve between the title and a backend (input_core.c): what the
 * fake pad is handed, with the clock in the test's hands. The curve's own
 * numbers are tests/input_rumble's. */
static void rumble_begin(const char *strength, const char *peak)
{
    set_env("RECOMP_INPUT_RUMBLE_GAIN", strength);
    set_env("RECOMP_INPUT_RUMBLE_PEAK", peak);
    memset(f_plugged, 0, sizeof f_plugged);
    f_plugged[0] = 1;
    f_kb_on = 0;
    xbox_InputSetBackend(&k_fake);      /* also: the variables are read again */
    xbox_InputInit();
    memset(f_set_calls, 0, sizeof f_set_calls);
    memset(f_last_vib, 0, sizeof f_last_vib);
}

#define FRAC(w) ((w) / 65535.0)
#define NEAR(w, f) ((w) / 65535.0 > (f) - 0.005 && (w) / 65535.0 < (f) + 0.005)

static void test_rumble(void)
{
    XBOX_VIBRATION v = { 0, 0 };
    int sets;

    /* No variables: the title's speeds as they are (strength 1, peak 65535),
     * but for the gate. */
    f_now = 200000;
    rumble_begin(NULL, NULL);
    v.wLeftMotorSpeed = 30000; v.wRightMotorSpeed = 658;
    CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS && f_set_calls[0] == 1
          && f_last_vib[0].wLeftMotorSpeed == 30000 && f_last_vib[0].wRightMotorSpeed == 0,
          "default: %u/%u", f_last_vib[0].wLeftMotorSpeed, f_last_vib[0].wRightMotorSpeed);

    /* The title's peak and a strength: the decision's table, at the pad. */
    f_now += 5000;
    rumble_begin("1.5", "52054");
    v.wLeftMotorSpeed = 52054; v.wRightMotorSpeed = 18349;
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535 && NEAR(f_last_vib[0].wRightMotorSpeed, 0.50),
          "1.5: %.3f/%.3f", FRAC(f_last_vib[0].wLeftMotorSpeed), FRAC(f_last_vib[0].wRightMotorSpeed));
    /* The live setter (the overlay) replaces the variable. */
    xbox_InputSetRumbleStrength(2.0);
    v.wRightMotorSpeed = 52054;                 /* up and down again: a fresh level */
    xbox_InputSetState(0, &v);
    f_now += 5000;
    xbox_InputRumbleTick(0);
    v.wLeftMotorSpeed = 5734; v.wRightMotorSpeed = 1791;
    xbox_InputSetState(0, &v);
    f_now += 5000;
    xbox_InputRumbleTick(0);
    CHECK(NEAR(f_last_vib[0].wLeftMotorSpeed, 0.33) && NEAR(f_last_vib[0].wRightMotorSpeed, 0.19),
          "2.0: %.3f/%.3f", FRAC(f_last_vib[0].wLeftMotorSpeed), FRAC(f_last_vib[0].wRightMotorSpeed));
    /* Strength 0: off, and the pad is told. */
    xbox_InputSetRumbleStrength(0.0);
    f_now += 5000;
    xbox_InputRumbleTick(0);
    CHECK(!f_last_vib[0].wLeftMotorSpeed && !f_last_vib[0].wRightMotorSpeed, "strength 0 not off");
    /* A spelling that is no number ("linear" was one once): the default. */
    f_now += 5000;
    rumble_begin("linear", NULL);
    v.wLeftMotorSpeed = 30000; v.wRightMotorSpeed = 0;
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 30000, "no number: %u", f_last_vib[0].wLeftMotorSpeed);

    /* The release, by the pad poll's clock: full, then the title's zero. */
    f_now += 5000;
    rumble_begin("1", "52054");
    v.wLeftMotorSpeed = 52054; v.wRightMotorSpeed = 0;
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "release: no rise");
    f_now += 1000;
    xbox_InputRumbleTick(0);                    /* held: as it was */
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "a held level moved");
    v.wLeftMotorSpeed = 0;
    sets = f_set_calls[0];
    xbox_InputSetState(0, &v);
    CHECK(f_set_calls[0] == sets && f_last_vib[0].wLeftMotorSpeed == 65535,
          "the zero cut the motor at once");
    for (int i = 0; i < 90; i++) {              /* a 1 ms poll */
        XBOX_INPUT_STATE st;
        f_now += 1;
        if (i & 1)
            xbox_InputRumbleTick(0);
        else
            xbox_InputGetState(0, &st);         /* the title's own poll ticks too */
    }
    CHECK(f_last_vib[0].wLeftMotorSpeed / 65535.0 > 0.36
          && f_last_vib[0].wLeftMotorSpeed / 65535.0 < 0.44,
          "90 ms after the zero: %.3f", FRAC(f_last_vib[0].wLeftMotorSpeed));
    /* A falling level is not written to the pad at every poll. */
    CHECK(f_set_calls[0] - sets >= 4 && f_set_calls[0] - sets <= 7,
          "%d writes in 90 ms of falling", f_set_calls[0] - sets);
    /* A rise in the fall goes out at once. */
    v.wLeftMotorSpeed = 52054;
    xbox_InputSetState(0, &v);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "rise in a fall held back");
    /* And the fall ends in a zero that is sent, after which nothing is. */
    v.wLeftMotorSpeed = 0;
    xbox_InputSetState(0, &v);
    for (int i = 0; i < 600; i++) {
        f_now += 1;
        xbox_InputRumbleTick(0);
    }
    CHECK(!f_last_vib[0].wLeftMotorSpeed && !f_last_vib[0].wRightMotorSpeed, "no final zero");
    sets = f_set_calls[0];
    for (int i = 0; i < 1000; i++) {
        f_now += 1;
        xbox_InputRumbleTick(0);
    }
    CHECK(f_set_calls[0] == sets, "%d writes to a stopped pad", f_set_calls[0] - sets);

    /* A held rumble is given again every 150 ms (SDL's rumble runs out). */
    v.wLeftMotorSpeed = 20000;
    xbox_InputSetState(0, &v);
    sets = f_set_calls[0];
    for (int i = 0; i < 600; i++) {
        f_now += 1;
        xbox_InputRumbleTick(0);
    }
    CHECK(f_set_calls[0] - sets == 4, "%d refreshes in 600 ms", f_set_calls[0] - sets);

    /* The window out of focus: a zero, then nothing, the refresh included;
     * back in front, what the title last asked for plays again. */
    sets = f_set_calls[0];
    xbox_InputSetBackground(1);
    CHECK(f_set_calls[0] == sets + 1 && !f_last_vib[0].wLeftMotorSpeed, "background: no stop");
    sets = f_set_calls[0];
    for (int i = 0; i < 600; i++) {
        f_now += 1;
        xbox_InputRumbleTick(0);
    }
    v.wLeftMotorSpeed = 52054;
    CHECK(xbox_InputSetState(0, &v) == ERROR_SUCCESS && f_set_calls[0] == sets,
          "background: %d writes", f_set_calls[0] - sets);
    xbox_InputSetBackground(1);                 /* twice is once */
    CHECK(f_set_calls[0] == sets, "background twice");
    xbox_InputSetBackground(0);
    f_now += 1;
    xbox_InputRumbleTick(0);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "foreground: %u", f_last_vib[0].wLeftMotorSpeed);
    /* The overlay over a window out of focus: silent until both are over. */
    xbox_InputSetBlocked(1);
    xbox_InputSetBackground(1);
    xbox_InputSetBlocked(0);
    {
        /* (The block's latch on the buttons, let go: not this test's.) */
        XBOX_INPUT_STATE st;
        f_hold_start = 0;
        xbox_InputGetState(0, &st);
        f_hold_start = 1;
    }
    f_now += 200;
    xbox_InputRumbleTick(0);
    CHECK(!f_last_vib[0].wLeftMotorSpeed, "unblocked in the background: rumble");
    xbox_InputSetBackground(0);
    f_now += 1;
    xbox_InputRumbleTick(0);
    CHECK(f_last_vib[0].wLeftMotorSpeed == 65535, "both over: no rumble");

    /* No pad: the title's speeds are kept, and a pad that appears gets them. */
    f_now += 5000;
    rumble_begin("1", NULL);
    v.wLeftMotorSpeed = 40000; v.wRightMotorSpeed = 0;
    CHECK(xbox_InputSetState(1, &v) == ERROR_DEVICE_NOT_CONNECTED && !f_set_calls[1],
          "rumble reached an empty port");
    f_plugged[1] = 1;
    f_now += 1000;
    CHECK(xbox_InputIsConnected(1), "port 1 not seen");
    xbox_InputRumbleTick(1);
    CHECK(f_set_calls[1] == 1 && f_last_vib[1].wLeftMotorSpeed == 40000,
          "a pad that appeared: %d writes, %u", f_set_calls[1], f_last_vib[1].wLeftMotorSpeed);

    /* Shutdown: every motor off and forgotten. */
    xbox_InputShutdown();
    CHECK(!f_last_vib[1].wLeftMotorSpeed, "shutdown left port 1 on");
    sets = f_set_calls[1];
    f_now += 1000;
    xbox_InputRumbleTick(1);
    CHECK(f_set_calls[1] == sets, "rumble after shutdown");

    set_env("RECOMP_INPUT_RUMBLE_GAIN", NULL);
    set_env("RECOMP_INPUT_RUMBLE_PEAK", NULL);
    xbox_InputSetBackend(NULL);
}

/* The overlay's combo guard: Start is hidden while Back is held. */
static void test_combo_guard(void)
{
    XBOX_INPUT_STATE st;

    f_now = 95000;
    f_plugged[0] = 1;
    f_kb_on = 0;
    f_hold_start = 1;
    xbox_InputSetBackend(&k_fake);
    xbox_InputInit();
    xbox_InputSetComboGuard(1);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && st.Gamepad.wButtons == XBOX_GAMEPAD_START,
          "guard: Start alone was hidden");
    f_hold_back = 1;
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS && st.Gamepad.wButtons == XBOX_GAMEPAD_BACK,
          "guard: Start reached the title with Back held (%04X)", st.Gamepad.wButtons);
    xbox_InputSetComboGuard(0);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS
          && st.Gamepad.wButtons == (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START),
          "guard off: the buttons were changed");
    f_hold_back = 0;
    xbox_InputSetBackend(NULL);
}

static void test_trigger_deadzone(void)
{
    XBOX_HOST_XINPUT_GAMEPAD in;
    XBOX_GAMEPAD out;

    memset(&in, 0, sizeof in);
    in.bLeftTrigger = 20;
    in.bRightTrigger = 255;
    xbox_InputSetTriggerDeadzone(0);
    xbox_InputMapXInput(&in, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 20, "trigger dz 0 is not the identity");
    xbox_InputSetTriggerDeadzone(30);
    xbox_InputMapXInput(&in, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 0, "trigger under the deadzone reads");
    CHECK(out.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "full pull not full");
    in.bLeftTrigger = 142;                              /* (142 - 30) * 255 / 225 */
    xbox_InputMapXInput(&in, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 126, "trigger rescale %d",
          out.bAnalogButtons[XBOX_BUTTON_LTRIGGER]);
    xbox_InputSetTriggerDeadzone(0);
}

/* The Nintendo face layout: A and B, X and Y change places, nothing else. */
static void test_face_layout(void)
{
    XBOX_HOST_XINPUT_GAMEPAD xi;
    XBOX_HOST_SDL_GAMEPAD sdl;
    XBOX_GAMEPAD out;

    CHECK(!xbox_InputFaceLayoutNintendo(), "the default is the Xbox layout");
    memset(&xi, 0, sizeof xi);
    xi.wButtons = XBOX_XI_A | XBOX_XI_X | XBOX_XI_START | XBOX_XI_DPAD_UP;
    xbox_InputMirrorFaceXInput(&xi);
    CHECK(xi.wButtons == (XBOX_XI_B | XBOX_XI_Y | XBOX_XI_START | XBOX_XI_DPAD_UP),
          "xinput mirror %04X", xi.wButtons);
    xbox_InputMirrorFaceXInput(&xi);
    CHECK(xi.wButtons == (XBOX_XI_A | XBOX_XI_X | XBOX_XI_START | XBOX_XI_DPAD_UP),
          "xinput mirror twice %04X", xi.wButtons);
    memset(&sdl, 0, sizeof sdl);
    sdl.button[XBOX_SDL_BUTTON_B] = 1;          /* the button printed A on such a pad */
    sdl.button[XBOX_SDL_BUTTON_Y] = 1;
    xbox_InputMirrorFaceSdl(&sdl);
    CHECK(sdl.button[XBOX_SDL_BUTTON_A] && !sdl.button[XBOX_SDL_BUTTON_B]
          && sdl.button[XBOX_SDL_BUTTON_X] && !sdl.button[XBOX_SDL_BUTTON_Y], "sdl mirror");
    xbox_InputMapSdl(&sdl, &out);
    CHECK(out.bAnalogButtons[XBOX_BUTTON_A] == 255 && out.bAnalogButtons[XBOX_BUTTON_B] == 0
          && out.bAnalogButtons[XBOX_BUTTON_X] == 255 && out.bAnalogButtons[XBOX_BUTTON_Y] == 0,
          "the title's A and X after the mirror");
    xbox_InputSetFaceLayoutNintendo(1);
    CHECK(xbox_InputFaceLayoutNintendo(), "the setting is kept");
    xbox_InputSetFaceLayoutNintendo(0);
}

/* The driving-controls choice is kept for the title's host code, and that
 * code can tell a controller's state from the keyboard's (BO-106). */
static void test_control_scheme(void)
{
    XBOX_INPUT_STATE st;

    CHECK(xbox_InputControlScheme() == 0, "the default is the title's own layout");
    CHECK(!xbox_InputControlSchemeOffered(), "no title has offered a second layout");
    xbox_InputOfferControlScheme(1);
    xbox_InputSetControlScheme(1);
    CHECK(xbox_InputControlSchemeOffered() && xbox_InputControlScheme() == 1, "the choice is kept");
    xbox_InputSetControlScheme(-3);
    CHECK(xbox_InputControlScheme() == 0, "a negative scheme is the default");
    xbox_InputOfferControlScheme(0);

    xbox_InputSetBackend(&k_fake);
    memset(f_plugged, 0, sizeof f_plugged);
    f_now = 5000;
    f_hold_start = 0;
    f_kb_on = 0;
    CHECK(xbox_InputSources(0) == 0, "sources before any read");
    f_plugged[0] = f_plugged[1] = 1;
    xbox_InputGetState(0, &st);
    CHECK(xbox_InputSources(0) == XBOX_INPUT_SRC_PAD, "a controller alone: %u", xbox_InputSources(0));
    f_kb_on = 1;                                /* the fake keyboard holds A */
    xbox_InputGetState(0, &st);
    CHECK(xbox_InputSources(0) == (XBOX_INPUT_SRC_PAD | XBOX_INPUT_SRC_KEYS),
          "controller and a held key: %u", xbox_InputSources(0));
    xbox_InputGetState(1, &st);
    CHECK(xbox_InputSources(1) == XBOX_INPUT_SRC_PAD, "the keyboard is port 0's only");
    f_plugged[0] = 0;
    f_now += 2 * XBOX_INPUT_PROBE_MS;
    xbox_InputGetState(0, &st);
    CHECK(xbox_InputSources(0) == XBOX_INPUT_SRC_KEYS, "keys alone: %u", xbox_InputSources(0));
    f_kb_on = 0;
    xbox_InputGetState(0, &st);
    CHECK(xbox_InputSources(0) == 0, "nothing on the port: %u", xbox_InputSources(0));
    CHECK(xbox_InputSources(9) == 0, "a port that does not exist");
    f_hold_start = 1;
    xbox_InputSetBackend(NULL);
}

int main(int argc, char **argv)
{
    xbox_InputSetDeadzone(0);
    test_rows();
    test_deadzone();
    test_readme(argc > 1 ? argv[1] : "input_README.md");
    test_cache();
    test_block();
    test_rumble();
    test_combo_guard();
    test_trigger_deadzone();
    test_face_layout();
    test_control_scheme();
    printf("input_map: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
