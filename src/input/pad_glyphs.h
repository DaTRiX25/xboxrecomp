/*
 * pad_glyphs.h - which button prompts go with the pad in use.
 *
 * A title draws its prompts for the Xbox pad: A B X Y discs, L and R
 * triggers, white and black buttons, START. input.glyphs (the overlay's Input
 * page, RECOMP_INPUT_GLYPHS) chooses whose pad they are drawn for instead:
 *
 *   auto         the family of the pad that last pressed a button; a pad of
 *                an unknown family gets the positional set
 *   xbox         the title's own prompts, nothing replaced (the original
 *                pad: white and black buttons, L and R triggers)
 *   xbox_modern  A B X Y as they are, LB RB LT RT and the Menu button: an
 *                Xbox 360 / One / Series pad, which has no white or black
 *   playstation  cross / circle / square / triangle (coloured, as on the
 *                DualShock 3 and 4), L1 R1 L2 R2
 *   playstation5 the same four symbols as a DualSense prints them: light
 *                grey, no colours
 *   nintendo     A B X Y as a Nintendo pad prints them, L R ZL ZR
 *   steam        Valve's: the Steam Deck's own controls and the Steam
 *                Controllers. A B X Y as they are, L1 R1 L2 R2, Menu
 *   positional   any pad: no letters on the face buttons, a diamond of four
 *                dots with the one to press filled (bottom, right, left,
 *                top); L1 R1 L2 R2, Menu. auto picks it for
 *                a pad of unknown family (a handheld's built-in pad)
 *
 * This file is the choice, without SDL and without a renderer: the family of
 * a pad from what the host knows about it, and the glyph set that follows
 * from the setting, the family and the face-button layout. The pictures and
 * the textures they go into are the title's (nv2a_texrep.h); it hears of a
 * change through xbox_InputSetGlyphListener.
 */
#ifndef XBOX_PAD_GLYPHS_H
#define XBOX_PAD_GLYPHS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XBOX_PAD_FAMILY_XBOX,          /* a pad nothing is known about (an unknown family) */
    XBOX_PAD_FAMILY_XBOX_MODERN,   /* an Xbox 360 / One / Series pad (XInput) */
    XBOX_PAD_FAMILY_PLAYSTATION,
    XBOX_PAD_FAMILY_NINTENDO,
    XBOX_PAD_FAMILY_STEAM,
    XBOX_PAD_FAMILY_PLAYSTATION5,  /* a DualSense / DualSense Edge */
    XBOX_PAD_FAMILY_NONE,          /* no pad has been used (the state before the first button): the title's own */
    XBOX_PAD_FAMILY_COUNT
} xbox_pad_family;

/* input.glyphs, in the order of the setting's choices. */
typedef enum {
    XBOX_GLYPHS_AUTO,
    XBOX_GLYPHS_XBOX,
    XBOX_GLYPHS_XBOX_MODERN,
    XBOX_GLYPHS_PLAYSTATION,
    XBOX_GLYPHS_PLAYSTATION5,
    XBOX_GLYPHS_NINTENDO,
    XBOX_GLYPHS_STEAM,
    XBOX_GLYPHS_POSITIONAL,
    XBOX_GLYPHS_MODE_COUNT
} xbox_glyph_mode;

/* What is drawn. 0 is the title's own. Nintendo is two sets, because the
 * same four letters sit in other places on its pads:
 *   POSITION  the title's A is the bottom button, which a Nintendo pad
 *             prints B: the prompt for A shows B, for B A, for X Y, for Y X
 *   LABEL     the title's A is the button printed A: the letters stay, only
 *             the look changes
 * Positional is two sets for the same reason, and names the place of the
 * button instead of its letter:
 *   POSITIONAL          the title's A is the bottom button, B the right one,
 *                       X the left one, Y the top one
 *   POSITIONAL_SWAPPED  the pad is read by letter: A is the right button, B the
 *                       bottom one, X the top one, Y the left one */
typedef enum {
    XBOX_GLYPH_SET_XBOX,
    XBOX_GLYPH_SET_PLAYSTATION,
    XBOX_GLYPH_SET_NINTENDO_POSITION,
    XBOX_GLYPH_SET_NINTENDO_LABEL,
    XBOX_GLYPH_SET_STEAM,
    XBOX_GLYPH_SET_XBOX_MODERN,
    XBOX_GLYPH_SET_POSITIONAL,
    XBOX_GLYPH_SET_POSITIONAL_SWAPPED,
    XBOX_GLYPH_SET_PLAYSTATION5,
    XBOX_GLYPH_SET_COUNT
} xbox_glyph_set;

