# Custom soundtracks: the user's music folder

`src/soundtrack` (library `xbox_soundtrack`, header `xbox_soundtrack.h`) lets
a title that plays Xbox custom soundtracks play a folder of the user's own
music. It is opt-in: nothing in it runs until the host calls
`xbox_SoundtrackInit` with a folder, and a port that never calls it links
code that is never reached.

## What a title does

A title with the feature links the XDK soundtrack library. Its strings are in
the executable: `\Device\Harddisk0\Partition1\TDATA\FFFE0000\MUSIC\ST.DB` and
`%s%04x\%08x.WMA`. The library reads the database the dashboard wrote when it
ripped a CD, and the title decodes the songs, WMA files, with the XDK's WMA
decoder into a DirectSound stream.

| XDK function | reads |
|---|---|
| `XFindFirstSoundtrack` / `XFindNextSoundtrack` | ST.DB: the header, then one soundtrack block a call |
| `XGetSoundtrackSongInfo(soundtrack, index, &id, &ms, name, n)` | ST.DB: the soundtrack's block, the song's group block |
| `XOpenSoundtrackSong(id, async)` | opens `MUSIC\<id >> 16, %04x>\<id, %08x>.WMA`, returns the handle |

## ST.DB

512-byte blocks, little-endian. Read from the library's code in a title
(Burnout 3: 0x001D0134-0x001D05E4); the same layout is described at
<https://xboxdevwiki.net/Soundtracks> and written by
<https://github.com/allkern/stdbtool>.

| block | contents |
|---|---|
| 0 | `u32 magic = 1`, `u32 soundtracks`, `u32 next soundtrack id`, `u32 id[100]`, `u32 next song id` |
| 1..100 | soundtrack: `u32 magic = 0x00021371`, `u32 id`, `u32 songs`, `u32 group[84]`, `u32 total ms`, `u16 name[32]` |
| 101.. | song group: `u32 magic = 0x00031073`, `u32 soundtrack id`, `u32 group id`, `u32 pad`, `u32 song id[6]`, `u32 ms[6]`, `u16 name[6][32]` |

Soundtrack i of the header's id list is block 1 + i; group g is block
101 + g; song n of a soundtrack is slot n % 6 of its group n / 6. Limits: 100
soundtracks, 84 groups (the dashboard stopped at 500 songs) a soundtrack,
names of 31 UTF-16 units.

`xst_db_build` writes soundtrack ids from 1 and song ids as
`(soundtrack id << 16) | index`: the library keeps the last soundtrack it
read in a zero-initialised cache keyed by id, so id 0 would match the empty
cache.

## The folder

`xst_scan(dir)`: every folder that holds playable files is one soundtrack,
named after the folder: the chosen folder itself (its loose files), then the
folders below it in natural order, six levels deep. Songs are in natural
order of file name ("2" before "10") and named by the title tag (ID3v2,
Vorbis comment), else the file name without its extension. Entries with a
leading dot are skipped. Past the limits, folders and songs are left out and
counted (`dropped_folders`, `dropped_songs`), files that do not open are
counted (`bad_files`); `xbox_SoundtrackInit` logs the counts.

Names are kept to Latin-1 (`xst_name_from_utf8`): a title's menu font goes no
further (Burnout 3's draws a box for U+0151). Decomposed letters (macOS file
names) are composed, Latin Extended-A is folded to the nearest letter ("ő"
to "ö", "ř" to "r"), typographic quotes and dashes to the plain ones, and
other scripts to "?".

The scan reads headers only: a FLAC's stream info, an Ogg's last page, a
WAV's header, and for an mp3 the first 64 KiB of its ID3v2 tag and its first
frame (the Xing/Info frame count, else size over bitrate).

## Decoding

`xst_stream_open(path)` / `xst_stream_read`: any of the formats as 44.1 kHz
16-bit stereo, the format the dashboard ripped to and what a title opens its
stream with. Other rates go through a 32-tap Kaiser-windowed sinc with the
cutoff under the lower Nyquist rate; mono is doubled, of more than two
channels the front pair is taken (with the centre at -3 dB).

| format | decoder | licence |
|---|---|---|
| mp3 | dr_mp3 0.7.4 (minimp3 inside) | public domain / MIT-0 (minimp3: CC0) |
| flac | dr_flac 0.13.4 | public domain / MIT-0 |
| wav | dr_wav 0.14.6 | public domain / MIT-0 |
| ogg (Vorbis) | stb_vorbis 1.22 | public domain / MIT |

On macOS also m4a (AAC, Apple Lossless), aiff and caf, through the system's
AudioToolbox (`ExtAudioFile`): no code of anyone's is added for it, and it is
where Apple Music and iTunes libraries are. Other hosts do not list those
files.

All single files under `third_party/`, compiled into `xst_decode.c`; no
library is linked, so every host (Windows, Linux, macOS, Android) builds the
same code. Not played: m4a/AAC off macOS and Opus (no decoder small enough to
vendor under a licence this project ships: FAAD2 is GPL, FDK-AAC's licence
is its own, FFmpeg is LGPL and large), WMA (the title's own decoder plays a console's real
`MUSIC` folder, see below).

