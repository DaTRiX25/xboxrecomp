/*
 * recomp_patch.h - the runtime half of the opt-in guest patches.
 *
 * tools.recomp --patches (tools/recomp/patches.py) follows a listed guest
 * load with
 *
 *     reg = RECOMP_PATCH_U32("game.speed_units", "kmh", 2u, reg);
 *
 * which lands in recomp_patch_u32: the value the guest loaded comes back
 * unchanged unless the setting named by `key` currently reads `when`, then
 * `value` does. A host opts a setting in with recomp_patch_enable, naming
 * the environment variable that carries it; that variable is the one the
 * overlay and settings.toml drive (xbox_settings.h), so a change is seen by
 * the next load, and the environment (RECOMP_SPEED_UNITS=kmh) works with no
 * overlay built. A setting nobody enabled is "off": the title runs as it was.
 */
#ifndef RECOMP_PATCH_H
#define RECOMP_PATCH_H

#include <stdint.h>

#include "recomp_env.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opt the setting `key` in; its value is the cached environment variable
 * `env`. Call from the host's main, before the guest runs. */
void recomp_patch_enable(const char *key, recomp_env_id env);
/* The setting was enabled (the overlay shows its row only then). */
int recomp_patch_enabled(const char *key);
/* A latched pair: a "set" site (the title builds something from the setting,
 * once) samples the setting; "use" sites (it reads the same choice every
 * frame) follow that sample, so a change made mid-way shows together, at the
 * next "set", and never as half a display. Before any "set" a "use" site
 * follows the setting. */
enum { RECOMP_PATCH_LATCH_SET = 1, RECOMP_PATCH_LATCH_USE = 2 };
uint32_t recomp_patch_u32_latch(const char *key, const char *when,
                                uint32_t value, uint32_t loaded, int mode);
/* What the generated code calls. Safe from any thread. */
uint32_t recomp_patch_u32(const char *key, const char *when, uint32_t value,
                          uint32_t loaded);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_PATCH_H */
