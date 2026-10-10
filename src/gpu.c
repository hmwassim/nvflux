#define _GNU_SOURCE
#include "gpu.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The few declarations we need from nvml.h. NVML keeps these signatures
 * stable across driver releases (versioned functions get a _vN suffix).
 */
typedef int nvmlReturn_t;
typedef struct nvmlDevice_st *nvmlDevice_t;

enum {
    NVML_SUCCESS                  = 0,
    NVML_ERROR_NOT_SUPPORTED      = 3,
    NVML_ERROR_NO_PERMISSION      = 4,
    NVML_ERROR_DRIVER_NOT_LOADED  = 9,
    NVML_ERROR_LIBRARY_NOT_FOUND  = 12,
    NVML_ERROR_FUNCTION_NOT_FOUND = 13,

    UNTRUSTED_LIBRARY = 1000, /* ours, outside NVML's range */
};

enum { NVML_CLOCK_GRAPHICS = 0, NVML_CLOCK_MEM = 2 };
enum { NVML_FEATURE_DISABLED = 0, NVML_FEATURE_ENABLED = 1 };

static struct {
    nvmlReturn_t (*Init_v2)(void);
    nvmlReturn_t (*Shutdown)(void);
    const char *(*ErrorString)(nvmlReturn_t);
    nvmlReturn_t (*DeviceGetCount_v2)(unsigned *);
    nvmlReturn_t (*DeviceGetHandleByIndex_v2)(unsigned, nvmlDevice_t *);
    nvmlReturn_t (*DeviceGetName)(nvmlDevice_t, char *, unsigned);
    nvmlReturn_t (*DeviceGetSupportedMemoryClocks)(nvmlDevice_t, unsigned *, unsigned *);
    nvmlReturn_t (*DeviceGetSupportedGraphicsClocks)(nvmlDevice_t, unsigned, unsigned *, unsigned *);
    nvmlReturn_t (*DeviceGetClockInfo)(nvmlDevice_t, int, unsigned *);
    nvmlReturn_t (*DeviceGetPersistenceMode)(nvmlDevice_t, int *);
    nvmlReturn_t (*DeviceSetPersistenceMode)(nvmlDevice_t, int);
    nvmlReturn_t (*DeviceSetMemoryLockedClocks)(nvmlDevice_t, unsigned, unsigned);
    nvmlReturn_t (*DeviceResetMemoryLockedClocks)(nvmlDevice_t);
} nvml;

#define SYMBOL(fn) { "nvml" #fn, (void **)&nvml.fn }

static const struct {
    const char *name;
    void **slot;
} symbols[] = {
    SYMBOL(Init_v2),
    SYMBOL(Shutdown),
    SYMBOL(ErrorString),
    SYMBOL(DeviceGetCount_v2),
    SYMBOL(DeviceGetHandleByIndex_v2),
    SYMBOL(DeviceGetName),
    SYMBOL(DeviceGetSupportedMemoryClocks),
    SYMBOL(DeviceGetSupportedGraphicsClocks),
    SYMBOL(DeviceGetClockInfo),
    SYMBOL(DeviceGetPersistenceMode),
    SYMBOL(DeviceSetPersistenceMode),
    SYMBOL(DeviceSetMemoryLockedClocks),
    SYMBOL(DeviceResetMemoryLockedClocks),
};

/*
 * Where to find the library. A bare name goes through the system linker
 * cache; since nvflux is setuid, the dynamic linker ignores LD_LIBRARY_PATH
 * and friends, so only root-controlled directories are searched. Absolute
 * paths are checked before loading.
 */
static const char *const library_paths[] = {
#ifdef NVML_LIB
    NVML_LIB,
#else
    "libnvidia-ml.so.1",
    "/run/opengl-driver/lib/libnvidia-ml.so.1", /* NixOS */
#endif
};

static void *library;
static int initialized;

static int is_root_owned(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
           !(st.st_mode & (S_IWGRP | S_IWOTH));
}

static void unload(void)
{
    if (library)
        dlclose(library);
    library = NULL;
    memset(&nvml, 0, sizeof(nvml));
}

