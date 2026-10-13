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
| Android | the system Back (button or gesture): it does not end the game, it opens the overlay; inside, it goes back like B. Leave through the app switcher |

Close with B or Esc. `RECOMP_OVERLAY=0` turns it off. For scripted runs:
`open` starts it open, `toggle=<s>,<s>` opens and closes it at those seconds,
`file=<path>` toggles it whenever that file appears (and removes it); a
`RECOMP_WINDOW_SHOT` pending while it is open is taken of the overlay over
the last frame.

## Look and sizes

The overlay uses the whole window (it pauses the game anyway), over the
frame dimmed to 22%. Every size is a fraction of the window, the game's own
proportions measured on its 640x464 menu frame: label 3.7% of the height,
row pitch 6.25%, hint 2.6%, footer text 2.8%, header 4.7%; labels start at
14% of the width, values are right-aligned in a column from 56% to 94%, with
the arrows outside it; no upper cap, a floor of 13 px for a tiny window. The
content is at most 1.8 H wide and centred. A touch device (Android;
`RECOMP_OVERLAY_TOUCH=1` elsewhere, `RECOMP_OVERLAY_DPI` to force the
density) keeps a row at least 7 mm high. A text that does not fit its column
shrinks to 72% and is then cut. The selected row shows the value between
arrows; a switch is an On / Off value, a slider a bar of blocks. The palette
is the game's menu colours (title red, selected, list items, disabled,
hint, arrows, rule, footer bar, attention yellow: `k_theme_game`). The font
is still the monospace one. The Mixer page and the folder browser use the
same sizes and colours in a body under the header, their layout as before
(the Mixer's rows a little denser).

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

Because Back+Start opens the settings, the title sees a pad's Back a moment
late, as in SpeedBreaker (`xbox_InputSetComboGuard`, per pad, the pad only; the
keyboard is untouched). Back down: neither it nor Start reaches the title. Start
while Back is down: the overlay opens and the title never sees either. Back
let go alone: the title gets a Back press then (`XBOX_INPUT_BACK_TAP_MS`, at
least two reads). Back held `XBOX_INPUT_BACK_DELAY_MS` (250 ms) without Start:
passed on as a held Back, with Start still hidden while it is. Start first is
not delayed (a grace would put its wait on every pause); Back after it still
opens the overlay. The same combo closes the overlay. Described in
`src/input/README.md`, "Overlay hotkey".

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

## LOD bias and dither (both off by default)

`graphics.lod_bias` adds a mip LOD bias to every mipmapped texture, on top of
the title's own (SET_TEXTURE_FILTER's bias field). It is added to the per-draw
`tex_bias` constant that both backends hand the pixel shader (Metal's
`bias()`, GLSL's `texture(..., bias)`), because Metal has no sampler-state LOD
bias; so it composes with the title's bias by plain addition and with
anisotropic filtering as any shader bias does (the sampler's anisotropy and
min/max LOD clamps act on the biased level). A stage that is not mipmapped
gets none. `off` (default) adds exactly 0.0: the picture is the one it was.
`auto` is -log2(render scale) / 2: -0.5 at 2x, -1.0 at 4x (about -0.29 at
1.5x), 0 at 1x; `-0.5`, `-1`, `-1.5` are explicit. The environment takes any
number: `RECOMP_GFX_LOD_BIAS=-0.75`. Metal and Vulkan share the code
(`nv2a_host_lod_bias`); the Vulkan side has not been run.

## 2D art upscaling (`graphics.ui_scale`, off by default)

PPSSPP's texture scaling for the 2D art only: a texture the title draws its
HUD, menus and fonts with is scaled 2x or 4x on the CPU, once, when a GPU
backend uploads it (`src/kernel/nv2a_texscale.[ch]`; hooks in
`nv2a_pb_metal.m` and `nv2a_pb_vulkan.c` after `nv2a_tex_decode` and
`nv2a_texrep_apply`, so replaced button prompts are scaled too).

Which textures, by how they are used and what they are (no list of a title's
textures, no hashes):

- a draw with the depth test off and no depth write, into the frame's own
  surface (as wide as what the title presents; not a smaller effect target),
  binds it. From then on the texture is scaled for every draw that uses it;
- it is decoded by the ordinary 2D path from guest memory: not a render
  target, not a cube map, not a signed (bump) texture;
- a swizzled or DXT format (a linear one is a video frame or a copy of the
  screen), one mip level, at most 512x512 texels;
- opaque or not: photographs drawn in 2D (the World Tour's, the loading
  pictures, the static between screens) are scaled with the rest. Only the
  switch-on pass below, which cannot see how a texture is drawn, keeps to
  art cut out with alpha (one texel in sixteen not opaque).

The scaled texture gets the mip levels down to the title's size (2 levels at
2x, 3 at 4x) and a trilinear sampler over them, so an element drawn smaller
than the scaled texture is minified through them; texture coordinates,
`tex_scale` and everything else computed from a size keep the guest's size.
Nothing in the shaders reads a texture's size.

