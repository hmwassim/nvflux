#define _GNU_SOURCE
#include "nvml.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"

typedef int ret_t;

typedef struct {
    char bus_id_legacy[16];
    unsigned domain, bus, device, pci_device_id, pci_subsystem_id;
    char bus_id[32];
} pci_info_t;

static struct {
    ret_t (*Init_v2)(void);
    ret_t (*Shutdown)(void);
    const char *(*ErrorString)(ret_t);
    ret_t (*SystemGetDriverVersion)(char *, unsigned);
    ret_t (*DeviceGetCount_v2)(unsigned *);
    ret_t (*DeviceGetHandleByIndex_v2)(unsigned, nvml_dev *);
    ret_t (*DeviceGetName)(nvml_dev, char *, unsigned);
    ret_t (*DeviceGetUUID)(nvml_dev, char *, unsigned);
    ret_t (*DeviceGetPciInfo_v3)(nvml_dev, pci_info_t *);
    ret_t (*DeviceGetClockInfo)(nvml_dev, int, unsigned *);
    ret_t (*DeviceGetMaxClockInfo)(nvml_dev, int, unsigned *);
    ret_t (*DeviceGetSupportedMemoryClocks)(nvml_dev, unsigned *, unsigned *);
    ret_t (*DeviceGetSupportedGraphicsClocks)(nvml_dev, unsigned, unsigned *, unsigned *);
    /* optional */
    ret_t (*DeviceGetArchitecture)(nvml_dev, unsigned *);
    ret_t (*DeviceGetPowerUsage)(nvml_dev, unsigned *);
    ret_t (*DeviceGetTemperature)(nvml_dev, int, unsigned *);
    ret_t (*DeviceGetCurrentClocksEventReasons)(nvml_dev, unsigned long long *);
    ret_t (*DeviceGetPersistenceMode)(nvml_dev, int *);
    ret_t (*DeviceSetPersistenceMode)(nvml_dev, int);
    ret_t (*DeviceSetMemoryLockedClocks)(nvml_dev, unsigned, unsigned);
    ret_t (*DeviceResetMemoryLockedClocks)(nvml_dev);
    ret_t (*DeviceSetGpuLockedClocks)(nvml_dev, unsigned, unsigned);
    ret_t (*DeviceResetGpuLockedClocks)(nvml_dev);
} fn;

#define SYMBOL(f, required) { "nvml" #f, (void **)&fn.f, required }

static const struct {
    const char *name;
    void **slot;
    int required;
} symbols[] = {
    SYMBOL(Init_v2, 1),
    SYMBOL(Shutdown, 1),
    SYMBOL(ErrorString, 1),
    SYMBOL(SystemGetDriverVersion, 1),
    SYMBOL(DeviceGetCount_v2, 1),
    SYMBOL(DeviceGetHandleByIndex_v2, 1),
    SYMBOL(DeviceGetName, 1),
    SYMBOL(DeviceGetUUID, 1),
    SYMBOL(DeviceGetPciInfo_v3, 1),
    SYMBOL(DeviceGetClockInfo, 1),
    SYMBOL(DeviceGetMaxClockInfo, 1),
    SYMBOL(DeviceGetSupportedMemoryClocks, 1),
    SYMBOL(DeviceGetSupportedGraphicsClocks, 1),
    SYMBOL(DeviceGetArchitecture, 0),
    SYMBOL(DeviceGetPowerUsage, 0),
    SYMBOL(DeviceGetTemperature, 0),
    SYMBOL(DeviceGetCurrentClocksEventReasons, 0),
    SYMBOL(DeviceGetPersistenceMode, 0),
    SYMBOL(DeviceSetPersistenceMode, 0),
    SYMBOL(DeviceSetMemoryLockedClocks, 0),
    SYMBOL(DeviceResetMemoryLockedClocks, 0),
    SYMBOL(DeviceSetGpuLockedClocks, 0),
    SYMBOL(DeviceResetGpuLockedClocks, 0),
};

/*
 * Where to find the library. A bare name goes through the system linker
 * cache; because nvflux is setuid, the dynamic linker ignores LD_LIBRARY_PATH
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
    memset(&fn, 0, sizeof(fn));
}

int nvml_open(void)
{
    int untrusted = 0;
    for (size_t i = 0; i < ARRAY_LEN(library_paths) && !library; i++) {
        const char *path = library_paths[i];
        /* Only root code needs the check: anyone else could run that
         * library's code anyway. */
        if (path[0] == '/' && geteuid() == 0 && access(path, F_OK) == 0 && !is_root_owned(path)) {
            untrusted = 1;
            continue;
        }
        library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!library)
        return untrusted ? NVML_UNTRUSTED_LIBRARY : NVML_ERROR_LIBRARY_NOT_FOUND;

    for (size_t i = 0; i < ARRAY_LEN(symbols); i++) {
        *symbols[i].slot = dlsym(library, symbols[i].name);
        if (!*symbols[i].slot && symbols[i].required) {
            unload();
            return NVML_ERROR_FUNCTION_NOT_FOUND;
        }
    }

    int rc = fn.Init_v2();
    if (rc != NVML_SUCCESS)
        return rc; /* keep the library loaded so nvml_strerror() works */
    initialized = 1;
    return NVML_SUCCESS;
}

