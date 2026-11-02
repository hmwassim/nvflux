#ifndef NVFLUX_NVML_H
#define NVFLUX_NVML_H

/*
 * The only part of nvflux that knows about NVML, the library the NVIDIA driver
 * ships for querying and controlling GPUs (nvidia-smi is built on it).
 *
 * The library is loaded at runtime, so nvflux builds without NVIDIA headers.
 * The few declarations we need are copied here; NVML keeps them stable across
 * driver releases (changed functions get a new _vN name instead).
 *
 * Every function returns an NVML status code (0 = success).
 */

typedef struct nvmlDevice_st *nvml_dev;

enum {
    NVML_SUCCESS                 = 0,
    NVML_ERROR_UNINITIALIZED     = 1,
    NVML_ERROR_INVALID_ARGUMENT  = 2,
    NVML_ERROR_NOT_SUPPORTED     = 3,
    NVML_ERROR_NO_PERMISSION     = 4,
    NVML_ERROR_NOT_FOUND         = 6,
    NVML_ERROR_INSUFFICIENT_SIZE = 7,
    NVML_ERROR_DRIVER_NOT_LOADED = 9,
    NVML_ERROR_LIBRARY_NOT_FOUND = 12,
    NVML_ERROR_FUNCTION_NOT_FOUND = 13,
    NVML_ERROR_GPU_IS_LOST       = 15,
    NVML_ERROR_UNKNOWN           = 999,

    NVML_UNTRUSTED_LIBRARY = 1000, /* ours, outside NVML's range */
};

/* Symbolic values accepted by nvml_set_gpu_locked (never mixed with MHz). */
#define NVML_CLOCK_LIMIT_TDP       0xffffff01u /* the card's base clock */
#define NVML_CLOCK_LIMIT_UNLIMITED 0xffffff02u

enum { NVML_CLOCK_GRAPHICS = 0, NVML_CLOCK_MEM = 2 };

enum {
    NVML_ARCH_VOLTA  = 5,
    NVML_ARCH_TURING = 6,
    NVML_ARCH_AMPERE = 7,
    NVML_ARCH_ADA    = 8,
    NVML_ARCH_HOPPER = 9,
    NVML_ARCH_UNKNOWN = 0xffffffffu,
};

#define NVML_NAME_LEN   96
#define NVML_UUID_LEN   96
#define NVML_BUSID_LEN  32
#define NVML_DRIVER_LEN 80

/* Loads and initialises the library. Returns NVML_UNTRUSTED_LIBRARY if the
 * only library found could be modified by non-root users. */
int nvml_open(void);
void nvml_close(void);
const char *nvml_strerror(int rc);

int nvml_driver_version(char *buf, unsigned len);
int nvml_count(unsigned *count);
int nvml_handle(unsigned index, nvml_dev *dev);
int nvml_name(nvml_dev dev, char *buf, unsigned len);
int nvml_uuid(nvml_dev dev, char *buf, unsigned len);
int nvml_bus_id(nvml_dev dev, char *buf, unsigned len);

int nvml_clock(nvml_dev dev, int type, unsigned *mhz);
int nvml_max_clock(nvml_dev dev, int type, unsigned *mhz);
int nvml_mem_clocks(nvml_dev dev, unsigned *count, unsigned *mhz);
int nvml_gfx_clocks(nvml_dev dev, unsigned mem_mhz, unsigned *count, unsigned *mhz);

/* Optional: return NVML_ERROR_FUNCTION_NOT_FOUND if the driver lacks them. */
int nvml_arch(nvml_dev dev, unsigned *arch);
int nvml_power_mw(nvml_dev dev, unsigned *mw);
int nvml_temperature(nvml_dev dev, unsigned *celsius);
int nvml_event_reasons(nvml_dev dev, unsigned long long *reasons);
int nvml_persistence(nvml_dev dev, int *enabled);

/* The only functions that change anything. Nothing else is ever resolved
 * (tests/check_symbols.sh enforces this). */
int nvml_set_persistence(nvml_dev dev, int enabled);
int nvml_set_mem_locked(nvml_dev dev, unsigned min_mhz, unsigned max_mhz);
int nvml_reset_mem_locked(nvml_dev dev);
int nvml_set_gpu_locked(nvml_dev dev, unsigned min_mhz, unsigned max_mhz);
int nvml_reset_gpu_locked(nvml_dev dev);

/* Clock event reasons worth explaining to users. */
#define NVML_REASON_IDLE          0x1ULL
#define NVML_REASON_SW_POWER_CAP  0x4ULL
#define NVML_REASON_HW_SLOWDOWN   0x8ULL
#define NVML_REASON_SW_THERMAL    0x20ULL
#define NVML_REASON_HW_THERMAL    0x40ULL
#define NVML_REASON_HW_POWER_BRAKE 0x80ULL
#define NVML_REASON_DISPLAY_CLOCK 0x100ULL

#endif
