/* recomp_patch.c - see recomp_patch.h. */
#include "recomp_patch.h"

#include <string.h>

#define PATCH_MAX 8

static struct { const char *key; recomp_env_id env; } s_en[PATCH_MAX];
static volatile int s_n;   /* written once, before the guest runs */

void recomp_patch_enable(const char *key, recomp_env_id env)
{
    int i;
    for (i = 0; i < s_n; i++)
        if (!strcmp(s_en[i].key, key)) {
            s_en[i].env = env;
            return;
        }
    if (s_n == PATCH_MAX)
        return;
    s_en[s_n].key = key;
    s_en[s_n].env = env;
    s_n = s_n + 1;
}

int recomp_patch_enabled(const char *key)
{
    int i, n = s_n;
    for (i = 0; i < n; i++)
        if (!strcmp(s_en[i].key, key))
            return 1;
    return 0;
}

uint32_t recomp_patch_u32(const char *key, const char *when, uint32_t value,
                          uint32_t loaded)
{
    int i, n = s_n;
    for (i = 0; i < n; i++)
        if (s_en[i].key == key || !strcmp(s_en[i].key, key)) {
            const char *v = recomp_env(s_en[i].env);
            return v && !strcmp(v, when) ? value : loaded;
        }
    return loaded;
}
