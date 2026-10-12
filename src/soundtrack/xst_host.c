/*
 * xst_host.c - the soundtrack library on the emulated disk.
 * See xbox_soundtrack.h.
 *
 * The title's library opens two kinds of path, both under
 * \Device\Harddisk0\Partition1\TDATA\FFFE0000\MUSIC\ :
 *
 *   ST.DB                     the database, read block by block
 *   <%04x>\<%08x>.WMA         a song, only opened (the handle then goes to
 *                             the decoder, which the port answers)
 *
 * Both are sent (xbox_PathRedirectHook) to one generated file,
 * TDATA\FFFE0000\MUSIC.HOST\ST.DB in the save folder: the database for the
 * first, any file that opens for the second. A MUSIC folder the user copied
 * from a console is not touched, and is what the title reads again once the
 * folder setting is cleared.
 */
#include "xbox_soundtrack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "kernel.h"

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define LOGP "[SOUNDTRACK] "

static const char MUSIC_DIR[]  = "\\Device\\Harddisk0\\Partition1\\TDATA\\FFFE0000\\MUSIC\\";
static const char HOST_DIR[]   = "\\Device\\Harddisk0\\Partition1\\TDATA\\FFFE0000\\MUSIC.HOST";
static const char HOST_DB[]    = "\\Device\\Harddisk0\\Partition1\\TDATA\\FFFE0000\\MUSIC.HOST\\ST.DB";

static xst_library *s_lib;
static int          s_active;
static const char *(*s_prev_hook)(const char *xbox_path);

/* The last handles XOpenSoundtrackSong returned. The title holds one or two
 * at a time; a handle value the kernel reuses is simply noted again. */
#define OPEN_SLOTS 16
static struct { uint32_t handle, song; } s_open[OPEN_SLOTS];
static unsigned s_open_next;
static volatile LONG s_open_lock;

static void open_lock(void)   { while (InterlockedExchange(&s_open_lock, 1)) ; }
static void open_unlock(void) { InterlockedExchange(&s_open_lock, 0); }