## On the emulated disk

`xbox_SoundtrackInit(music_dir)` scans, writes the database to
`<save dir>/TDATA/FFFE0000/MUSIC.HOST/ST.DB` and installs a path redirect
(`xbox_PathRedirectHook`, chaining to one already there): every path under
`...\TDATA\FFFE0000\MUSIC\` opens that file. For `ST.DB` that is the
database; for a song it is merely a file that opens, since a title only
passes the handle on to its decoder.

A real `MUSIC` folder copied from a console into the save folder is not
touched: with the setting empty the title reads it as before, through its
own WMA decoder.

## What the port adds

Two guest functions, wrapped in the port's `recomp_manual.c` (the
recompiler's `sub_X` / `sub_X_gen` wrap):

1. the title's `XOpenSoundtrackSong`: after it, the port calls
   `xbox_SoundtrackNoteOpen(handle, song id)`;
2. the function that makes the WMA decoder for a handle: the port calls
   `xbox_SoundtrackOpenHandle(handle, &ours)` and, when the handle is one of
   ours, returns an object of its own in place of the XDK's: a vtable of the
   ten `XFileMediaObject` methods whose `Process` copies
   `xst_stream_read` into the title's packet.

Burnout 3's are in `src/game/soundtrack.c` of that port, with the addresses
and the player's state machine. Everything after the decoder (the title's
stream, its volume and mix bins, pause, skip, shuffle) is the title's code,
unchanged.

## Choosing the folder in the overlay

`xbox_OverlaySetMusicFolder(&ops)` (xbox_settings.h) puts a "Custom music
folder" row on the overlay's Audio page. The row opens a folder browser
drawn in the place of the page's rows; D-pad or arrows, A or Enter, B or Esc,
the mouse. Its model is `xst_browse.c` (`xsb_*`, no drawing, unit-tested):

- places: the folder in use, Music, Home, Downloads, the mounts (macOS
  /Volumes; Linux /run/media/<user>/<label>, /run/media/<label>, /media,
  /mnt; Windows drive letters; Android shared storage and /storage/<card>)
  and, off Windows, "Computer: /". The same list as the Burnout 3 setup
  window's disc browser, with Music first.
- a folder: "Use this folder", "..", its folders in natural order (1024 rows
  at most), each with the playable files in and below it, counted by name on
  the browser's own thread, six levels deep, 20 000 entries a row.
- B goes up one folder, from a place's own folder to the places, from the
  places back to the page.

The folder picked is stored (`xbox_SettingsSetText`) and handed to
`ops->chosen`; `ops->status` fills the line under the row. A port that can
take the change while its title runs calls
`xbox_SoundtrackSetFolder(dir, done)`: the scan runs on a thread
(`xbox_SoundtrackScanning` gives the counts for a progress line), then the
database is replaced (written beside and renamed over) and `done` is called
on that thread. What a title has cached of the old list is the port's to
know (Burnout 3 keeps the soundtrack count: its `soundtrack.c`).

## Paths are UTF-8

A folder or file name is UTF-8 everywhere in this library, the setting
included, and is handed to the system unchanged on POSIX hosts. On Windows
every call that takes a path converts to UTF-16 and uses the wide function:
the scan and the browser (`FindFirstFileW`, `GetFileAttributesW`), the
probes and decoders (`_wfopen`, dr_libs' `_w` opens, stb_vorbis through a
`FILE *`), the database (`_wfopen`, `MoveFileExW`). The two places where
Windows hands over a name in the ANSI code page are converted from the wide
environment: the home folder for the browser's places (`_wgetenv`), and
`RECOMP_AUDIO_MUSIC_DIR` when the user set it (`xbox_SettingsText`, which a
port reads the setting through; `--music-dir` arrives that way too).
`tests/soundtrack` scans, lists, probes and decodes through a folder named
"Árvíztűrő tükörfúrógép"; the test and the library cross-compile for Windows
(llvm-mingw) but have not been run there.

The title's own menu is another matter: names in ST.DB stay folded to
Latin-1 (above), as far as its font goes.

## Settings

`audio.custom_music_dir` in settings.toml (a text setting:
`xbox_SettingsText`), `RECOMP_AUDIO_MUSIC_DIR` in the environment. Empty is
off. Read at start: a change shows after a restart.

## Test

`tests/soundtrack`: the natural order, the names, a scan of a folder tree it
makes, the ST.DB image read back the way the XDK library reads it, the
limits, and the decoders (pass-through at 44.1 kHz; a 1 kHz tone through the
resampler from 22.05, 48 and 96 kHz and through mp3, FLAC and Ogg: frequency
and level; a 30 kHz tone at 96 kHz filtered out). The encoded fixtures in
`data/` are half a second of a sine made by `make_fixtures.sh`.
