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

Close with B or Esc. `RECOMP_OVERLAY=0` turns it off; `RECOMP_OVERLAY=open`
starts it open (screenshots).

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
| `audio.volume`, `audio.mute`, `audio.mute_background` | `RECOMP_AUDIO_VOLUME`, `_MUTE`, `_MUTE_BG` | yes |
| `audio.speakers` (`auto`, `stereo`, `5.1`) | `RECOMP_AUDIO_SPEAKERS` | stored; 5.1 is not built (BO-76) |
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
