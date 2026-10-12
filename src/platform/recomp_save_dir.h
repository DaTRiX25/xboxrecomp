/*
 * recomp_save_dir.h - where a title's saves live, and their backups.
 *
 * The save root holds what the kernel maps there (UDATA, TDATA, the HDD
 * partition images; kernel_path.c) and the crash reports (xbox_crash.h). By
 * platform, as other ports keep theirs:
 *
 *   macOS    ~/Library/Application Support/<Name>
 *   Linux    $XDG_DATA_HOME/<name>, else ~/.local/share/<name>
 *   Windows  %APPDATA%\<Name>  (under Proton: the prefix's
 *            drive_c/users/steamuser/AppData/Roaming/<Name>)
 *
 * RECOMP_SAVE_DIR=<dir> replaces it, which is how a test run keeps its own.
 */
#ifndef RECOMP_SAVE_DIR_H
#define RECOMP_SAVE_DIR_H

#ifdef __cplusplus
extern "C" {
#endif

/* The save root, created: RECOMP_SAVE_DIR, else the platform directory for
 * `name` (lower-cased on Linux). Resolved once; the same string after. */
const char *recomp_save_dir(const char *name);

/* A per-user folder by platform, not created. 1 with the path in `out`.
 *
 *   RECOMP_DIR_DATA   the save root above, without RECOMP_SAVE_DIR
 *   RECOMP_DIR_LOCAL  for what is large and stays on this machine (a title's
 *                     data): Windows %LOCALAPPDATA%\<Name>; elsewhere the same
 *                     folder as RECOMP_DIR_DATA
 *   RECOMP_DIR_CACHE  macOS ~/Library/Caches/<Name>, Linux $XDG_CACHE_HOME/<name>
 *                     else ~/.cache/<name>, Windows %LOCALAPPDATA%\<Name>\cache
 *                     (where nv2a_pcache.h keeps the shader cache)
 */
enum { RECOMP_DIR_DATA, RECOMP_DIR_LOCAL, RECOMP_DIR_CACHE };
int recomp_user_dir(int kind, const char *name, char *out, unsigned out_size);

/* A folder that got a new name: move `old_dir` to `new_dir`, once. Only when
 * `old_dir` is a folder and nothing is at `new_dir` (a folder without a file
 * in it is nothing); no file is ever deleted or written over, so a second call, or
 * both being there, changes nothing.
 * 1 moved, 0 nothing to do, -1 it could not be moved (both are as they
 * were). Call it before anything makes `new_dir` (recomp_save_dir does). */
int recomp_dir_migrate(const char *old_dir, const char *new_dir);
/* Whether recomp_dir_migrate would move it: `old_dir` is a folder (not a
 * link) and `new_dir` is not there, or is there with no file anywhere in it
 * (such a one gives way: something that only asked where the folder is may
 * have made it, and an empty cache folder inside it).
 * Reads only. */
int recomp_dir_migrate_pending(const char *old_dir, const char *new_dir);

/* Copy a save folder from before this (the working directory's saves/) into
 * `dir`, once: only when `legacy` has a UDATA, `dir` has none and `legacy`
 * has no MOVED.txt from an earlier copy. The
 * partition images are not copied (sparse, and remade at start); the legacy
 * folder is left as it was, with a MOVED.txt naming the new place.
 * 1 when it copied. */
int recomp_save_dir_migrate(const char *legacy, const char *dir);

/* Copy dir/UDATA and dir/TDATA to dir/backup/<YYYYMMDD-HHMMSS>/, unless they
 * are byte for byte the newest backup, and keep the `keep` newest backups.
 * Call at start, before the title can write. Fills `out` (may be NULL) with
 * the backup made, or "" when none was needed. 1 when it made one. */
int recomp_save_backup(const char *dir, int keep, char *out, unsigned out_size);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_SAVE_DIR_H */