int gpu_init(void)
{
    int untrusted = 0;
    for (size_t i = 0; i < sizeof(library_paths) / sizeof(*library_paths) && !library; i++) {
        const char *path = library_paths[i];
        if (path[0] == '/' && access(path, F_OK) == 0 && !is_root_owned(path)) {
            untrusted = 1;
            continue;
        }
        library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!library)
        return untrusted ? UNTRUSTED_LIBRARY : NVML_ERROR_LIBRARY_NOT_FOUND;

    for (size_t i = 0; i < sizeof(symbols) / sizeof(*symbols); i++) {
        if (!(*symbols[i].slot = dlsym(library, symbols[i].name))) {
            unload();
            return NVML_ERROR_FUNCTION_NOT_FOUND;
        }
    }

    nvmlReturn_t rc = nvml.Init_v2();
    if (rc != NVML_SUCCESS)
        return rc; /* keep the library loaded so gpu_strerror() can use it */
    initialized = 1;
    return 0;
}

void gpu_shutdown(void)
{
    if (initialized)
        nvml.Shutdown();
    initialized = 0;
    unload();
}

const char *gpu_strerror(int err)
{
    switch (err) {
    case NVML_ERROR_NOT_SUPPORTED:
        return "not supported by this GPU or driver";
    case NVML_ERROR_NO_PERMISSION:
        return "permission denied";
    case NVML_ERROR_DRIVER_NOT_LOADED:
        return "the NVIDIA driver is not loaded";
    case NVML_ERROR_LIBRARY_NOT_FOUND:
        return "libnvidia-ml.so.1 not found; is the NVIDIA driver installed?";
    case NVML_ERROR_FUNCTION_NOT_FOUND:
        return "the installed NVIDIA driver is too old";
    case UNTRUSTED_LIBRARY:
        return "refusing to load libnvidia-ml.so.1: it is not owned by root "
               "or can be modified by other users";
    }
    return nvml.ErrorString ? nvml.ErrorString(err) : "unknown error";
}

int gpu_count(unsigned *count)
{
    return nvml.DeviceGetCount_v2(count);
}

int gpu_get(unsigned index, struct gpu *gpu)
{
    nvmlDevice_t handle;
    nvmlReturn_t rc = nvml.DeviceGetHandleByIndex_v2(index, &handle);
    if (rc != NVML_SUCCESS)
        return rc;

    gpu->index = index;
    gpu->handle = handle;
    if (nvml.DeviceGetName(handle, gpu->name, sizeof(gpu->name)) != NVML_SUCCESS)
        snprintf(gpu->name, sizeof(gpu->name), "unknown GPU");
    return 0;
}

static int cmp_desc(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return (x < y) - (x > y);
}

int gpu_mem_clocks(const struct gpu *gpu, unsigned *clocks, unsigned *count)
{
    nvmlReturn_t rc = nvml.DeviceGetSupportedMemoryClocks(gpu->handle, count, clocks);
    if (rc != NVML_SUCCESS)
        return rc;
    if (*count == 0)
        return 0;

    qsort(clocks, *count, sizeof(*clocks), cmp_desc);
    unsigned n = 1;
    for (unsigned i = 1; i < *count; i++)
        if (clocks[i] != clocks[n - 1])
            clocks[n++] = clocks[i];
    *count = n;
    return 0;
}

int gpu_max_core_clock(const struct gpu *gpu, unsigned mem_mhz, unsigned *mhz)
{
    unsigned clocks[1024], count = sizeof(clocks) / sizeof(*clocks);
    nvmlReturn_t rc = nvml.DeviceGetSupportedGraphicsClocks(gpu->handle, mem_mhz, &count, clocks);
    if (rc != NVML_SUCCESS)
        return rc;

    *mhz = 0;
    for (unsigned i = 0; i < count; i++)
        if (clocks[i] > *mhz)
            *mhz = clocks[i];
    return 0;
}

int gpu_current_clock(const struct gpu *gpu, enum gpu_clock which, unsigned *mhz)
{
    int type = which == GPU_CLOCK_MEM ? NVML_CLOCK_MEM : NVML_CLOCK_GRAPHICS;
    return nvml.DeviceGetClockInfo(gpu->handle, type, mhz);
}

int gpu_lock_mem(const struct gpu *gpu, unsigned mhz)
{
    return nvml.DeviceSetMemoryLockedClocks(gpu->handle, mhz, mhz);
}

int gpu_unlock_mem(const struct gpu *gpu)
{
    return nvml.DeviceResetMemoryLockedClocks(gpu->handle);
}

int gpu_keep_initialized(const struct gpu *gpu)
{
    int mode;
    nvmlReturn_t rc = nvml.DeviceGetPersistenceMode(gpu->handle, &mode);
    if (rc == NVML_SUCCESS && mode == NVML_FEATURE_ENABLED)
        return 0; /* already on, e.g. through nvidia-persistenced */
    return nvml.DeviceSetPersistenceMode(gpu->handle, NVML_FEATURE_ENABLED);
}
