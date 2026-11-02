/*
 * A stand-in for libnvidia-ml.so.1 used by the tests. It implements the
 * functions nvflux loads and simulates GPUs described in FAKE_DIR/config:
 *
 *   gpus <n>                number of GPUs (default 1)
 *   arch <gpu> <n>          NVML architecture of one GPU (default 7, Ampere)
 *   clocks <mhz>...         supported memory clocks (all GPUs)
 *   cap <mem> <core>        highest graphics clock allowed at memory clock <mem>
 *   nocaps                  graphics clock tables can't be queried
 *   bigtable                graphics tables with 400 entries
 *   nomem <gpu>             memory locks fail with NOT_SUPPORTED on that GPU
 *   nogfx <gpu>             graphics locks fail with NOT_SUPPORTED on that GPU
 *   nosymbolic              symbolic graphics values are rejected (old driver)
 *   failgfx <gpu>           graphics locks fail with an unknown error
 *   memshown <gpu> <mhz>    report this memory clock whatever is locked
 *                           (a display needing more bandwidth)
 *   nopersist               enabling persistence mode fails
 *
 * The defaults mirror the RTX 3060 measured on the reference machine:
 * memory 7501/7301/5001/810/405 MHz, graphics 405-2100 in 15 MHz steps,
 * except at 405 MHz memory where graphics stops at 420.
 *
 * Like the real driver, it accepts an unsupported memory clock by rounding it
 * up to the next supported one, and rejects mixed symbolic/numeric values.
 *
 * Locks are kept in FAKE_DIR/mem<gpu> and FAKE_DIR/gfx<gpu> so they survive
 * between runs like real driver state, and every change is appended to
 * FAKE_DIR/log.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
    OK = 0, INVALID_ARG = 2, NOT_SUPPORTED = 3, NO_PERMISSION = 4,
    INSUFFICIENT_SIZE = 7, UNKNOWN = 999,
};

#define TDP 0xffffff01u
#define UNL 0xffffff02u
#define MAXG 8

static struct {
    unsigned gpus;
    unsigned arch[MAXG];
    unsigned clocks[16], nclocks;
    unsigned cap_mem[8], cap_core[8], ncaps;
    int nocaps, bigtable, nosymbolic, nopersist;
    int nomem[MAXG], nogfx[MAXG], failgfx[MAXG];
    unsigned memshown[MAXG];
    int persistence[MAXG];
} fake = {
    .gpus = 1,
    .arch = { 7, 7, 7, 7, 7, 7, 7, 7 },
    .clocks = { 7501, 405, 7301, 5001, 810, 405 }, /* unsorted, with a duplicate */
    .nclocks = 6,
    .cap_mem = { 405 },
    .cap_core = { 420 },
    .ncaps = 1,
};

static void log_line(const char *fmt, unsigned gpu, unsigned a, unsigned b)
{
    FILE *f = fopen(FAKE_DIR "/log", "a");
    if (!f)
        return;
    fprintf(f, fmt, gpu, a, b);
    fputc('\n', f);
    fclose(f);
}

static void write_file(const char *name, unsigned gpu, const char *text)
{
    char path[512];
    snprintf(path, sizeof(path), FAKE_DIR "/%s%u", name, gpu);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int read_file(const char *name, unsigned gpu, char *buf, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), FAKE_DIR "/%s%u", name, gpu);
    FILE *f = fopen(path, "r");
    buf[0] = '\0';
    if (!f)
        return -1;
    if (!fgets(buf, (int)len, f))
        buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static unsigned gpu_of(void *device) { return (unsigned)(uintptr_t)device - 1; }

