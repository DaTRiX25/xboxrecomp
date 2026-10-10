/*
 * nv2a_pcache.h - the on-disk pipeline-key list a GPU backend pre-warms from.
 *
 * A backend compiles a shader (or a pipeline) the first time a draw needs it,
 * on the NV2A ack thread, so every cold start stalls a frame on each one. This
 * file remembers what was compiled, as the backend's own keys (the vertex
 * program's slots, the combiner state, the pipeline's fixed state), so the
 * next start compiles them before the title draws with them. It stores keys,
 * not compiled code: the backend regenerates the source from a key, so a
 * changed generator never runs stale code (the version string still drops a
 * file from another build, as the key layout may have changed with it).
 *
 * The approach is the one emulators converged on: Dolphin keeps a per-game
 * list of pipeline UIDs under a 'PUID' + GX_PIPELINE_UID_VERSION header and
 * compiles all of it at startup on its pre-compiler threads
 * (Source/Core/VideoCommon/ShaderCache.cpp, InitializeShaderCache and the
 * UID cache load; on Metal it replays only the UIDs, MTLUtil.mm turns the
 * binary caches off); xemu keeps its GL shader binaries under a header of
 * its version and the driver's, deletes a mismatch, and loads the list on a
 * background thread (hw/xbox/nv2a/pgraph/gl/shaders.c); UnleashedRecomp
 * ships a pipeline-state list it compiles during the intro logos and
 * compiles new ones on worker threads (UnleashedRecomp/gpu/video.cpp, the
 * pipeline pre-compilation and EnqueueGraphicsPipelineCompilation).
 *
 * File: a header (magic, format, the caller's version string), then records
 * { u32 type, u32 len, u64 fnv64(type, payload), payload[len] }, appended as
 * they are made. On open: a missing, foreign or wrong-version file is started
 * afresh (deleted); a record that fails its check ends the file there, and the
 * file is rewritten without the tail (a torn append from a killed process).
 * Appends are one fwrite + fflush each, so a killed process loses at most the
 * record being written. Identical records are written once.
 *
 * Threading: open, put and close on one thread; get/count only read what open
 * loaded and may run on any thread once open returned.
 */
#ifndef NV2A_PCACHE_H
#define NV2A_PCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nv2a_pcache nv2a_pcache;

/* Largest payload a record may have. */
#define NV2A_PCACHE_MAX_LEN 65536u

/* Opens <path>, creating it. `version` names the build that may read it;
 * a file written under another is deleted and started afresh. Returns NULL
 * when the file can neither be read nor created (the cache is then off);
 * `msg` (may be NULL) gets one line on what was found: loaded, new, version
 * mismatch, a cut tail. */
nv2a_pcache *nv2a_pcache_open(const char *path, const char *version,
                              char *msg, size_t msg_len);
/* Records loaded at open (not counting those put since). */
size_t nv2a_pcache_count(const nv2a_pcache *pc);
/* Record i of those loaded: 1, or 0 past the end. */
int nv2a_pcache_get(const nv2a_pcache *pc, size_t i, uint32_t *type,
                    const void **data, uint32_t *len);
/* Appends a record. 1 written, 0 already in the file, -1 error. */
int nv2a_pcache_put(nv2a_pcache *pc, uint32_t type, const void *data, uint32_t len);
void nv2a_pcache_close(nv2a_pcache *pc);

/* FNV-1a, 64-bit, continued from `h` (start with NV2A_FNV64_INIT). */
#define NV2A_FNV64_INIT 0xcbf29ce484222325ull
uint64_t nv2a_fnv64(const void *p, size_t n, uint64_t h);

/* The directory pipeline caches go in, created if missing: `dir` when it is
 * set (a leading "~/" is $HOME), else the platform's per-user cache directory
 * (macOS ~/Library/Caches/<app>, elsewhere $XDG_CACHE_HOME or ~/.cache/<app>,
 * Windows %LOCALAPPDATA%\<app>). "none", "off", "0" or "" turn it off.
 * Returns 1 with the path in `out`, 0 when off or not creatable. */
int nv2a_pcache_dir(const char *dir, const char *app, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PCACHE_H */