The scaler is Super-xBR (Hyllian, MIT; a C version of the three shader
passes, notice in `NOTICE`), on premultiplied colour with alpha as a fourth
channel and part of the edge measure, in the title's own (gamma) colour
values, as its blending and the GPU's bilinear filter work. It reads past a
texture's edge by repeating the edge texel (never the opposite edge). Inside
a sheet nothing knows where one cell ends: a result texel is made of the four
source texels around it, steered and slightly sharpened by those up to three
texels away, and held between the four's lowest and highest value (the
anti-ringing clamp), so a neighbouring cell's colour cannot appear where the
four nearest do not have it. That is the reach bilinear filtering of the
title's own texture has (one texel); cells packed with no gap meet as they do
unscaled. Fully transparent result texels beside visible ones take the
visible colour. 4x is two 2x steps.

`RECOMP_DEBUG=ui_scale_algo=bicubic` scales with a Catmull-Rom bicubic
instead, for comparison (its own cache files). The scalers use
single-precision arithmetic without fused multiply-add and no library
function, so one source gives the same bytes on every machine
(`tests/nv2a_texscale` holds a checksum per scaler and factor).

`RECOMP_DEBUG=ui_scale_log` prints one line per
texture and kind of draw that binds it (with `texrep_dump`'s hash), and the
time and memory of each scale.

Nothing is scaled on the render thread (`nv2a_texscale_store.c`). At upload
a picked texture goes to the GPU as the title has it, and its decoded texels
(hashed: `nv2a_texscale_key`) are queued for one worker thread. The worker
loads the scaled level 0 from the disk cache or scales it (rows on up to four
threads) and writes it; the backend asks again once a flip at the texture's
bind and swaps the scaled texture in, one texture a flip (a new texture
object, so draws already encoded keep the old one).

The disk cache is `<shader cache directory>/ui_scale/`
(`render.shader_cache`; per user, made from the user's own disc): a file a
texture, `<key>_<w>x<h>_x<factor>_<scaler><version>.tsc`, a 48-byte header
(what it is, a checksum of the texels) and the raw scaled level 0. Written
as `.tmp` and renamed; a file whose header, length or checksum is wrong is
deleted and the texture scaled again; `NV2A_TEXSCALE_VERSION` in the name
retires a changed scaler's files. Past 256 MB
(`RECOMP_DEBUG=ui_scale_cache_mb=<n>`, 0: no disk cache) the least recently
used files are deleted down to three quarters. No disk cache directory: the
worker still scales, only nothing is kept.

Filling it ahead of use: any run with the setting on writes what it draws,
so a title's shader pre-build run does (it must pass the setting to its
scenes and call `nv2a_texscale_flush` before it exits). When the setting is
switched on in the overlay, the backend goes through its texture cache
while the title is held for the pause (`xbox_HostPauseIdleFn`) and queues
every loaded texture of at most 256x256 that passes the texture half of the
rule, for the disk only (it cannot tell which are drawn in 2D, and the larger
one-level textures with alpha are mostly the world's); the row's note shows
`nv2a_texscale_progress`.

Off (the default) asks for factor 0: no draw is classified, nothing is
hashed or queued, no thread is started, and every path is the one it was. A
change of the setting reaches a texture at its next bind (the cache entry
remembers the factor it was uploaded at).

`graphics.dither` adds interleaved gradient noise (Jimenez 2014) of just
under +-0.5 LSB to the Metal presenter's scale to the window, the last step
before the 8-bit drawable. A texel that is already an exact 8-bit value
(1:1 text, flat colour) rounds back to itself, so only filtered, in-between
values move by one level at most. The source frame is itself 8-bit, so the
gain is at the edges of its bands when the frame is scaled to the window.
Off (default) skips the branch. The Vulkan presenter ends in `vkCmdBlitImage`
and has no such pass yet. Not in the menu; settings.toml or RECOMP_GFX_DITHER only.

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
| `graphics.lod_bias` (`off`, `auto`, `-0.5`, `-1`, `-1.5`) | `RECOMP_GFX_LOD_BIAS` (`0`, `auto` or a number) | yes (a per-draw constant) |
| `graphics.dither` (bool) | `RECOMP_GFX_DITHER` | yes; Metal only. Not in the menu; settings.toml or RECOMP_GFX_DITHER only |
| `graphics.ui_scale` (`off`, `2x`, `4x`) | `RECOMP_GFX_UI_SCALE` | yes (the texture caches upload a 2D texture again at its next bind) |
| `audio.volume`, `audio.mute`, `audio.mute_background` | `RECOMP_AUDIO_VOLUME`, `_MUTE`, `_MUTE_BG` | yes |
| `audio.speakers` (`auto`, `stereo`, `5.1`) | `RECOMP_AUDIO_SPEAKERS` | restart (auto: 5.1 when the default output has 6+ channels) |
| `audio.fx_level` (0..1, default 0.25) | `RECOMP_AUDIO_FX_LEVEL` | yes; 5.1 only: the title's effect-send bins, where they play dry. Not used (and not shown) for a title whose GP effects image is modelled (`src/apu/apu_gp.h`) |
| `audio.lfe_lowpass` (`off`, `80`, `100`, `120`; default `120`) | `RECOMP_AUDIO_LFE_LOWPASS` | not on the page (the settings file or the variable only; a change in a running game fades over 21 ms). For a title whose GP effects image is modelled: the low-pass on the LFE (subwoofer) feed, 24 dB an octave. 120 Hz is what the console's Dolby Digital encoder did after the effects stage and is the stock behaviour; off is that stage's own full-range feed. For tuning by ear use the mixer's own `lfe_lowpass_hz`, which comes after this one: with both on the feed passes both |
| `audio.mixer` (default off) | `RECOMP_AUDIO_MIXER` | yes; the host mixer (`src/apu/apu_bus.h`): buses over the title's voices, on the Audio page's Mixer page. Its mix is not in settings.toml but in `mixer.toml` beside it (`RECOMP_AUDIO_MIXER_FILE`), read again when it changes. Off, and on with the neutral preset, the output is the same as before |
| `audio.custom_music_dir` (a folder, default empty) | `RECOMP_AUDIO_MUSIC_DIR` | a text setting (`xbox_SettingsText`). The Audio page's first row, "Custom music folder", with a folder browser (its status line and notes right under it, the page's settings after them), when the port asked for it (`xbox_OverlaySetMusicFolder`); live if the port can (`soundtrack.md`) |
| `audio.custom_music_level` (`off`, `quieter`, `match`, `louder`; default `match`) | `RECOMP_AUDIO_CUSTOM_LEVEL` | live; the level of a title's custom soundtrack against its own music (`src/apu/apu_level.h`): a slow gain on the custom-soundtrack voices only (3 s envelope, -15 to +6 dB, 0.3 s down and 4 s up, frozen in silence), with `quieter` and `louder` at -4 and +4 dB on the target and `off` no processing at all (bit for bit the title's sends). Audio page, under "Custom music folder", only for a title that registered the folder row. Needs the port to name its custom-soundtrack voices (`apu_level_set_classifier`); a title that does not is not touched. `RECOMP_AUDIO_LEVEL_LOG=1` logs the envelope and gain once a second. Separately, always on: a no-look-ahead peak limiter at -0.5 dBFS in front of the 16-bit clamp (`src/apu/apu_limit.h`), 50 ms release, which changes nothing while the mix stays under the ceiling (`RECOMP_AUDIO_SAFETY_LIMITER=0` switches it off, for comparison) |
| `game.speed_units` (`mph`, `kmh`) | `RECOMP_SPEED_UNITS` | live; Display page, only when the title enabled a guest patch for it (`recomp_patch_enable`, lifting: Guest patches) |
| `display.ultrawide` (`off`, `21:9`, `32:9`, `window`) | `RECOMP_DISPLAY_ULTRAWIDE` | live, from the title's next 3D set-up; Display page, only when the title enabled a guest patch for it |
| `display.hud_fix` (bool) | `RECOMP_GFX_HUD_FIX` | live, shown whole from the title's next HUD set-up; Display page, only when the title enabled a guest patch for it |
| `graphics.bloom`, `graphics.motion_blur` (bool, default on) | `RECOMP_GFX_BLOOM`, `RECOMP_GFX_MOTION_BLUR` | live, from the next frame; the title's own effects, which a guest patch skips at `0`; Graphics page, only when the title enabled a guest patch for them |
| `graphics.crash_60fps` (bool, default off) | `RECOMP_GFX_CRASH_60FPS` | live, from the title's next frame; the title's own 30 Hz phases (Burnout 3: crash and takedown cameras) at 60, by a guest patch on the title's own rate choice at `1`; Graphics page, only when the title enabled a guest patch for it; for strong machines: a host that cannot hold 60 runs the game itself slower while it is on (the title steps once a frame), so the row says to leave it off on a Steam Deck and other handhelds |
| `graphics.split_60fps` (bool, default off) | `RECOMP_GFX_SPLIT_60FPS` | from the title's next two-player stage; the title's 30 Hz split screen at 60, by a guest patch at `1`; Graphics page, only when the title enabled a guest patch for it; for strong machines: a host that cannot hold 60 runs the game itself slower while it is on (the title steps once a frame), so the row says to leave it off on a Steam Deck and other handhelds |
| `audio.split_music` (bool, default off) | `RECOMP_AUDIO_SPLIT_MUSIC` | live; the title's music in split screen, where it plays none, by a guest patch at `1`; Audio page, only when the title enabled a guest patch for it |
| `graphics.vehicle_detail` (`original`, `far`, `farthest`) | `RECOMP_GFX_VEHICLE_DETAIL` | live, from the next frame; the distances at which the title swaps its vehicle models for simpler ones, scaled by a guest patch (2x, 4x); Graphics page, only when the title enabled a guest patch for it |
| `input.deadzone`, `input.trigger_deadzone` | `RECOMP_PAD_DEADZONE`, `RECOMP_PAD_TRIGGER_DEADZONE` | yes |
| `input.rumble` (strength, 0 to 3 in steps of 0.25; 1 the original, 0 off; motor = (speed / peak) ^ (1 / strength), see src/input/README.md) | `RECOMP_INPUT_RUMBLE_GAIN` | yes |
| `input.glyphs` (`auto`, `xbox`, `xbox_modern`, `playstation`, `playstation5`, `nintendo`, `steam`, `positional`, `keyboard`) | `RECOMP_INPUT_GLYPHS` | live: the title's button prompts drawn for the pad in use (`src/input/pad_glyphs.h`). `auto` follows the pad that last pressed a button (the keyboard counts, and shows key caps); `xbox` is the title's own picture, nothing replaced; `positional` is for any pad (no letters: a diamond of four dots, the one to press filled; `auto` picks it for a pad of unknown family, and the title's own picture while no pad has been used). Only a title that registers its prompt textures and brings pictures for them has anything to change (`src/kernel/nv2a_texrep.h`; Metal and Vulkan backends) |
| `power.mode` (`stock`, `performance`, `battery`) | `RECOMP_PRESENT_PACING` + `RECOMP_PRESENT_SPIN_LEAD` | yes |
| `advanced.perf_overlay` | `RECOMP_OVERLAY_PERF` | yes |
| `advanced.diagnostic_log` (bool; default on, off in a release build) | `RECOMP_LOG_DIAG` | yes; the print gate (`logging.md`). The row shows where the log is and what set it when the environment did |
| `advanced.perf_steps` (bool) | `RECOMP_OVERLAY_PERF_STEPS` | yes; the graph marks frames of more than one simulation step, when the port reports them (`xbox_PresentCadenceSteps`) |
| `display.present_timing` (`legacy`, `auto`) | `RECOMP_PRESENT_TIMING` | yes, within half a second; Display page, Metal windows only. `legacy` presents a frame when it is ready; `auto` aims each frame at one refresh (`present-timing.md`) |

Power modes: *stock* is the busy-wait; *performance* sleeps and spins the
last 8 ms before each frame (steady frame time, about 10 W on the Deck);
*battery* sleeps throughout (about 8 W, small dips).

The host's default is Balanced on every platform; the Burnout 3 port's macOS
default became Balanced too (BO-75, measured: as steady as Stock at about half
the CPU), with Maximum still offered there and a stored `power.mode = "stock"`
still honoured. *stock* (shown as Maximum) is offered on macOS only. On Windows, Android and
Linux / Steam Deck the Power page has Balanced (`performance`) and Battery
saver; a stored `stock` is read as `performance` at load (the file is not
rewritten for that; the next save writes the new value). The spin pacing
stays reachable for diagnosis with `RECOMP_PRESENT_PACING=spin`, which also
locks the row. The choice is one table, `k_stock_hidden` in
`src/overlay/xbox_settings.c` (BO-75; set an entry to 0 to offer Maximum
again); `xbox_PowerModeAtLoad(host, stored)` is the rule, testable per host
(`tests/overlay_settings`).

## Restart

Some settings are read once, when the game starts. The table in
`xbox_settings.c` says which with a flag, `XS_RESTART`, and that flag is the
only list: the row's RESTART tag and note, `xbox_SettingsNeedRestart`
and the Restart row all read it (the `overlay_settings` test names the
flagged ones, so adding a setting means deciding). Flagged today:
`display.aspect` (the dashboard bit the title reads at boot),
`graphics.render_scale` (render targets are made at start),
`audio.speakers` (the title picks its mix, and the device is opened, at
start), on Android `input.vibration`. Everything else is applied while
running or read each time it is used.

While a flagged setting differs from its value at launch, or a port asked
with `xbox_OverlayWantRestart(reason)`, or a save import waits, the foot of
every page shows one row, "Restart now to apply N changes" (N counts those
three), in attention yellow, over Quit game; its note has the reasons. A
changed setting's row carries a yellow RESTART tag beside its label; a
setting only read at start carries the same tag in grey. No page has a
Restart row of its own. Nothing restarts unless that row is picked. The row
closes the overlay (the settings are written), asks `recomp_restart`
for a restart and closes the window the ordinary way: the program's own
shutdown runs (motors off, audio closed, logs finished), and its last exit
handler starts it again with the arguments, directory and environment it
was first given (`src/platform/recomp_restart.h`): in place on Linux and
macOS (the same process id, so Steam, a launcher script, an AppImage's
runtime or a Mac bundle see one program throughout), as a new process on
Windows, through the activity (`restartGame()`) on Android. A port calls
`recomp_restart_init(argc, argv)` first thing in `main`; without that there
is no row, and the foot shows the yellow line "Restart the game to apply N
changes" instead (nothing to press).

### Restart now and Quit game (the foot of every page)

Every page ends with rows that stay in view below the page's own rows (the
rows scroll above them): "Restart now to apply N changes" (only while a
restart is pending) and "Quit game" (always). Both ask first: A (Enter, a
click) on the row shows "Press A again to restart / quit, B to cancel" in the
row and its note; the same row again does it. B or Esc, another row or
another page takes it back.