int nvmlInit_v2(void)
{
    /* Recorded so tests can prove a sleeping GPU was never woken. */
    FILE *init = fopen(FAKE_DIR "/initlog", "a");
    if (init) {
        fputs("init\n", init);
        fclose(init);
    }

    FILE *f = fopen(FAKE_DIR "/config", "r");
    char key[32];
    unsigned g, v;
    if (!f)
        return OK;
    while (fscanf(f, "%31s", key) == 1) {
        if (!strcmp(key, "gpus")) {
            if (fscanf(f, "%u", &fake.gpus) != 1 || fake.gpus > MAXG)
                break;
        } else if (!strcmp(key, "arch")) {
            if (fscanf(f, "%u %u", &g, &v) != 2 || g >= MAXG)
                break;
            fake.arch[g] = v;
        } else if (!strcmp(key, "clocks")) {
            fake.nclocks = 0;
            fake.ncaps = 0; /* new clocks, so the default cap no longer applies */
            while (fake.nclocks < 16 && fscanf(f, "%u", &fake.clocks[fake.nclocks]) == 1)
                fake.nclocks++;
        } else if (!strcmp(key, "cap")) {
            if (fake.ncaps == 8 ||
                fscanf(f, "%u %u", &fake.cap_mem[fake.ncaps], &fake.cap_core[fake.ncaps]) != 2)
                break;
            fake.ncaps++;
        } else if (!strcmp(key, "memshown")) {
            if (fscanf(f, "%u %u", &g, &v) != 2 || g >= MAXG)
                break;
            fake.memshown[g] = v;
        } else if (!strcmp(key, "nomem") || !strcmp(key, "nogfx") || !strcmp(key, "failgfx")) {
            if (fscanf(f, "%u", &g) != 1 || g >= MAXG)
                break;
            (key[0] == 'n' ? (key[2] == 'm' ? fake.nomem : fake.nogfx) : fake.failgfx)[g] = 1;
        } else if (!strcmp(key, "nocaps")) {
            fake.nocaps = 1;
        } else if (!strcmp(key, "bigtable")) {
            fake.bigtable = 1;
        } else if (!strcmp(key, "nosymbolic")) {
            fake.nosymbolic = 1;
        } else if (!strcmp(key, "nopersist")) {
            fake.nopersist = 1;
        }
    }
    fclose(f);
    return OK;
}

int nvmlShutdown(void) { return OK; }
const char *nvmlErrorString(int rc) { return rc == INVALID_ARG ? "Invalid Argument" : "Fake Error"; }

int nvmlSystemGetDriverVersion(char *buf, unsigned len)
{
    snprintf(buf, len, "999.99");
    return OK;
}

int nvmlDeviceGetCount_v2(unsigned *count)
{
    *count = fake.gpus;
    return OK;
}

int nvmlDeviceGetHandleByIndex_v2(unsigned index, void **device)
{
    if (index >= fake.gpus)
        return INVALID_ARG;
    *device = (void *)(uintptr_t)(index + 1);
    return OK;
}

int nvmlDeviceGetName(void *device, char *name, unsigned len)
{
    snprintf(name, len, "Fake GPU %u", gpu_of(device));
    return OK;
}

int nvmlDeviceGetUUID(void *device, char *uuid, unsigned len)
{
    snprintf(uuid, len, "GPU-fake0000-0000-0000-0000-00000000000%u", gpu_of(device));
    return OK;
}

typedef struct {
    char bus_id_legacy[16];
    unsigned domain, bus, device, pci_device_id, pci_subsystem_id;
    char bus_id[32];
} pci_info_t;

int nvmlDeviceGetPciInfo_v3(void *device, pci_info_t *pci)
{
    memset(pci, 0, sizeof(*pci));
    snprintf(pci->bus_id, sizeof(pci->bus_id), "00000000:%02X:00.0", gpu_of(device) + 1);
    return OK;
}

int nvmlDeviceGetArchitecture(void *device, unsigned *arch)
{
    *arch = fake.arch[gpu_of(device)];
    return OK;
}

static unsigned top_for(unsigned mem)
{
    unsigned top = 2100;
    for (unsigned i = 0; i < fake.ncaps; i++)
        if (fake.cap_mem[i] == mem)
            top = fake.cap_core[i];
    return top;
}

int nvmlDeviceGetSupportedMemoryClocks(void *device, unsigned *count, unsigned *clocks)
{
    (void)device;
    if (*count < fake.nclocks) {
        *count = fake.nclocks;
        return INSUFFICIENT_SIZE;
    }
    memcpy(clocks, fake.clocks, fake.nclocks * sizeof(*clocks));
    *count = fake.nclocks;
    return OK;
}

int nvmlDeviceGetSupportedGraphicsClocks(void *device, unsigned mem, unsigned *count, unsigned *clocks)
{
    (void)device;
    if (fake.nocaps)
        return NOT_SUPPORTED;
    unsigned step = fake.bigtable ? 4 : 15, top = top_for(mem), n = 0;
    for (unsigned mhz = 405; mhz <= top; mhz += step)
        n++;
    if (*count < n) {
        *count = n;
        return INSUFFICIENT_SIZE;
    }
    n = 0;
    for (unsigned mhz = top; mhz >= 405 && mhz <= top; mhz -= step) /* highest first, like the driver */
        clocks[n++] = mhz;
    *count = n;
    return OK;
}

