/* pad_glyphs: the family of a pad, the set that is drawn, the listener. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pad_glyphs.h"

static int s_checks, s_fail;
#define CHECK(x) do { s_checks++; if (!(x)) { s_fail++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* input_map.c's, stubbed. */
static int st_face;
int xbox_InputFaceLayoutNintendo(void) { return st_face; }

static int st_calls, st_last = -1;
static void listener(int set) { st_calls++; st_last = set; }

#define X  XBOX_PAD_FAMILY_XBOX
#define XM XBOX_PAD_FAMILY_XBOX_MODERN
#define PS XBOX_PAD_FAMILY_PLAYSTATION
#define NI XBOX_PAD_FAMILY_NINTENDO
#define DK XBOX_PAD_FAMILY_STEAM
#define P5 XBOX_PAD_FAMILY_PLAYSTATION5
#define NO XBOX_PAD_FAMILY_NONE
#define KB XBOX_PAD_FAMILY_KEYBOARD

int main(void)
{
    /* ── family ── */
    CHECK(xbox_PadFamilyFromIds(XM, 0x045E, 0x0B13, "Xbox Series X Controller", 0) == XM);
    CHECK(xbox_PadFamilyFromIds(X, 0x1234, 0x5678, "a pad nothing is known about", 0) == X);
    CHECK(xbox_PadFamilyFromIds(X, 0, 0, NULL, 0) == X);
    CHECK(xbox_PadFamilyFromIds(P5, 0x054C, 0x0CE6, "PS5 Controller", 0) == P5);        /* SDL type PS5 */
    CHECK(xbox_PadFamilyFromIds(PS, 0x054C, 0x0CE6, "DualSense", 0) == P5);           /* an SDL that says PS4 */
    CHECK(xbox_PadFamilyFromIds(PS, 0x054C, 0x05C4, "Wireless Controller", 0) == PS); /* DualShock 4 stays */
    CHECK(xbox_PadFamilyFromIds(PS, 0x054C, 0x0268, "PS3 Controller", 0) == PS);
    CHECK(xbox_PadFamilyFromIds(X, 0x054C, 0x0268, "Sony pad SDL has no type for", 0) == PS);
    CHECK(xbox_PadFamilyFromIds(NI, 0x057E, 0x2009, "Nintendo Switch Pro Controller", 0) == NI);
    CHECK(xbox_PadFamilyFromIds(X, 0x057E, 0x2017, "SNES Controller", 0) == NI);
    /* A third-party pad SDL types as PlayStation or Nintendo keeps the type. */
    CHECK(xbox_PadFamilyFromIds(PS, 0x0F0D, 0x00EE, "HORIPAD mini4", 0) == PS);
    CHECK(xbox_PadFamilyFromIds(NI, 0x20D6, 0xA711, "PowerA Switch pad", 0) == NI);
    /* Valve: the Deck's own controls, the Steam Controllers old and new
     * (SDL 3.4's controller_list.h), by id; SDL2 has no type for them. */
    {
        static const unsigned valve[] = { 0x1101, 0x1102, 0x1105, 0x1106, 0x1142, 0x1201, 0x1202,
                                          0x1205, 0x1302, 0x1303, 0x1304, 0x1305 };
        unsigned i;
        for (i = 0; i < sizeof valve / sizeof valve[0]; i++) {
            CHECK(xbox_PadFamilyFromIds(X, 0x28DE, valve[i], "Steam Controller", 0) == DK);
            CHECK(xbox_PadFamilyFromIds(XM, 0x28DE, valve[i], NULL, 1) == DK);
        }
    }
    CHECK(xbox_PadFamilyFromIds(X, 0, 0, "Microsoft X-Box 360 pad 0 (Steam Deck)", 0) == DK);
    /* Steam Input's virtual pad is any pad at all: Steam's only on a Deck. */
    CHECK(xbox_PadFamilyFromIds(X, 0x28DE, 0x11FF, "Steam Virtual Gamepad", 1) == DK);
    CHECK(xbox_PadFamilyFromIds(XM, 0x28DE, 0x11FF, "Steam Virtual Gamepad", 1) == DK);
    CHECK(xbox_PadFamilyFromIds(X, 0x28DE, 0x11FF, "Steam Virtual Gamepad", 0) == XM);
    /* Windows, as SDL reports the common pads there (type first, then ids) */
    CHECK(xbox_PadFamilyFromIds(XM, 0x045E, 0x02FF, "Xbox One Controller", 0) == XM);       /* XInput */
    CHECK(xbox_PadFamilyFromIds(X, 0x045E, 0x028E, "Xbox 360 Controller", 0) == XM);        /* untyped 360 */
    CHECK(xbox_PadFamilyFromIds(X, 0x045E, 0x0719, "Xbox 360 Wireless Receiver", 0) == XM);
    CHECK(xbox_PadFamilyFromIds(X, 0, 0, "XInput Controller #1", 0) == XM);
    CHECK(xbox_PadFamilyFromIds(X, 0x24C6, 0x5300, "PowerA Mini Pro Ex (Xbox 360)", 0) == XM);
    CHECK(xbox_PadFamilyFromIds(X, 0x045E, 0x0289, "Original Xbox Controller S", 0) == X);   /* original stays */
    CHECK(xbox_PadFamilyFromIds(X, 0x045E, 0x0202, "Xbox Duke", 0) == X);
    CHECK(xbox_PadFamilyFromIds(X, 0x054C, 0x05C4, "Wireless Controller", 0) == PS);         /* DualShock 4, untyped */
    CHECK(xbox_PadFamilyFromIds(X, 0x054C, 0x09CC, "Wireless Controller", 0) == PS);         /* DualShock 4 v2 */
    CHECK(xbox_PadFamilyFromIds(X, 0x054C, 0x0CE6, "DualSense Wireless Controller", 0) == P5);
    CHECK(xbox_PadFamilyFromIds(X, 0x054C, 0x0DF2, "DualSense Edge", 0) == P5);
    CHECK(xbox_PadFamilyFromIds(P5, 0x054C, 0x0DF2, "DualSense Edge", 0) == P5);
    CHECK(xbox_PadFamilyFromIds(X, 0x057E, 0x2009, "Pro Controller", 0) == NI);
    CHECK(xbox_PadFamilyFromIds(XM, 0x28DE, 0x11FF, "Xbox 360 Controller", 0) == XM);        /* Steam Input, XInput */
    CHECK(xbox_PadFamilyFromIds(PS, 0x28DE, 0x11FF, "Steam Virtual Gamepad", 0) == XM);
    CHECK(xbox_PadFamilyFromIds(X, 0x28DE, 0x1304, "Steam Controller", 0) == DK);
    CHECK(xbox_PadFamilyFromIds(XM, 0x28DE, 0x11FF, "Steam Virtual Gamepad", 0) == XM);
    /* SteamDeck=1 alone does not make a wired Xbox pad Valve's (no Steam Input). */
    CHECK(xbox_PadFamilyFromIds(XM, 0x045E, 0x028E, "Xbox 360 Controller", 1) == XM);

    /* ── the set ── */
    /* A fixed choice does not look at the pad. */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_XBOX, PS, 0, 1) == XBOX_GLYPH_SET_XBOX);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_PLAYSTATION, X, 0, 0) == XBOX_GLYPH_SET_PLAYSTATION);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_STEAM, NI, 1, 0) == XBOX_GLYPH_SET_STEAM);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_XBOX_MODERN, PS, 0, 1) == XBOX_GLYPH_SET_XBOX_MODERN);
    /* auto: the family; no pad yet is the title's own; a pad nothing is
     * known about (an unknown family: a handheld's built-in pad) is the
     * positional set, by position or by letter as the face layout says; an
     * Xbox 360 / One / Series pad gets LB RB LT RT. */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, NO, 0, 0) == XBOX_GLYPH_SET_XBOX);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, NO, 0, 1) == XBOX_GLYPH_SET_NINTENDO_LABEL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, X, 0, 0) == XBOX_GLYPH_SET_POSITIONAL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, X, 0, 1) == XBOX_GLYPH_SET_POSITIONAL_SWAPPED);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, XM, 0, 0) == XBOX_GLYPH_SET_XBOX_MODERN);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, XM, 0, 1) == XBOX_GLYPH_SET_NINTENDO_LABEL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, PS, 0, 0) == XBOX_GLYPH_SET_PLAYSTATION);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, DK, 0, 0) == XBOX_GLYPH_SET_STEAM);
    /* The face layout does not touch PlayStation or Deck prompts. */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, PS, 0, 1) == XBOX_GLYPH_SET_PLAYSTATION);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, DK, 0, 1) == XBOX_GLYPH_SET_STEAM);
    /* Nintendo: a prompt shows what is printed on the button to press. The
     * four cases of a pad printed the Nintendo way (read by letter or by
     * position) x (face layout xbox or nintendo): the title's A is on the
     * button printed A (LABEL) when exactly one of the two holds, on the one
     * printed B (POSITION) otherwise. */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, NI, 1, 0) == XBOX_GLYPH_SET_NINTENDO_LABEL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, NI, 0, 0) == XBOX_GLYPH_SET_NINTENDO_POSITION);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, NI, 0, 1) == XBOX_GLYPH_SET_NINTENDO_LABEL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, NI, 1, 1) == XBOX_GLYPH_SET_NINTENDO_POSITION);
    /* PlayStation 5: the DualSense by auto; by hand on any pad. Face layout
     * does not matter (no letters). */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, P5, 0, 0) == XBOX_GLYPH_SET_PLAYSTATION5);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, P5, 0, 1) == XBOX_GLYPH_SET_PLAYSTATION5);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_PLAYSTATION5, X, 0, 0) == XBOX_GLYPH_SET_PLAYSTATION5);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_PLAYSTATION5, NI, 1, 1) == XBOX_GLYPH_SET_PLAYSTATION5);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_PLAYSTATION, P5, 0, 0) == XBOX_GLYPH_SET_PLAYSTATION);   /* the coloured one by hand */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, PS, 0, 0) == XBOX_GLYPH_SET_PLAYSTATION);
    /* Chosen by hand on a pad the host reads by position (a handheld's own). */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_NINTENDO, X, 0, 0) == XBOX_GLYPH_SET_NINTENDO_POSITION);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_NINTENDO, X, 0, 1) == XBOX_GLYPH_SET_NINTENDO_LABEL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_NINTENDO, X, 1, 0) == XBOX_GLYPH_SET_NINTENDO_POSITION);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_NINTENDO, NI, 1, 0) == XBOX_GLYPH_SET_NINTENDO_LABEL);
    /* auto, unknown pad, face layout nintendo: the player said how it is printed. */
    /* Positional: the place of the button to press, for any pad. Same four
     * cases as Nintendo's: the title's A is the bottom button (POSITIONAL) or
     * the right one (SWAPPED) when the pad is read by letter xor the face
     * layout is nintendo. */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_POSITIONAL, NI, 1, 0) == XBOX_GLYPH_SET_POSITIONAL_SWAPPED);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_POSITIONAL, NI, 0, 0) == XBOX_GLYPH_SET_POSITIONAL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_POSITIONAL, NI, 0, 1) == XBOX_GLYPH_SET_POSITIONAL_SWAPPED);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_POSITIONAL, NI, 1, 1) == XBOX_GLYPH_SET_POSITIONAL);
    /* Any other pad is read by position (label_mapped is only a Nintendo pad's). */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_POSITIONAL, X, 0, 0) == XBOX_GLYPH_SET_POSITIONAL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_POSITIONAL, XM, 0, 1) == XBOX_GLYPH_SET_POSITIONAL_SWAPPED);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_POSITIONAL, PS, 1, 0) == XBOX_GLYPH_SET_POSITIONAL);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_POSITIONAL, DK, 0, 1) == XBOX_GLYPH_SET_POSITIONAL_SWAPPED);
    /* auto picks it for a pad of unknown family only, never for a known
     * one or for no pad. */
    {
        int f, l, n;
        for (f = 0; f < XBOX_PAD_FAMILY_COUNT; f++)
            for (l = 0; l < 2; l++)
                for (n = 0; n < 2; n++) {
                    int s = xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, (xbox_pad_family)f, l, n);
                    int pos = s == XBOX_GLYPH_SET_POSITIONAL || s == XBOX_GLYPH_SET_POSITIONAL_SWAPPED;
                    CHECK(pos == (f == (int)X));
                }
    }

    /* The keyboard as the device in use: its own key caps, whatever the
     * face layout; a fixed choice still wins. */
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, KB, 0, 0) == XBOX_GLYPH_SET_KEYBOARD);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, KB, 1, 1) == XBOX_GLYPH_SET_KEYBOARD);
    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_PLAYSTATION, KB, 0, 0) == XBOX_GLYPH_SET_PLAYSTATION);
    /* The key caps by hand: any pad, either face layout, no pad at all. */
    {
        int f, l, n;
        for (f = 0; f < XBOX_PAD_FAMILY_COUNT; f++)
            for (l = 0; l < 2; l++)
                for (n = 0; n < 2; n++) {
                    CHECK(xbox_GlyphSetResolve(XBOX_GLYPHS_KEYBOARD, (xbox_pad_family)f, l, n) == XBOX_GLYPH_SET_KEYBOARD);
                    /* and auto picks them for the keyboard and for nothing else */
                    CHECK((xbox_GlyphSetResolve(XBOX_GLYPHS_AUTO, (xbox_pad_family)f, l, n) == XBOX_GLYPH_SET_KEYBOARD) == (f == (int)KB));
                }
    }

    /* ── names ── */
    CHECK(xbox_GlyphModeFromName("auto") == XBOX_GLYPHS_AUTO);
    CHECK(xbox_GlyphModeFromName("PlayStation") == XBOX_GLYPHS_PLAYSTATION);
    CHECK(xbox_GlyphModeFromName("steam") == XBOX_GLYPHS_STEAM && xbox_GlyphModeFromName("Deck") == XBOX_GLYPHS_STEAM);
    CHECK(xbox_GlyphModeFromName("xbox") == XBOX_GLYPHS_XBOX && xbox_GlyphModeFromName("xbox_modern") == XBOX_GLYPHS_XBOX_MODERN);
    CHECK(xbox_GlyphModeFromName("playstation5") == XBOX_GLYPHS_PLAYSTATION5 && xbox_GlyphModeFromName("playstation") == XBOX_GLYPHS_PLAYSTATION);
    CHECK(xbox_GlyphModeFromName("positional") == XBOX_GLYPHS_POSITIONAL && xbox_GlyphModeFromName("Positional") == XBOX_GLYPHS_POSITIONAL);
    CHECK(!strcmp(xbox_GlyphModeName(XBOX_GLYPHS_POSITIONAL), "positional"));
    CHECK(xbox_GlyphModeFromName("keyboard") == XBOX_GLYPHS_KEYBOARD && xbox_GlyphModeFromName("Keyboard") == XBOX_GLYPHS_KEYBOARD);
    CHECK(!strcmp(xbox_GlyphModeName(XBOX_GLYPHS_KEYBOARD), "keyboard"));
    CHECK(XBOX_GLYPHS_KEYBOARD == XBOX_GLYPHS_MODE_COUNT - 1 && XBOX_GLYPH_SET_KEYBOARD == XBOX_GLYPH_SET_COUNT - 1);
    {   /* every mode has its own name, in the order of the setting's choices */
        int i;
        for (i = 0; i < XBOX_GLYPHS_MODE_COUNT; i++)
            CHECK(xbox_GlyphModeFromName(xbox_GlyphModeName((xbox_glyph_mode)i)) == i);
    }
    CHECK(xbox_GlyphModeFromName("decks") == -1);
    CHECK(xbox_GlyphModeFromName("") == -1 && xbox_GlyphModeFromName(NULL) == -1);

    /* ── state and the listener ── */