void nvml_close(void)
{
    if (initialized)
        fn.Shutdown();
    initialized = 0;
    unload();
}

const char *nvml_strerror(int rc)
{
    switch (rc) {
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
    case NVML_ERROR_GPU_IS_LOST:
        return "the GPU stopped responding";
    case NVML_UNTRUSTED_LIBRARY:
        return "refusing to load libnvidia-ml.so.1: it is not owned by root "
               "or other users can modify it";
    }
    return fn.ErrorString ? fn.ErrorString(rc) : "unknown error";
}

#define OPTIONAL(f, ...) (fn.f ? fn.f(__VA_ARGS__) : NVML_ERROR_FUNCTION_NOT_FOUND)

int nvml_driver_version(char *buf, unsigned len) { return fn.SystemGetDriverVersion(buf, len); }
int nvml_count(unsigned *count) { return fn.DeviceGetCount_v2(count); }
int nvml_handle(unsigned index, nvml_dev *dev) { return fn.DeviceGetHandleByIndex_v2(index, dev); }
int nvml_name(nvml_dev dev, char *buf, unsigned len) { return fn.DeviceGetName(dev, buf, len); }
int nvml_uuid(nvml_dev dev, char *buf, unsigned len) { return fn.DeviceGetUUID(dev, buf, len); }

int nvml_bus_id(nvml_dev dev, char *buf, unsigned len)
{
    pci_info_t pci;
    memset(&pci, 0, sizeof(pci));
    int rc = fn.DeviceGetPciInfo_v3(dev, &pci);
    if (rc == NVML_SUCCESS)
        snprintf(buf, len, "%.*s", (int)sizeof(pci.bus_id), pci.bus_id);
    return rc;
}

int nvml_clock(nvml_dev dev, int type, unsigned *mhz) { return fn.DeviceGetClockInfo(dev, type, mhz); }
int nvml_max_clock(nvml_dev dev, int type, unsigned *mhz) { return fn.DeviceGetMaxClockInfo(dev, type, mhz); }

int nvml_mem_clocks(nvml_dev dev, unsigned *count, unsigned *mhz)
{
    return fn.DeviceGetSupportedMemoryClocks(dev, count, mhz);
}

int nvml_gfx_clocks(nvml_dev dev, unsigned mem_mhz, unsigned *count, unsigned *mhz)
{
    return fn.DeviceGetSupportedGraphicsClocks(dev, mem_mhz, count, mhz);
}

int nvml_arch(nvml_dev dev, unsigned *arch) { return OPTIONAL(DeviceGetArchitecture, dev, arch); }
int nvml_power_mw(nvml_dev dev, unsigned *mw) { return OPTIONAL(DeviceGetPowerUsage, dev, mw); }
int nvml_temperature(nvml_dev dev, unsigned *c) { return OPTIONAL(DeviceGetTemperature, dev, 0, c); }

int nvml_event_reasons(nvml_dev dev, unsigned long long *reasons)
{
    return OPTIONAL(DeviceGetCurrentClocksEventReasons, dev, reasons);
}

int nvml_persistence(nvml_dev dev, int *enabled) { return OPTIONAL(DeviceGetPersistenceMode, dev, enabled); }
int nvml_set_persistence(nvml_dev dev, int enabled) { return OPTIONAL(DeviceSetPersistenceMode, dev, enabled); }

int nvml_set_mem_locked(nvml_dev dev, unsigned min_mhz, unsigned max_mhz)
{
    return OPTIONAL(DeviceSetMemoryLockedClocks, dev, min_mhz, max_mhz);
}

int nvml_reset_mem_locked(nvml_dev dev) { return OPTIONAL(DeviceResetMemoryLockedClocks, dev); }

int nvml_set_gpu_locked(nvml_dev dev, unsigned min_mhz, unsigned max_mhz)
{
    return OPTIONAL(DeviceSetGpuLockedClocks, dev, min_mhz, max_mhz);
}

int nvml_reset_gpu_locked(nvml_dev dev) { return OPTIONAL(DeviceResetGpuLockedClocks, dev); }
