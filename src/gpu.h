#ifndef NVFLUX_GPU_H
#define NVFLUX_GPU_H

/*
 * Thin layer over NVML, the library the NVIDIA driver ships for querying and
 * controlling GPUs (nvidia-smi is built on it). The library is loaded at
 * runtime, so nvflux builds without any NVIDIA headers.
 *
 * Functions returning int return 0 on success or an error code that
 * gpu_strerror() turns into a message.
 */

#define GPU_NAME_MAX   96
#define GPU_MAX_CLOCKS 64

struct gpu {
    unsigned index;
    void *handle;
    char name[GPU_NAME_MAX];
};

enum gpu_clock { GPU_CLOCK_CORE, GPU_CLOCK_MEM };

int gpu_init(void);
void gpu_shutdown(void);
const char *gpu_strerror(int err);

int gpu_count(unsigned *count);
int gpu_get(unsigned index, struct gpu *gpu);

/* Fills clocks with the distinct supported memory clocks, highest first.
 * *count holds the array size on entry and the number found on return. */
int gpu_mem_clocks(const struct gpu *gpu, unsigned *clocks, unsigned *count);
/* Highest core clock the GPU allows while memory runs at mem_mhz. The lowest
 * memory clocks belong to idle power states that also cap the core. */
int gpu_max_core_clock(const struct gpu *gpu, unsigned mem_mhz, unsigned *mhz);
int gpu_current_clock(const struct gpu *gpu, enum gpu_clock which, unsigned *mhz);

int gpu_lock_mem(const struct gpu *gpu, unsigned mhz);
int gpu_unlock_mem(const struct gpu *gpu);

/* Keep the driver initialised while the GPU is idle (persistence mode);
 * otherwise it can tear the GPU down and forget the lock. */
int gpu_keep_initialized(const struct gpu *gpu);

#endif