Quit game closes the overlay (settings written) and posts SDL_QUIT, the same
event as the window's close button, so the presenter's ordinary way out
runs: pads stopped, window and audio closed, `exit(0)`. Before that it calls
`recomp_quit_request` (`src/platform/recomp_restart.h`), whose host hook, set
by the SDL presenter on Android, calls `Activity.finishAndRemoveTask()`, so
the task does not stay behind. The Android part is not tested on a device.

## Save data: export and import

`xbox_OverlaySetSaveIO({save_dir, title_id, export_dir, note})` puts three
rows on the Advanced page, above the reset row (the functions are
`recomp_save_dir.h`'s; `tests/save_io` covers them on a made tree):

- **Export save to home folder**: copies the title's own save files,
  `UDATA/<title id>` and `TDATA/<title id>` of the save root, unchanged, to
  `<export_dir>/<YYYYMMDD-HHMMSS>/`, with a `save-export.txt` naming the
  title. Nothing else of the save root goes along. The full path is shown.
  The layout under the save root is the same on every host, so the dated
  folder can be carried to another machine's folder of the same name.
- **Import save from home folder (newest: ...)**: the newest export of this
  title in `export_dir`.
- **Import save from another folder...**: the folder browser (the music
  folder's, opened with other words: `xsb_open_with`), starting on "Exported
  saves", each folder marked when it is a save of this title.

An import never touches the save under the running title (the title holds
its save in memory and writes it back when it saves). The folder is checked
(it has `UDATA/<title id>` with files; its `save-export.txt`, if any, names
this title) and noted in the save root; the rows become "Cancel the import"
and the foot's "Restart now to apply 1 change". At the next start the
port calls `recomp_save_import_apply` before anything else looks at the save
root: the save in use is copied to
`<save root>/save-before-import/<date-time>/` (a folder of its own, which
the start-up backups' rotation never deletes), then replaced; if the copy
fails the old save is put back. The result is in the log (`[SAVE] Imported
the save from ...`) and under the rows for that run.

Messages, not silence: nothing to export yet, no export found, the target
cannot be written, not a save of this game, a save of another game.

Android: `export_dir` is in the app's own external files folder
(`/storage/emulated/0/Android/data/<package>/files/...`), the one place an
app writes without a permission; the row shows the full path. It is reached
over USB and by some file managers, not by all on Android 11 and later. A
place every file manager reaches (Download, Documents) needs the storage
access framework or a zip through MediaStore: not built.

## Save history: restore an earlier save

With the save guard on (`kernel_saveguard.h`, below) and a port that takes
a copy from its hook (`recomp_save_history_take`), the save root keeps
`save-history/<YYYYMMDD-HHMMSS>/`: the save as it was right before the game
wrote over it, each folder an export. Kept: the 20 newest, the newest of
each of the last 7 days and of each of the last 4 weeks; only dated, real
folders are removed (never a link, never a folder of another name).

**Restore an earlier save (N kept)...** (on the Advanced page with the
other save rows, when there is at least one) opens the folder browser in
`save-history`, newest first, the cursor on the newest. The folder chosen
is staged exactly like an import: "Restart now (the save is imported at the
start)", the save in use copied to `save-before-import/` first.

## Cheats (a tester's tool)

`xbox_OverlaySetCheats` registers up to four cheats, but their rows on the
Advanced page show only with `RECOMP_DEBUG=cheats`; players do not see them.
With the key set, a cheat on asks once more, and from the first one on the
title's saves go to a copy of the save (`<save root>/cheat-session`). When
the last goes off, the page asks whether to keep that progress (the save
goes into the save history first, then the copy replaces it) or throw it
away (the save stays as it was and saving waits for a restart). A copy left
behind by a quit is removed at the next start, with or without the key. A
port can also switch a cheat on without the row (the Burnout 3 port reads
`BO3_UNLOCK_ALL=1`; give such a run a scratch save).

## Saves are replaced whole (kernel_saveguard.h)

A port turns it on with `xbox_SaveGuardEnable("<title id>")` after
`xbox_path_init`. Then a file under `UDATA/<id>` or `TDATA/<id>` opened for
writing is a new file beside it (`<name>.~new`, a copy of the old one to
start with); at its last close a thread of the guard's own flushes it
(`F_FULLFSYNC` on macOS, `fsync`, `FlushFileBuffers`) and renames it over
the old name (`rename`, `MoveFileExW` with replace and write-through). The
title's close returns at once; a later open of the same file waits until
the rename is done. Same bytes as before: the new file is dropped and
nothing is renamed. Listings never show a `.~new`; one left by a process
that ended mid-save is removed at the next enable (the old file beside it
is the save). Every way out (`exit`, and the exit hooks of the windows that
end with `ExitProcess`) waits up to two seconds for a save on its way.
`tests/kernel_saveguard` ends a process at each step of a save and checks
the file is the old save or the new one, whole.

One running copy per save root: `recomp_instance_lock(save_root, wait_ms)`
(`recomp_instance.h`) holds `instance.lock` there for the life of the
process; a second copy is told and does not start. A restart gets it back.

## Text and fonts

Every string the overlay draws is UTF-8: its own labels and hints, a port's
rows, folder names in the music folder browser. Three faces, each a chain
Dear ImGui walks for a character (`overlay_fonts`, `gamefont_attach` in
`overlay_ui.cpp`); a glyph is rasterised when it is first drawn, at the size
it is drawn at, so there is no baked atlas and no list of ranges to keep:

| face | chain | used for |
|---|---|---|
| ui | Roboto Condensed Medium, then DejaVu Sans Mono | hints, the Mixer page, the folder browser, the graph |
| body | the title's text font (if registered), then ui's chain | the page name, tabs, row labels (capitals, leant), values, the footer keys |
| title | the title's large font, then its text font, then ui's chain | the one big word, SETTINGS |

| font | covers | licence |
|---|---|---|
| Roboto Condensed Medium, a subset (`overlay_font_ui.h`, 51 KB, 693 characters) | Basic Latin, Latin-1, Latin Extended-A and -B, Greek, Cyrillic, dashes and quotes, the euro and trade mark signs | SIL OFL 1.1 (`third_party/roboto_condensed/OFL.txt`, which a binary release carries) |
| DejaVu Sans Mono 2.37, a subset (`overlay_font_ext.h`, 41 KB, 630 characters) | the same scripts again, U+25A1 | Bitstream Vera licence (MIT kind; `third_party/dejavu/LICENSE.txt`, which a binary release carries) |

Roboto is made by `third_party/roboto_condensed/make_subset.py` from
google/fonts' variable `RobotoCondensed[wght].ttf` (checked by SHA-256),
pinned to wght 500.

### The title's own sprites and prompts

The same way, a port can name small DXT images inside a file of the
player's install (`xbox_OverlaySetGameArt`, `xbox_settings.h`: offsets and
sizes, the file's size and SHA-256; Burnout 3 names nine sprites of
`Data/Global.txd`): the up and left triangles and the tick of `Arrow`, the
`dpad` triangles, a rounded box's corner (`box_curve`, `16_curve` and their
`b` shapes), the `grid` tile, `big_curve` and the `Buttons` sheet. A thread
(`overlay_gameart.cpp`) reads and checks the file, decodes the sprites,
makes them 4x larger with Super-xBR and packs them into two RGBA textures
(one for the sprites, one for the prompt sheet of each of the ten glyph
sets) that Dear ImGui's backends upload like the font atlas; nothing is
written anywhere. They draw the cursor before the selected row, the arrows
beside a value and the page name, the value boxes (corners from the
sprites, sides plain, #101820 at 85% with a #385870 outline), the tick bar of
a percentage or scale row, the grid and the arc behind the rows, and the
footer: B Back, Y Default, the d-pad Change, A Select, as the buttons of the
pad in use (`xbox_OverlaySetPadSheets`: the port's per-set sheets, the
same images and rectangles it hands `nv2a_texrep`, over the title's own
`Buttons` sheet; `input.glyphs` and the active pad decide the set), or key
caps (Esc, Del, Arrows, Enter) when the last input was the keyboard; the
shoulder prompts sit beside the page names. The Mixer page and the folder
browser keep their text footer. No file or another file: the overlay draws
its own shapes and text as before (a log line, nothing on screen).
`RECOMP_OVERLAY_GAME_ART=0` forces that look.

### The title's own fonts

A port can name two bitmap fonts inside its title's executable
(`xbox_OverlaySetGameFonts`, `xbox_settings.h`): where each DXT5 atlas and
its glyph table are, the file's size and SHA-256. Burnout 3 names GlobalFont
and HeadFont of the US retail `default.xbe`. At start a thread reads the
player's own file, checks it, decodes the two atlases, scales them 4x with
Super-xBR (`nv2a_texscale`) and keeps the coverage in memory
(`overlay_gamefont.cpp`; about 90 ms on an M-series Mac); between two frames
the overlay then adds them in front of its own fonts. A font loader of the
overlay's own samples the 4x picture at whatever size a glyph is drawn, so
the letters stay sharp at 4K. Nothing decoded is written anywhere, and
nothing of the title is in this repository. No file, another file or a
table that does not read as glyphs: the overlay stays on Roboto everywhere,
scaled to the game font's capital height so the page keeps its proportions,
with one line in the log and no message on screen.
`RECOMP_OVERLAY_GAME_FONT=0` keeps Roboto.

The game's italics are its upright letters leant at draw time; the overlay
does the same (a shear of 0.2 applied to the drawn vertices, so a fallback
letter in the same string leans with them): the title, the page name, the
tabs and the labels are leant, values, hints and the keys upright. The
game's text font has Latin-1 and little more (no ő ű, no Cyrillic or
Greek): those letters come from Roboto, scaled to the game font's capital
height and set on its baseline, so "Zenék – ősz" mixes the two in one line.
The large font has capitals only.