int nvmlDeviceGetClockInfo(void *device, int type, unsigned *mhz)
{
    unsigned g = gpu_of(device);
    char buf[64];
    if (type == 2) {
        unsigned locked = 0;
        read_file("mem", g, buf, sizeof(buf));
        sscanf(buf, "%u", &locked);
        *mhz = fake.memshown[g] ? fake.memshown[g] : locked ? locked : 405;
    } else {
        unsigned lo = 0;
        read_file("gfx", g, buf, sizeof(buf));
        *mhz = sscanf(buf, "%u", &lo) == 1 && lo ? lo : 210;
    }
    return OK;
}

int nvmlDeviceGetMaxClockInfo(void *device, int type, unsigned *mhz)
{
    (void)device;
    *mhz = type == 2 ? 7501 : 2100;
    return OK;
}

int nvmlDeviceGetPowerUsage(void *device, unsigned *mw) { (void)device; *mw = 16500; return OK; }

int nvmlDeviceGetTemperature(void *device, int sensor, unsigned *temp)
{
    (void)device; (void)sensor;
    *temp = 42;
    return OK;
}

int nvmlDeviceGetCurrentClocksEventReasons(void *device, unsigned long long *r)
{
    (void)device;
    *r = 1; /* idle */
    return OK;
}

int nvmlDeviceGetPersistenceMode(void *device, int *mode)
{
    *mode = fake.persistence[gpu_of(device)];
    return OK;
}

int nvmlDeviceSetPersistenceMode(void *device, int mode)
{
    if (geteuid() != 0 || fake.nopersist)
        return NO_PERMISSION;
    fake.persistence[gpu_of(device)] = mode;
    log_line("persistence %u %u", gpu_of(device), (unsigned)mode, 0);
    return OK;
}

int nvmlDeviceSetMemoryLockedClocks(void *device, unsigned min, unsigned max)
{
    unsigned g = gpu_of(device);
    if (geteuid() != 0)
        return NO_PERMISSION;
    if (fake.nomem[g])
        return NOT_SUPPORTED;
    if (min != max || min == 0)
        return INVALID_ARG;
    /* Like the real driver: round an unsupported clock up. */
    unsigned best = 0;
    for (unsigned i = 0; i < fake.nclocks; i++)
        if (fake.clocks[i] >= min && (best == 0 || fake.clocks[i] < best))
            best = fake.clocks[i];
    if (best == 0)
        return INVALID_ARG;
    char buf[32];
    snprintf(buf, sizeof(buf), "%u\n", best);
    write_file("mem", g, buf);
    log_line("lockmem %u %u %u", g, min, best);
    return OK;
}

int nvmlDeviceResetMemoryLockedClocks(void *device)
{
    unsigned g = gpu_of(device);
    if (geteuid() != 0)
        return NO_PERMISSION;
    if (fake.nomem[g])
        return NOT_SUPPORTED;
    write_file("mem", g, "0\n");
    log_line("resetmem %u %u %u", g, 0, 0);
    return OK;
}

int nvmlDeviceSetGpuLockedClocks(void *device, unsigned min, unsigned max)
{
    unsigned g = gpu_of(device);
    if (geteuid() != 0)
        return NO_PERMISSION;
    if (fake.nogfx[g])
        return NOT_SUPPORTED;
    if (fake.failgfx[g])
        return UNKNOWN;
    int smin = min >= 0xffffff00u, smax = max >= 0xffffff00u;
    if (smin != smax)
        return INVALID_ARG;
    if (smin && fake.nosymbolic)
        return INVALID_ARG;
    if (!smin && (min == 0 || min > max))
        return INVALID_ARG;
    char buf[64];
    if (smin) {
        snprintf(buf, sizeof(buf), "%s %s\n", min == TDP ? "tdp" : "unl", max == TDP ? "tdp" : "unl");
        log_line("lockgfx %u %u %u", g, min, max);
    } else {
        snprintf(buf, sizeof(buf), "%u %u\n", min, max);
        log_line("lockgfx %u %u %u", g, min, max);
    }
    write_file("gfx", g, buf);
    return OK;
}

int nvmlDeviceResetGpuLockedClocks(void *device)
{
    unsigned g = gpu_of(device);
    if (geteuid() != 0)
        return NO_PERMISSION;
    if (fake.nogfx[g])
        return NOT_SUPPORTED;
    write_file("gfx", g, "0\n");
    log_line("resetgfx %u %u %u", g, 0, 0);
    return OK;
}
