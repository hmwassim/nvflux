#define _POSIX_C_SOURCE 200809L
#include "apply.h"

#include <string.h>
#include <time.h>

static int unsupported(int rc)
{
    return rc == NVML_ERROR_NOT_SUPPORTED || rc == NVML_ERROR_FUNCTION_NOT_FOUND;
}

static int locks_anything(const struct target *t)
{
    return t->mem == MEM_LOCK || t->gfx == GFX_RANGE || t->gfx == GFX_CAP_BASE;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void keep_initialized(const struct gpu *g, const struct pci_gpu *pm, struct apply_result *r)
{
    if (pm && pm->runtime_pm)
        return;
    int on = 0;
    int rc = nvml_persistence(g->dev, &on);
    if (rc == NVML_SUCCESS && on)
        return; /* already on, e.g. through nvidia-persistenced */
    rc = nvml_set_persistence(g->dev, 1);
    if (rc != NVML_SUCCESS && !unsupported(rc))
        r->flags |= APPLY_PERSIST_FAILED;
}

static int apply_mem(const struct gpu *g, const struct target *t, struct apply_result *r)
{
    int rc = NVML_SUCCESS;
    if (t->mem == MEM_LOCK)
        rc = nvml_set_mem_locked(g->dev, t->mem_mhz, t->mem_mhz);
    else if (t->mem == MEM_RESET)
        rc = nvml_reset_mem_locked(g->dev);
    if (unsupported(rc)) {
        if (t->mem == MEM_LOCK)
            r->flags |= APPLY_MEM_SKIPPED;
        return NVML_SUCCESS;
    }
    return rc;
}

/* The driver keeps an earlier numeric lower limit when it gets the symbolic
 * "cap at base clock" (seen on an RTX 3060, driver 615: powersave after
 * balanced idled at balanced's 945 MHz floor). So every graphics lock starts
 * from a clean slate; failures here show up in the lock that follows. */
static void reset_first(const struct gpu *g)
{
    nvml_reset_gpu_locked(g->dev);
}

static int apply_gfx(const struct gpu *g, const struct target *t, struct apply_result *r)
{
    int rc = NVML_SUCCESS;
    switch (t->gfx) {
    case GFX_KEEP:
        return NVML_SUCCESS;
    case GFX_RESET:
        rc = nvml_reset_gpu_locked(g->dev);
        return unsupported(rc) ? NVML_SUCCESS : rc;
    case GFX_RANGE:
        reset_first(g);
        rc = nvml_set_gpu_locked(g->dev, t->gfx_min, t->gfx_max);
        r->gfx_min = t->gfx_min;
        r->gfx_max = t->gfx_max;
        break;
    case GFX_CAP_BASE:
        reset_first(g);
        rc = nvml_set_gpu_locked(g->dev, NVML_CLOCK_LIMIT_UNLIMITED, NVML_CLOCK_LIMIT_TDP);
        if (rc == NVML_ERROR_INVALID_ARGUMENT || rc == NVML_ERROR_NOT_SUPPORTED) {
            /* Older drivers don't know the symbolic values. */
            int rc2 = nvml_set_gpu_locked(g->dev, t->gfx_fallback_min, t->gfx_fallback_max);
            if (rc2 == NVML_SUCCESS) {
                r->flags |= APPLY_GFX_FALLBACK;
                r->gfx_min = t->gfx_fallback_min;
                r->gfx_max = t->gfx_fallback_max;
            }
            rc = rc2;
        }
        break;
    }
    if (unsupported(rc)) {
        r->flags |= APPLY_GFX_SKIPPED;
        return NVML_SUCCESS;
    }
    return rc;
}

/* Memory clock changes take a moment; wait up to 1.5 s for the lock to show. */
static void verify_mem(const struct gpu *g, const struct target *t, struct apply_result *r)
{
    unsigned now = 0;
    for (int i = 0; i < 7; i++) {
        if (nvml_clock(g->dev, NVML_CLOCK_MEM, &now) != NVML_SUCCESS || now <= t->mem_mhz)
            return;
        sleep_ms(250);
    }
    r->flags |= APPLY_MEM_OVERRIDDEN;
    r->observed_mem = now;
}

void apply_target(const struct gpu *g, const struct pci_gpu *pm, const struct target *t,
                  int verify, struct apply_result *r)
{
    memset(r, 0, sizeof(*r));

    if (locks_anything(t))
        keep_initialized(g, pm, r);

    int rc = apply_mem(g, t, r);
    if (rc != NVML_SUCCESS) {
        r->status = APPLY_FAILED;
        r->nvml_rc = rc;
        r->failed = "memory";
        return;
    }

    rc = apply_gfx(g, t, r);
    if (rc != NVML_SUCCESS) {
        if (t->mem == MEM_LOCK && !(r->flags & APPLY_MEM_SKIPPED)) {
            nvml_reset_mem_locked(g->dev);
            r->flags |= APPLY_ROLLED_BACK;
        }
        r->status = APPLY_FAILED;
        r->nvml_rc = rc;
        r->failed = "graphics";
        return;
    }

    int mem_wanted = t->mem == MEM_LOCK;
    int gfx_wanted = t->gfx == GFX_RANGE || t->gfx == GFX_CAP_BASE;
    int mem_done = mem_wanted && !(r->flags & APPLY_MEM_SKIPPED);
    int gfx_done = gfx_wanted && !(r->flags & APPLY_GFX_SKIPPED);
    if ((mem_wanted || gfx_wanted) && !mem_done && !gfx_done &&
        t->mem != MEM_RESET && t->gfx != GFX_RESET) {
        r->status = APPLY_UNSUPPORTED;
        return;
    }

    if (verify && mem_done)
        verify_mem(g, t, r);
    r->status = APPLY_OK;
}
