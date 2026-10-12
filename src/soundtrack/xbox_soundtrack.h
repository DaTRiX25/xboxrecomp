/*
 * xbox_soundtrack.h - the user's music folder as Xbox custom soundtracks.
 *
 * A title that links the XDK soundtrack library (XFindFirstSoundtrack,
 * XGetSoundtrackSongInfo, XOpenSoundtrackSong) reads the dashboard's music
 * database, \Device\Harddisk0\Partition1\TDATA\FFFE0000\MUSIC\ST.DB, and
 * plays the songs it lists (MUSIC\<soundtrack>\<song id>.WMA) through the
 * XDK's WMA decoder. Here the database is made from a folder of the user's
 * own files and the songs are decoded by the host:
 *
 *   xst_library.c   the folder scan and the ST.DB image (no decoder needed)
 *   xst_decode.c    a song as 44.1 kHz 16-bit stereo, the format the
 *                   dashboard ripped to, from mp3 / flac / ogg / wav
 *   xst_host.c      the runtime side: the database on the emulated disk and
 *                   which song an opened handle is
 *
 * Opt-in: nothing here runs until a host calls xbox_SoundtrackInit with a
 * folder. The title-side hooks (which guest functions open a song and make
 * its decoder) belong to the port: see docs/runtime/soundtrack.md.
 *
 * The ST.DB layout is the one the XDK library in the title reads (512-byte
 * blocks: a header, 100 soundtrack blocks, then song-group blocks of six
 * songs); it is also described at https://xboxdevwiki.net/Soundtracks.
 */
#ifndef XBOX_SOUNDTRACK_H
#define XBOX_SOUNDTRACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The database's own limits: 100 soundtrack blocks, 84 song groups of six a
 * soundtrack. 500 songs is what the dashboard stopped at. */
#define XST_MAX_SOUNDTRACKS   100
#define XST_MAX_SONGS         500
#define XST_SONGS_PER_GROUP   6
#define XST_MAX_GROUPS        84
#define XST_NAME_UNITS        32          /* UTF-16 units, the NUL included */
#define XST_BLOCK             512
#define XST_MAX_DEPTH         6           /* folders below the chosen one */

/* The stream every song is delivered as. */
#define XST_RATE              44100
#define XST_CHANNELS          2

typedef struct xst_song {
    char    *path;                        /* host path, UTF-8 */
    uint16_t name[XST_NAME_UNITS];
    uint32_t ms;                          /* length, 0 when unknown */
    uint32_t id;                          /* (soundtrack id << 16) | index */
} xst_song;

typedef struct xst_soundtrack {
    uint16_t name[XST_NAME_UNITS];
    uint32_t id;                          /* 1.. */
    int      first, count;                /* its songs in xst_library.songs */
    uint32_t ms;                          /* sum of its songs */
} xst_soundtrack;

typedef struct xst_library {
    xst_soundtrack st[XST_MAX_SOUNDTRACKS];
    int       n_st;
    xst_song *songs;
    int       n_songs, cap_songs;
    /* What the limits and unreadable files left out. */
    int       dropped_folders, dropped_songs, bad_files;
} xst_library;

/* ── xst_library.c ─────────────────────────────────────────────────── */

/* Scan `dir`: every folder that holds playable files (the folder itself and
 * the folders below it, XST_MAX_DEPTH deep) is one soundtrack, named after
 * the folder, its songs in natural order ("2" before "10") and named by the
 * title tag, or the file name without the extension. Hidden entries (a
 * leading dot) are skipped. NULL when `dir` cannot be read; a folder with no
 * playable file gives a library with n_st == 0. */
xst_library *xst_scan(const char *dir);
void         xst_free(xst_library *lib);

const xst_song *xst_song_by_id(const xst_library *lib, uint32_t id);

/* The ST.DB image: xst_db_size bytes. Returns the bytes written, 0 when
 * `cap` is too small. */
size_t xst_db_size(const xst_library *lib);
size_t xst_db_build(const xst_library *lib, uint8_t *out, size_t cap);

/* Natural, case-insensitive order of two names (digit runs by value). */
int  xst_natural_cmp(const char *a, const char *b);
/* UTF-8 to a fixed UTF-16 name, cut at XST_NAME_UNITS - 1 units and kept
 * to Latin-1, which is as far as a title's menu font goes: decomposed
 * letters are composed, Latin Extended-A is folded to the nearest letter
 * ("ő" to "ö"), anything else above it is '?'. */
void xst_name_from_utf8(uint16_t out[XST_NAME_UNITS], const char *utf8);
/* 1 when the file name has an extension xst_decode.c plays. */
int  xst_is_playable_name(const char *name);

/* ── xst_decode.c ──────────────────────────────────────────────────── */

/* A file's length (ms, 0 unknown) and title tag ("" when it has none)
 * without decoding it. 0 when it is not a file this can play. */
int xst_probe(const char *path, uint32_t *ms, char *title, size_t title_cap);

typedef struct xst_stream xst_stream;

/* A song as XST_RATE stereo 16-bit frames, resampled and folded as needed.
 * NULL when the file cannot be opened or is not playable. */
xst_stream *xst_stream_open(const char *path);
/* Up to `frames` frames; fewer only at the end of the song, then 0. */
size_t      xst_stream_read(xst_stream *s, int16_t *out, size_t frames);
void        xst_stream_close(xst_stream *s);
/* The file's own rate and channel count (tests, the log). */
void        xst_stream_format(const xst_stream *s, unsigned *rate, unsigned *channels);

/* ── xst_host.c ────────────────────────────────────────────────────── */

/* Scan `music_dir`, write the database to the emulated disk (beside, not
 * over, a MUSIC folder the user copied from a console) and send the title's
 * MUSIC paths to it (xbox_PathRedirectHook; a hook already installed keeps
 * being asked first). Returns the number of songs; 0 leaves everything as it
 * was (no folder, nothing playable in it). Call once, after xbox_path_init
 * and before the title starts. */
int  xbox_SoundtrackInit(const char *music_dir);
int  xbox_SoundtrackActive(void);
const xst_library *xbox_SoundtrackLibrary(void);

/* The port's hook on the title's XOpenSoundtrackSong: `handle` (the guest's
 * 32-bit handle value) is the song `song_id` until it is noted again. */
void xbox_SoundtrackNoteOpen(uint32_t handle, uint32_t song_id);
/* The port's hook where the title makes the decoder for a handle: the song's
 * stream, or NULL when the handle is not one of ours (the title's own
 * decoder then takes it) or the file fails to open (*ours says which). */
xst_stream *xbox_SoundtrackOpenHandle(uint32_t handle, int *ours);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_SOUNDTRACK_H */
