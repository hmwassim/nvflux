#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *gpu_arch_name(unsigned arch)
{
    switch (arch) {
    case 2:  return "Kepler";
    case 3:  return "Maxwell";
    case 4:  return "Pascal";
    case 5:  return "Volta";
    case 6:  return "Turing";
    case 7:  return "Ampere";
    case 8:  return "Ada";
    case 9:  return "Hopper";
    case 10: return "Blackwell";
    case 13: return "Rubin";
    }
    return "unknown";
}

void gpu_sysfs_bus_id(const char *nvml_id, char *out, unsigned len)
{
    unsigned domain, bus, dev, fn;
    if (sscanf(nvml_id, "%x:%x:%x.%x", &domain, &bus, &dev, &fn) == 4)
        snprintf(out, len, "%04x:%02x:%02x.%x", domain, bus, dev, fn);
    else
        snprintf(out, len, "%s", nvml_id);
}

static int cmp_desc(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return (x < y) - (x > y);
}

/* Sorts highest first and removes duplicates; returns the new count. */
static unsigned sort_unique(unsigned *v, unsigned n)
{
    if (n == 0)
        return 0;
    qsort(v, n, sizeof(*v), cmp_desc);
    unsigned out = 1;
    for (unsigned i = 1; i < n; i++)
        if (v[i] != v[out - 1])
            v[out++] = v[i];
    return out;
}

/* Calls an NVML list function, growing the buffer until the list fits. */
static int read_list(nvml_dev dev, int gfx, unsigned mem, unsigned **out, unsigned *n)
{
    unsigned cap = 128;
    for (;;) {
        unsigned *buf = malloc(cap * sizeof(*buf));
        if (!buf)
            return NVML_ERROR_UNKNOWN;
        unsigned count = cap;
        int rc = gfx ? nvml_gfx_clocks(dev, mem, &count, buf) : nvml_mem_clocks(dev, &count, buf);
        if (rc == NVML_ERROR_INSUFFICIENT_SIZE && count > cap && count < 65536) {
            free(buf);
            cap = count;
            continue;
        }
        if (rc != NVML_SUCCESS || count > cap) {
            free(buf);
            return rc != NVML_SUCCESS ? rc : NVML_ERROR_UNKNOWN;
        }
        *out = buf;
        *n = sort_unique(buf, count);
        return NVML_SUCCESS;
    }
}

/* NVIDIA documents graphics locks for Volta and newer and memory locks for
 * Ampere and newer; Hopper locks memory through a different ("deferred") call
 * that nvflux doesn't use. An unknown architecture is allowed through: the
 * driver then answers NOT_SUPPORTED if it can't, which nvflux handles. */
static void set_lock_support(struct gpu *g)
{
    unsigned a = g->arch;
    int known = a != NVML_ARCH_UNKNOWN && a != 0;
    g->caps.can_lock_gfx = !known || a >= NVML_ARCH_VOLTA;
    g->caps.can_lock_mem = !known || (a >= NVML_ARCH_AMPERE && a != NVML_ARCH_HOPPER);
}

int gpu_open(unsigned index, struct gpu *g)
{
    memset(g, 0, sizeof(*g));
    g->index = index;
    g->arch = NVML_ARCH_UNKNOWN;

    int rc = nvml_handle(index, &g->dev);
    if (rc != NVML_SUCCESS)
        return rc;
    if (nvml_name(g->dev, g->name, sizeof(g->name)) != NVML_SUCCESS)
        snprintf(g->name, sizeof(g->name), "unknown GPU");
    if ((rc = nvml_uuid(g->dev, g->uuid, sizeof(g->uuid))) != NVML_SUCCESS)
        return rc;
    char raw[NVML_BUSID_LEN] = "";
    if (nvml_bus_id(g->dev, raw, sizeof(raw)) == NVML_SUCCESS)
        gpu_sysfs_bus_id(raw, g->bus_id, sizeof(g->bus_id));
    if (nvml_arch(g->dev, &g->arch) != NVML_SUCCESS)
        g->arch = NVML_ARCH_UNKNOWN;
    set_lock_support(g);

    if ((rc = read_list(g->dev, 0, 0, &g->mem_mhz, &g->nmem)) != NVML_SUCCESS) {
        gpu_close(g);
        return rc;
    }
    g->gfx = calloc(g->nmem ? g->nmem : 1, sizeof(*g->gfx));
    if (!g->gfx) {
        gpu_close(g);
        return NVML_ERROR_UNKNOWN;
    }
    for (unsigned i = 0; i < g->nmem; i++) {
        unsigned *list = NULL, n = 0;
        /* A missing table just means "unknown"; policy handles that. */
        if (read_list(g->dev, 1, g->mem_mhz[i], &list, &n) == NVML_SUCCESS) {
            g->gfx[i].mhz = list;
            g->gfx[i].n = n;
        }
    }

    g->caps.mem.mhz = g->mem_mhz;
    g->caps.mem.n = g->nmem;
    g->caps.gfx = g->gfx;
    return NVML_SUCCESS;
}

void gpu_close(struct gpu *g)
{
    if (g->gfx)
        for (unsigned i = 0; i < g->nmem; i++)
            free((void *)g->gfx[i].mhz);
    free(g->gfx);
    free(g->mem_mhz);
    g->gfx = NULL;
    g->mem_mhz = NULL;
    g->nmem = 0;
}
