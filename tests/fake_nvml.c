/*
 * A stand-in for libnvidia-ml.so.1 used by the tests. It implements just the
 * functions nvflux loads and simulates GPUs described in FAKE_DIR/config:
 *
 *   gpus <n>              number of GPUs (default 1)
 *   clocks <mhz>...       supported memory clocks
 *   cap <mem> <core>      highest core clock allowed at memory clock <mem>
 *   nocaps                core clock limits can't be queried
 *   unsupported <index>   memory locking fails with NOT_SUPPORTED on that GPU
 *   nopersist             enabling persistence mode fails
 *
 *
 * The defaults mirror an RTX 3060: memory 7501/7301/5001/810/405 MHz, with the
 * core limited to 420 MHz at 405 MHz memory and 2100 MHz otherwise.
 *
 * Locked clocks are kept in FAKE_DIR/mem<index> so they survive between runs
 * like real driver state, and every change is appended to FAKE_DIR/log.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { OK = 0, NOT_SUPPORTED = 3, NO_PERMISSION = 4, INVALID_ARG = 2, INSUFFICIENT_SIZE = 7 };

static struct {
    unsigned gpus;
    unsigned clocks[16], nclocks;
    unsigned cap_mem[8], cap_core[8], ncaps;
    int nocaps;
    int unsupported;
    int nopersist;
    int persistence[8];
} fake = {
    .gpus = 1,
    .clocks = { 7501, 405, 7301, 5001, 810, 405 }, /* unsorted, with a duplicate */
    .nclocks = 6,
    .cap_mem = { 405 },
    .cap_core = { 420 },
    .ncaps = 1,
    .unsupported = -1,
};

static void log_line(const char *what, unsigned gpu, unsigned value)
{
    FILE *f = fopen(FAKE_DIR "/log", "a");
    if (!f)
        return;
    fprintf(f, "%s %u %u\n", what, gpu, value);
    fclose(f);
}

static unsigned read_mem(unsigned gpu)
{
    char path[512];
    unsigned mhz = 0;
    snprintf(path, sizeof(path), FAKE_DIR "/mem%u", gpu);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%u", &mhz) != 1)
            mhz = 0;
        fclose(f);
    }
    return mhz;
}

static void write_mem(unsigned gpu, unsigned mhz)
{
    char path[512];
    snprintf(path, sizeof(path), FAKE_DIR "/mem%u", gpu);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%u\n", mhz);
        fclose(f);
    }
}

/* Device handles are just index + 1 disguised as pointers. */
static unsigned gpu_of(void *device) { return (unsigned)(uintptr_t)device - 1; }

int nvmlInit_v2(void)
{
    FILE *f = fopen(FAKE_DIR "/config", "r");
    char key[32];
    if (!f)
        return OK;
    while (fscanf(f, "%31s", key) == 1) {
        if (!strcmp(key, "gpus")) {
            if (fscanf(f, "%u", &fake.gpus) != 1)
                break;
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
        } else if (!strcmp(key, "nocaps")) {
            fake.nocaps = 1;
        } else if (!strcmp(key, "unsupported")) {
            if (fscanf(f, "%d", &fake.unsupported) != 1)
                break;
        } else if (!strcmp(key, "nopersist")) {
            fake.nopersist = 1;
        }
    }
    fclose(f);
    return OK;
}

int nvmlShutdown(void) { return OK; }

const char *nvmlErrorString(int rc)
{
    return rc == INVALID_ARG ? "Invalid Argument" : "Fake Error";
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

int nvmlDeviceGetSupportedGraphicsClocks(void *device, unsigned mem, unsigned *count,
                                         unsigned *clocks)
{
    (void)device;
    if (fake.nocaps)
        return NOT_SUPPORTED;

    unsigned top = 2100;
    for (unsigned i = 0; i < fake.ncaps; i++)
        if (fake.cap_mem[i] == mem)
            top = fake.cap_core[i];

    unsigned n = 0;
    for (unsigned mhz = 405; mhz <= top && n < *count; mhz += 15)
        clocks[n++] = mhz;
    *count = n;
    return OK;
}

int nvmlDeviceGetClockInfo(void *device, int type, unsigned *mhz)
{
    unsigned locked = read_mem(gpu_of(device));
    *mhz = type == 2 ? (locked ? locked : 810) : 720;
    return OK;
}

int nvmlDeviceGetPersistenceMode(void *device, int *mode)
{
    *mode = fake.persistence[gpu_of(device) % 8];
    return OK;
}

int nvmlDeviceSetPersistenceMode(void *device, int mode)
{
    if (geteuid() != 0 || fake.nopersist)
        return NO_PERMISSION;
    fake.persistence[gpu_of(device) % 8] = mode;
    log_line("persistence", gpu_of(device), (unsigned)mode);
    return OK;
}

int nvmlDeviceSetMemoryLockedClocks(void *device, unsigned min, unsigned max)
{
    unsigned gpu = gpu_of(device);
    if (geteuid() != 0)
        return NO_PERMISSION;
    if ((int)gpu == fake.unsupported)
        return NOT_SUPPORTED;
    if (min != max)
        return INVALID_ARG;
    write_mem(gpu, min);
    log_line("lock", gpu, min);
    return OK;
}

int nvmlDeviceResetMemoryLockedClocks(void *device)
{
    unsigned gpu = gpu_of(device);
    if (geteuid() != 0)
        return NO_PERMISSION;
    write_mem(gpu, 0);
    log_line("unlock", gpu, 0);
    return OK;
}