A character no font has is drawn as a square (U+25A1), not as "?".
That is the case for CJK, Arabic, Hebrew, Indic scripts and emoji: out of
scope (a CJK font is megabytes, and right-to-left and combining scripts need
shaping this renderer does not do). A name in such a script still works: the
folder is found, chosen and stored by its real name; only its drawing is
squares.

Names from a disk may be decomposed (macOS stores "ő" as "o" + U+030B): the
browser's labels are composed first (`xst_utf8_compose`, Latin-1 and Latin
Extended-A), since the renderer does not stack combining marks.

## Shader pre-build

A host that can build its shaders ahead of time gives the overlay three calls
(`xbox_shader_build.h`, `xbox_OverlaySetShaderBuild`): start, cancel and poll
(state, fraction, text). The Graphics page then has a status line and an
action, *Build shaders now* (*Stop building shaders* while one runs, *Build
shaders again* after), with the host's own sentence on what it costs as the
hint. The overlay never opens by itself for it: the offer belongs to the
host's installer, and a player who skipped it there finds the row here. It
never starts a build on its own either. Closing the overlay stops a running build (the title is
paused while the overlay is open; a build beside a running title would cost
it frames), and the next one goes on from where it stopped. When a build
ends the host calls `nv2a_pcache_rescan`, and the backend builds what the
key list gained on its pre-warm thread.

What a build is (the scenes, the child process, the key list) is the host's
and the backends': see `nv2a_pcache.h`, `xbox_autoplay.h`, and Burnout 3's
`docs/shader-prebuild.md` for the one host that has it.

## Adding a graphics API

A backend is one file: init the official `imgui_impl_*` renderer with the
API's objects, and per frame call its NewFrame, `overlay_begin_frame()`,
`overlay_end_frame()`, its RenderDrawData (see `overlay_internal.h`), from
inside the presenter's pass. `overlay_metal.mm` and `overlay_vulkan.cpp` are
about 80 lines each.