static int prefix_ci(const char *s, const char *prefix)
{
    int i;
    for (i = 0; prefix[i]; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    return i;
}

static const char *redirect(const char *xbox_path)
{
    if (s_prev_hook) {
        const char *to = s_prev_hook(xbox_path);
        if (to)
            return to;
    }
    if (s_active && xbox_path && prefix_ci(xbox_path, MUSIC_DIR))
        return HOST_DB;
    return NULL;
}

static int write_db(const uint8_t *db, size_t size)
{
    xbox_host_char dir[MAX_PATH], file[MAX_PATH];
    FILE *f;
    size_t i;
    int ok;

    if (!xbox_translate_path(HOST_DIR, dir, MAX_PATH)
        || !xbox_translate_path(HOST_DB, file, MAX_PATH))
        return 0;
    /* The folder and the one above it; TDATA itself the path layer makes. */
    for (i = 1; dir[i]; i++) {
        if (dir[i] != '/' && dir[i] != '\\')
            continue;
        {
            xbox_host_char c = dir[i];
            dir[i] = 0;
#if defined(_WIN32)
            _wmkdir(dir);
#else
            mkdir(dir, 0755);
#endif
            dir[i] = c;
        }
    }
#if defined(_WIN32)
    _wmkdir(dir);
    f = _wfopen(file, L"wb");
#else
    mkdir(dir, 0755);
    f = fopen(file, "wb");
#endif
    if (!f)
        return 0;
    ok = fwrite(db, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

int xbox_SoundtrackInit(const char *music_dir)
{
    xst_library *lib;
    uint8_t *db;
    size_t size;
    int i;

    if (s_active || !music_dir || !*music_dir)
        return s_active ? s_lib->n_songs : 0;
    lib = xst_scan(music_dir);
    if (!lib) {
        fprintf(stderr, LOGP "%s: cannot read the folder; custom music is off\n", music_dir);
        return 0;
    }
    if (lib->n_songs == 0) {
        fprintf(stderr, LOGP "%s: no mp3, flac, ogg or wav file found; custom music is off\n",
                music_dir);
        xst_free(lib);
        return 0;
    }
    size = xst_db_size(lib);
    db = (uint8_t *)malloc(size);
    if (!db || xst_db_build(lib, db, size) != size || !write_db(db, size)) {
        fprintf(stderr, LOGP "cannot write the soundtrack database; custom music is off\n");
        free(db);
        xst_free(lib);
        return 0;
    }
    free(db);

    s_lib = lib;
    s_prev_hook = xbox_PathRedirectHook;
    s_active = 1;
    xbox_PathRedirectHook = redirect;

    fprintf(stderr, LOGP "%s: %d soundtrack%s, %d song%s\n", music_dir,
            lib->n_st, lib->n_st == 1 ? "" : "s", lib->n_songs, lib->n_songs == 1 ? "" : "s");
    for (i = 0; i < lib->n_st && i < 12; i++) {
        char name[XST_NAME_UNITS];
        int k;
        for (k = 0; k < XST_NAME_UNITS; k++)
            name[k] = lib->st[i].name[k] < 0x80 ? (char)lib->st[i].name[k] : '?';
        name[XST_NAME_UNITS - 1] = 0;
        fprintf(stderr, LOGP "  %2u \"%s\": %d song%s, %u:%02u\n", (unsigned)lib->st[i].id, name,
                lib->st[i].count, lib->st[i].count == 1 ? "" : "s",
                (unsigned)(lib->st[i].ms / 60000), (unsigned)(lib->st[i].ms / 1000 % 60));
    }
    if (lib->dropped_folders || lib->dropped_songs || lib->bad_files)
        fprintf(stderr, LOGP "left out: %d folder%s past the %d soundtracks, %d song%s past "
                "%d in a folder, %d unreadable file%s\n",
                lib->dropped_folders, lib->dropped_folders == 1 ? "" : "s", XST_MAX_SOUNDTRACKS,
                lib->dropped_songs, lib->dropped_songs == 1 ? "" : "s", XST_MAX_SONGS,
                lib->bad_files, lib->bad_files == 1 ? "" : "s");
    return lib->n_songs;
}

int xbox_SoundtrackActive(void)
{
    return s_active;
}

const xst_library *xbox_SoundtrackLibrary(void)
{
    return s_lib;
}

void xbox_SoundtrackNoteOpen(uint32_t handle, uint32_t song_id)
{
    unsigned i;

    if (!s_active || handle == 0xFFFFFFFFu || handle == 0)
        return;
    open_lock();
    for (i = 0; i < OPEN_SLOTS; i++)
        if (s_open[i].handle == handle)
            break;
    if (i == OPEN_SLOTS)
        i = s_open_next++ % OPEN_SLOTS;
    s_open[i].handle = handle;
    s_open[i].song = song_id;
    open_unlock();
}

xst_stream *xbox_SoundtrackOpenHandle(uint32_t handle, int *ours)
{
    const xst_song *song = NULL;
    xst_stream *s;
    unsigned i, rate = 0, ch = 0;

    if (ours)
        *ours = 0;
    if (!s_active)
        return NULL;
    open_lock();
    for (i = 0; i < OPEN_SLOTS; i++)
        if (s_open[i].handle == handle && handle) {
            song = xst_song_by_id(s_lib, s_open[i].song);
            break;
        }
    open_unlock();
    if (!song)
        return NULL;
    if (ours)
        *ours = 1;
    s = xst_stream_open(song->path);
    if (!s) {
        fprintf(stderr, LOGP "cannot play %s\n", song->path);
        return NULL;
    }
    xst_stream_format(s, &rate, &ch);
    fprintf(stderr, LOGP "song %08X: %s (%u Hz, %u ch)\n", (unsigned)song->id, song->path, rate, ch);
    return s;
}
