#ifndef NVFLUX_GPU_H
#define NVFLUX_GPU_H

#include "nvml.h"
#include "policy.h"

/* One GPU, read once from the driver into plain data. */
struct gpu {
    unsigned index;
    nvml_dev dev;
    char name[NVML_NAME_LEN];
    char uuid[NVML_UUID_LEN];
    char bus_id[NVML_BUSID_LEN]; /* sysfs form, e.g. 0000:01:00.0 */
    unsigned arch;               /* NVML_ARCH_*, or NVML_ARCH_UNKNOWN */

    /* Supported clocks (owned arrays) and what this GPU can lock. */
    unsigned *mem_mhz;
    unsigned nmem;
    struct clocks *gfx;          /* one table per memory clock */
    struct gpu_caps caps;
};

/* Reads identity and clock tables. Returns an NVML status code. */
int gpu_open(unsigned index, struct gpu *g);
void gpu_close(struct gpu *g);

/* "Ampere", "Turing", ... for messages. */
const char *gpu_arch_name(unsigned arch);

/* Converts NVML's bus id (00000000:01:00.0) to the sysfs form (0000:01:00.0). */
void gpu_sysfs_bus_id(const char *nvml_id, char *out, unsigned len);

#endif
