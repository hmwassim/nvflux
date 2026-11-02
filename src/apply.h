#ifndef NVFLUX_APPLY_H
#define NVFLUX_APPLY_H

#include "gpu.h"
#include "policy.h"
#include "sysfs.h"

enum apply_status {
    APPLY_OK,
    APPLY_FAILED,      /* nothing changed on this GPU (rolled back if needed) */
    APPLY_UNSUPPORTED, /* the driver refused every part as unsupported */
};

enum {
    APPLY_MEM_SKIPPED    = 1 << 0, /* driver said memory locks are unsupported */
    APPLY_GFX_SKIPPED    = 1 << 1,
    APPLY_GFX_FALLBACK   = 1 << 2, /* symbolic base-clock cap refused; numeric cap used */
    APPLY_MEM_OVERRIDDEN = 1 << 3, /* memory runs above the lock (displays need more) */
    APPLY_ROLLED_BACK    = 1 << 4, /* graphics failed, so the memory lock was undone */
    APPLY_PERSIST_FAILED = 1 << 5, /* couldn't enable persistence mode */
};

struct apply_result {
    enum apply_status status;
    unsigned flags;
    int nvml_rc;            /* on failure */
    const char *failed;     /* "memory" or "graphics" */
    unsigned gfx_min, gfx_max; /* graphics range actually set (fallback included) */
    unsigned observed_mem;  /* with APPLY_MEM_OVERRIDDEN */
};

/*
 * Applies a target to one GPU, all or nothing: memory first, then graphics;
 * if graphics fails after memory was locked, the memory lock is reset.
 *
 * pm describes the GPU's runtime power management (NULL if unknown).
 * Persistence mode is only turned on for GPUs without it: on a laptop it
 * would keep the GPU awake and drain the battery.
 *
 * With verify set, checks afterwards that memory actually runs at the lock;
 * with several or high-refresh monitors the driver may keep it higher.
 */
void apply_target(const struct gpu *g, const struct pci_gpu *pm, const struct target *t,
                  int verify, struct apply_result *r);

#endif
