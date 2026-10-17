/*
 * kernel_save_import.h - bring a save from an original Xbox (or xemu) into
 * the port's save layout.
 *
 * What the original title wrote lives under E:\UDATA\<title id>\<save id>\
 * (SaveMeta.xbx and the profile, here "Profile 1"), with the title's
 * TitleMeta.xbx, TitleImage.xbx and SaveImage.xbx one level up, and
 * sometimes E:\TDATA\<title id>. The profile ends in a 20-byte signature,
 *     HMAC-SHA1(XboxHDKey, HMAC-SHA1(XboxSignatureKey, file minus signature))
 * and the port keeps both keys at zero, so the profile is signed again with
 * zeros. SaveMeta.xbx and the other files carry no signature.
 *
 * The input is a save folder (the <save id> folder), a folder that holds it
 * (the title id folder, UDATA, or a whole E: drive copy), or a .zip of any of
 * them. The result is a folder shaped like a save export (recomp_save_dir.h:
 * UDATA/<title id>/... plus save-export.txt), which the port's own import
 * puts in place at the next start, with the save in use set aside first.
 */
#ifndef KERNEL_SAVE_IMPORT_H
#define KERNEL_SAVE_IMPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Whether `path` (a folder, or a .zip file) holds an Xbox save of this
 * title: a quick look for a file list, no copy made. */
int xbox_SaveImportCheck(const char *path, const char *title_id);

/*
 * Make the export-shaped folder <work_root>/import-work/out from `input`.
 *   keep_root  the save root in use, or NULL: its title-level files (title
 *              image and meta) fill in what the input lacks.
 * 1 and the folder in `out`; `info` gets a line for the player (profile
 * name, files, how it was signed). 0 with a sentence in `err`; nothing in
 * the save root is touched either way.
 */
int xbox_SaveImportPrepare(const char *input, const char *title_id, const char *keep_root,
                           const char *work_root, char *out, unsigned out_size,
                           char *info, unsigned info_size, char *err, unsigned err_size);

/* Remove the work folder (after the import has been applied). */
void xbox_SaveImportCleanup(const char *work_root);

#ifdef __cplusplus
}
#endif
#endif