/* The family of a pad. by_type: what the host's own classification says
 * (SDL_GameControllerGetType): XBOX_MODERN for its Xbox 360 and Xbox One
 * types, XBOX when it says nothing. The USB ids and the name settle the rest:
 *
 *   Valve (28DE), any product but 11FF: STEAM. SDL2 has no type for these
 *     (sdl2-compat reports SDL3's SDL_GAMEPAD_TYPE_STEAM as unknown); the
 *     products SDL 3.4 lists are the Steam Controllers 1101 1102 1105 1106
 *     1142 1201 1202, the Steam Deck's own controls 1205, and the 2026 Steam
 *     Controller 1302 (cable), 1303 (Bluetooth), 1304 and 1305 (its dongles)
 *   28DE:11FF, Steam Input's virtual pad: every pad Steam wraps arrives as
 *     this one, whatever it is, so it says nothing about the pad in the
 *     hands. STEAM only when steam_deck says the machine is a Steam Deck (the
 *     SteamDeck=1 Steam sets there: its own controls are what is most likely
 *     held); elsewhere XBOX_MODERN: Steam hands every wrapped pad on as an
 *     Xbox 360 pad
 *   Sony 054C:0CE6 (DualSense) and 054C:0DF2 (DualSense Edge): PLAYSTATION5,
 *     whatever type the host gave (an SDL that predates the type says PS4)
 *   Sony (054C) and Nintendo (057E) pads the host has no type for
 *   a name with "Steam Deck" in it: STEAM
 *
 * name may be NULL. */
xbox_pad_family xbox_PadFamilyFromIds(xbox_pad_family by_type, unsigned vendor,
                                      unsigned product, const char *name,
                                      int steam_deck);

/* The set for a setting, the active pad and the face layout. Pure.
 *   label_mapped   the host already maps this pad's face buttons by their
 *                  printed letters (SDL does for a Nintendo pad, its
 *                  SDL_GAMECONTROLLER_USE_BUTTON_LABELS default)
 *   face_nintendo  input.face_layout = nintendo (A/B and X/Y change places
 *                  as the pad is read)
 * The rule: a prompt shows what is PRINTED on the button the player has to
 * press. On a pad printed the Nintendo way (A right, B below, X above, Y
 * left) the title's A is
 *   read by position, face layout xbox      the bottom button, printed B
 *   read by position, face layout nintendo  the right button, printed A
 *   read by letter,   face layout xbox      the right button, printed A
 *   read by letter,   face layout nintendo  the bottom button, printed B
 * so one of the two gives NINTENDO_LABEL, both or neither NINTENDO_POSITION,
 * and the same for B, X and Y (the footer's confirm and cancel with them).
 * The positional set asks the same question and answers with the place: the
 * button to press is the bottom one (POSITIONAL) or the right one
 * (POSITIONAL_SWAPPED) for the title's A. It works for any pad.
 * auto with an Xbox 360 / One / Series pad and face_nintendo on is
 * NINTENDO_LABEL: the player said the pad is printed the Nintendo way.
 * auto with a pad nothing is known about (FAMILY_XBOX: an SDL pad of no
 * known type or vendor, a handheld's built-in pad) is POSITIONAL: no letters
 * to be wrong (POSITIONAL_SWAPPED when face_nintendo says the pad is read by
 * letter). auto with no pad at all (FAMILY_NONE) is XBOX: the title's own
 * prompts, nothing replaced. */
xbox_glyph_set xbox_GlyphSetResolve(xbox_glyph_mode mode, xbox_pad_family family,
                                    int label_mapped, int face_nintendo);

/* input.glyphs as text and back ("auto", "xbox", ...); -1: not one. */
const char *xbox_GlyphModeName(xbox_glyph_mode m);
int         xbox_GlyphModeFromName(const char *s);

/* ── state ───────────────────────────────────────────────────────────── */

/* The setting. -1: back to RECOMP_INPUT_GLYPHS (auto when unset). */
void            xbox_InputSetGlyphMode(int mode);
xbox_glyph_mode xbox_InputGlyphMode(void);
/* The pad that last pressed a button (a host backend calls it). */
void            xbox_InputSetActivePad(xbox_pad_family family, int label_mapped);
xbox_pad_family xbox_InputActivePadFamily(void);
/* Resolve again (after the face layout changed) and tell the listener if
 * the set is another one. */
void            xbox_InputGlyphsRefresh(void);
xbox_glyph_set  xbox_InputGlyphSet(void);
/* fn(set) at once with the set in use, and on every change after. From the
 * thread that makes the change (the window's, the overlay's): fn must be
 * safe there. NULL: none. */
void xbox_InputSetGlyphListener(void (*fn)(int set));

#ifdef __cplusplus
}
#endif

#endif /* XBOX_PAD_GLYPHS_H */
