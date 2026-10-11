# The in-game settings overlay

Dear ImGui over the window's Metal or Vulkan present (`src/overlay`,
`XBOXRECOMP_OVERLAY=ON`, which needs `XBOXRECOMP_ENHANCE`). The SDL-renderer
fallback window and Windows have none yet (Windows follows with the D3D11
path).

## Opening it

| Where | How |
|---|---|
| Controller | Back + Start together (on the Steam Deck: View held + Menu, which arrive as Back + Start) |
| Keyboard | F1 |
| macOS | Cmd + , |

Close with B or Esc. `RECOMP_OVERLAY=0` turns it off. For scripted runs:
`open` starts it open, `toggle=<s>,<s>` opens and closes it at those seconds,
`file=<path>` toggles it whenever that file appears (and removes it); a
`RECOMP_WINDOW_SHOT` pending while it is open is taken of the overlay over
the last frame.

## What it does to the title

* **Input.** While it is open `xbox_InputGetState` (src/input/input_core.c)
  returns neutral pads and keys, so the title sees nothing, and the title's
  rumble is silenced. After it closes a port stays neutral until its buttons
  have been let go, so the press that closed it is not the title's. This is
  at the one function the USB pad model, the XInput path and the keyboard
  all read, not in each backend.
* **Pause.** `xbox_HostPause` (src/kernel/kernel_pause.h). The executor (the
  nv2a-ack thread, standing in for the GPU) holds at the end of FLIP_STALL;
  the title runs into its own fence and pushbuffer-space waits within a
  frame or two and stops there, as it does when a console's GPU stalls. No
  guest thread is suspended from outside and no lock is held across the
  hold, so it cannot deadlock with what the title holds. The APU's frame
  thread idles (its existing `pause_requested` path, via a separate
  `host_paused` flag the title's own register writes do not clear), so
  sound stops. The game time is not frozen: Burnout 3 steps its simulation
  by elapsed time with a limit of four steps a frame (sub_001B58E0), so it
  catches up by at most that.
  The pause takes hold at the next flip, so a load or movie that does not
  flip finishes first; the overlay opens at once.

A pad's Back is held back from Start: with the overlay on, the title never
sees Start while Back is held (`xbox_InputSetComboGuard`), so pressing View
first and Menu second does not also pause the game. Menu first still does.

## Post passes and anisotropy

Anisotropic filtering applies to a mipmapped sampler with a linear
minification filter only (never a point-filtered one, never a render-target
read). FXAA and CAS run in the window presenters (`src/video/post_shaders.h`,
one neutral-dialect source for MSL and GLSL) at the game's internal
resolution, after the frame is final and before the scale to the window; the
overlay is drawn after them.

SMAA 1x is the reference implementation (`third_party/smaa`, MIT) at its
HIGH preset: edge detection by luma, blending weights from the area and
search tables, neighbourhood blending: three passes in place of FXAA's one.
`src/video/smaa.c` gives both presenters the same source (GLSL as it is;
for Metal a few exact text substitutions, checked at run time). A target
exists only while its setting is on (made at the first frame that needs it,
freed when none does), so switching live costs nothing while it is off.
`RECOMP_TRACE=post_gpu` prints each pass's GPU time every 300 presents
(Metal: the pass's own command buffer; Vulkan: timestamp queries, which a
tile-based GPU such as MoltenVK's does not give meaningfully). The
`aa_cycle=<s>` form of `RECOMP_OVERLAY` steps the setting for such a test.

## Settings

Each is one RECOMP_* knob. Precedence, high to low: the environment, then
`settings.toml` in the save folder (or the file `RECOMP_SETTINGS` names,
`none` for no file), then the title's defaults, then the toolkit's. A setting
the environment sets is shown locked.

`xbox_SettingsInit` (before the host's `env_default` calls) reads the file
and sets the variables it names, without replacing any already set;
`xbox_SettingsGameDefault` tells it each default; `xbox_SettingsFinish`
(after the last) takes the values the environment ends up with. So every
reader of the environment, including the enhancements layer (which then ranks
the file over `enhance.toml`), sees the file without knowing it exists.

The file is TOML, not INI: the toolkit already reads it (`recomp_cfg.h`,
`enhance.toml`), values are typed, and sections map to the overlay's pages.
Only settings the user changed are written, so the others follow the
title's defaults when those change. Keys it does not know are kept.

| Key | Variable | Live |
|---|---|---|
| `display.fullscreen` | `RECOMP_PRESENT_FULLSCREEN` | yes |
| `display.aspect` (`4:3`, `16:9`) | `RECOMP_DISPLAY_WIDESCREEN` | restart: the title reads the dashboard's video flags at boot |
| `display.filter` | `RECOMP_PRESENT_FILTER` | yes |
| `display.overscan` (`off`, `crop`) | `RECOMP_PRESENT_OVERSCAN` | yes |
| `display.fit` (`letterbox`, `fill16x10`) | `RECOMP_PRESENT_FIT` | yes |
| `graphics.render_scale` | `RECOMP_RENDER_SCALE` | restart |
| `graphics.aniso` (`off`, `2x` .. `16x`) | `RECOMP_GFX_ANISO` | yes (the sampler caches key on the level) |
| `graphics.aa` (`off`, `fxaa`, `smaa`) | `RECOMP_GFX_AA` | yes |
| `graphics.sharpen` (0..1) | `RECOMP_GFX_SHARPEN` | yes |
| `audio.volume`, `audio.mute`, `audio.mute_background` | `RECOMP_AUDIO_VOLUME`, `_MUTE`, `_MUTE_BG` | yes |
| `audio.speakers` (`auto`, `stereo`, `5.1`) | `RECOMP_AUDIO_SPEAKERS` | stored; 5.1 is not built (BO-76) |
| `game.speed_units` (`mph`, `kmh`) | `RECOMP_SPEED_UNITS` | live; Display page, only when the title enabled a guest patch for it (`recomp_patch_enable`, lifting: Guest patches) |
| `input.deadzone`, `input.trigger_deadzone` | `RECOMP_PAD_DEADZONE`, `RECOMP_PAD_TRIGGER_DEADZONE` | yes |
| `input.rumble` (strength, 0 off) | `RECOMP_INPUT_RUMBLE_GAIN` | yes |
| `power.mode` (`stock`, `performance`, `battery`) | `RECOMP_PRESENT_PACING` + `RECOMP_PRESENT_SPIN_LEAD` | yes |
| `advanced.perf_overlay` | `RECOMP_OVERLAY_PERF` | yes |

Power modes: *stock* is the busy-wait; *performance* sleeps and spins the
last 8 ms before each frame (steady frame time, about 10 W on the Deck);
*battery* sleeps throughout (about 8 W, small dips).

## Adding a graphics API

A backend is one file: init the official `imgui_impl_*` renderer with the
API's objects, and per frame call its NewFrame, `overlay_begin_frame()`,
`overlay_end_frame()`, its RenderDrawData (see `overlay_internal.h`), from
inside the presenter's pass. `overlay_metal.mm` and `overlay_vulkan.cpp` are
about 80 lines each.