#if !defined(_WIN32)
    unsetenv("RECOMP_INPUT_GLYPHS");
#endif
    CHECK(xbox_InputGlyphMode() == XBOX_GLYPHS_AUTO);
    xbox_InputSetGlyphListener(listener);
    CHECK(st_calls == 1 && st_last == XBOX_GLYPH_SET_XBOX);        /* told at once */
    xbox_InputSetActivePad(PS, 0);
    CHECK(st_calls == 2 && st_last == XBOX_GLYPH_SET_PLAYSTATION);
    xbox_InputSetActivePad(PS, 0);                                 /* the same pad again */
    CHECK(st_calls == 2);
    xbox_InputSetActivePad(X, 0);                                  /* a pad of unknown family is picked up */
    CHECK(st_calls == 3 && st_last == XBOX_GLYPH_SET_POSITIONAL);
    CHECK(xbox_InputActivePadFamily() == X);
    xbox_InputSetGlyphMode(XBOX_GLYPHS_NINTENDO);                  /* by hand */
    CHECK(st_calls == 4 && st_last == XBOX_GLYPH_SET_NINTENDO_POSITION);
    st_face = 1;
    xbox_InputGlyphsRefresh();                                     /* the face layout changed */
    CHECK(st_calls == 5 && st_last == XBOX_GLYPH_SET_NINTENDO_LABEL);
    xbox_InputSetActivePad(DK, 0);                                 /* a fixed choice ignores the pad */
    CHECK(st_calls == 5);
    st_face = 0;
    xbox_InputSetGlyphMode(XBOX_GLYPHS_AUTO);
    CHECK(st_calls == 6 && st_last == XBOX_GLYPH_SET_STEAM);
    CHECK(xbox_InputGlyphSet() == XBOX_GLYPH_SET_STEAM && xbox_InputActivePadFamily() == DK);
    xbox_InputSetGlyphMode(XBOX_GLYPHS_XBOX);
    CHECK(st_calls == 7 && st_last == XBOX_GLYPH_SET_XBOX);
    xbox_InputSetGlyphMode(XBOX_GLYPHS_POSITIONAL);
    CHECK(st_calls == 8 && st_last == XBOX_GLYPH_SET_POSITIONAL);
    st_face = 1;
    xbox_InputGlyphsRefresh();                                     /* the face layout changed */
    CHECK(st_calls == 9 && st_last == XBOX_GLYPH_SET_POSITIONAL_SWAPPED);
    xbox_InputSetActivePad(PS, 0);                                 /* a fixed choice ignores the pad */
    CHECK(st_calls == 9);
    st_face = 0;
    xbox_InputGlyphsRefresh();
    CHECK(st_calls == 10 && st_last == XBOX_GLYPH_SET_POSITIONAL);

    /* A key after a pad, and a pad button after the keyboard. */
    xbox_InputSetGlyphMode(XBOX_GLYPHS_AUTO);
    xbox_InputSetActivePad(PS, 0);
    CHECK(xbox_InputGlyphSet() == XBOX_GLYPH_SET_PLAYSTATION);
    xbox_InputSetActivePad(KB, 0);
    CHECK(xbox_InputGlyphSet() == XBOX_GLYPH_SET_KEYBOARD && xbox_InputActivePadFamily() == KB);
    CHECK(st_last == XBOX_GLYPH_SET_KEYBOARD);                     /* the title is told */
    xbox_InputSetActivePad(KB, 0);                                 /* a second key: nothing changes */
    {
        int calls = st_calls;
        xbox_InputSetActivePad(KB, 0);
        CHECK(st_calls == calls);
    }
    xbox_InputSetActivePad(PS, 0);                                 /* the next pad button: the pad's set is back */
    CHECK(xbox_InputGlyphSet() == XBOX_GLYPH_SET_PLAYSTATION && xbox_InputActivePadFamily() == PS);
    CHECK(st_last == XBOX_GLYPH_SET_PLAYSTATION);
    xbox_InputSetActivePad(XM, 0);
    xbox_InputSetActivePad(KB, 0);
    xbox_InputSetActivePad(XM, 0);
    CHECK(st_last == XBOX_GLYPH_SET_XBOX_MODERN);
    xbox_InputSetGlyphMode(XBOX_GLYPHS_KEYBOARD);                  /* by hand: any pad leaves it */
    CHECK(st_last == XBOX_GLYPH_SET_KEYBOARD);
    xbox_InputSetActivePad(PS, 0);
    CHECK(xbox_InputGlyphSet() == XBOX_GLYPH_SET_KEYBOARD);
    xbox_InputSetGlyphMode(XBOX_GLYPHS_AUTO);
    CHECK(xbox_InputGlyphSet() == XBOX_GLYPH_SET_PLAYSTATION);

    printf("pad_glyphs: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
