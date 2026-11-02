#define _GNU_SOURCE
#include "status.h"

#include <stdio.h>
#include <string.h>

#include "common.h"
#include "gpu.h"
#include "nvml.h"
#include "out.h"
#include "policy.h"
#include "state.h"
#include "sysfs.h"

#define MAX_GPUS 16

struct view {
    struct state saved, temp;
    int has_temp;
    const struct state *effective;
};

static void load_view(struct view *v)
{
    if (state_load(state_saved_path(), &v->saved, NULL) != STATE_LOADED)
        state_init(&v->saved);
    v->has_temp = state_load(state_temp_path(), &v->temp, NULL) == STATE_LOADED;
    v->effective = v->has_temp ? &v->temp : &v->saved;
}

static const char *state_label(const struct state *s)
{
    if (s->ngpu)
        return "per-gpu";
    return s->all.mode == MODE_CUSTOM ? "locked" : mode_name(s->all.mode);
}

/* If any NVIDIA GPU is powered down, NVML isn't touched at all: initialising
 * it would wake the GPU and drain a laptop battery. */
static int any_asleep(struct pci_gpu *pci, int *n)
{
    *n = sysfs_nvidia_gpus(pci, SYSFS_MAX_GPUS);
    for (int i = 0; i < *n; i++)
        if (pci[i].asleep)
            return 1;
    return 0;
}

/* "locked 810", "locked 945-2100", "capped at base", "driver" */
static void target_text(const struct target *t, int rc, int mem, char *buf, size_t len)
{
    if (rc != POLICY_OK) {
        snprintf(buf, len, "can't apply; see 'nvflux doctor'");
    } else if (mem) {
        if (t->mem == MEM_LOCK)
            snprintf(buf, len, "locked %u", t->mem_mhz);
        else
            snprintf(buf, len, "driver");
    } else if (t->gfx == GFX_RANGE && t->gfx_min == t->gfx_max) {
        snprintf(buf, len, "locked %u", t->gfx_min);
    } else if (t->gfx == GFX_RANGE) {
        snprintf(buf, len, "locked %u-%u", t->gfx_min, t->gfx_max);
    } else if (t->gfx == GFX_CAP_BASE) {
        snprintf(buf, len, "capped at base");
    } else {
        snprintf(buf, len, "driver");
    }
}

static void limits_text(unsigned long long r, char *buf, size_t len)
{
    buf[0] = '\0';
    if (r & (NVML_REASON_HW_THERMAL | NVML_REASON_SW_THERMAL))
        snprintf(buf + strlen(buf), len - strlen(buf), "%sheat", buf[0] ? ", " : "");
    if (r & (NVML_REASON_SW_POWER_CAP | NVML_REASON_HW_POWER_BRAKE))
        snprintf(buf + strlen(buf), len - strlen(buf), "%spower", buf[0] ? ", " : "");
    if (r & NVML_REASON_DISPLAY_CLOCK)
        snprintf(buf + strlen(buf), len - strlen(buf), "%sdisplay", buf[0] ? ", " : "");
}

static int open_all(struct gpu *gpus, unsigned *count)
{
    int rc = nvml_open();
    unsigned n = 0;
    if (rc == NVML_SUCCESS)
        rc = nvml_count(&n);
    if (rc != NVML_SUCCESS) {
        out_error("%s", nvml_strerror(rc));
        nvml_close();
        return -1;
    }
    *count = 0;
    for (unsigned i = 0; i < n && *count < MAX_GPUS; i++)
        if (gpu_open(i, &gpus[*count]) == NVML_SUCCESS)
            (*count)++;
    return 0;
}

static void close_all(struct gpu *gpus, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        gpu_close(&gpus[i]);
    nvml_close();
}

/* ------------------------------------------------------------------------ */

static void json_gpu(const struct gpu *g, const struct request *req, const struct target *t, int prc,
                     unsigned mem, unsigned gfx, int have_power, unsigned mw, int have_temp, unsigned temp,
                     unsigned long long reasons, int first)
{
    printf("%s{\"index\":%u,\"name\":", first ? "" : ",", g->index);
    out_json_str(stdout, g->name);
    printf(",\"uuid\":");
    out_json_str(stdout, g->uuid);
    printf(",\"bus_id\":");
    out_json_str(stdout, g->bus_id);
    printf(",\"asleep\":false,\"mode\":\"%s\",\"applicable\":%s", mode_name(req->mode),
           prc == POLICY_OK ? "true" : "false");
    printf(",\"memory\":{\"mhz\":%u,\"target\":", mem);
    if (prc == POLICY_OK && t->mem == MEM_LOCK)
        printf("{\"min\":%u,\"max\":%u}", t->mem_mhz, t->mem_mhz);
    else
        printf("null");
    printf("},\"graphics\":{\"mhz\":%u,\"target\":", gfx);
    if (prc == POLICY_OK && t->gfx == GFX_RANGE)
        printf("{\"min\":%u,\"max\":%u}", t->gfx_min, t->gfx_max);
    else if (prc == POLICY_OK && t->gfx == GFX_CAP_BASE)
        printf("{\"max\":\"base\"}");
    else
        printf("null");
    printf("}");
    if (have_power)
        printf(",\"power_w\":%.1f", mw / 1000.0);
    if (have_temp)
        printf(",\"temperature_c\":%u", temp);
    printf(",\"limited_by\":[");
    const char *sep = "";
    if (reasons & (NVML_REASON_HW_THERMAL | NVML_REASON_SW_THERMAL)) { printf("%s\"heat\"", sep); sep = ","; }
    if (reasons & (NVML_REASON_SW_POWER_CAP | NVML_REASON_HW_POWER_BRAKE)) { printf("%s\"power\"", sep); sep = ","; }
    if (reasons & NVML_REASON_DISPLAY_CLOCK) printf("%s\"display\"", sep);
    printf("]}");
}

/*
 * mode: balanced
 * GPU 0: NVIDIA GeForce RTX 3060
 *   memory     810 MHz  (locked 810)
 *   graphics   945 MHz  (locked 945-2100)
 *   power      14 W, 43 C
 */
int status_show(int json)
{
    struct view v;
    load_view(&v);

    struct pci_gpu pci[SYSFS_MAX_GPUS];
    int npci;
    int sleeping = any_asleep(pci, &npci);

    if (json)
        printf("{\"schema\":1,\"version\":\"%s\",\"mode\":\"%s\",\"temporary\":%s,\"saved_mode\":\"%s\"",
               VERSION, state_label(v.effective), v.has_temp ? "true" : "false", state_label(&v.saved));
    else if (v.has_temp)
        printf("mode: %s (temporary, saved: %s)\n", state_label(v.effective), state_label(&v.saved));
    else
        printf("mode: %s\n", state_label(v.effective));

    if (sleeping) {
        if (json) {
            printf(",\"gpus\":[");
            for (int i = 0; i < npci; i++) {
                printf("%s{\"bus_id\":", i ? "," : "");
                out_json_str(stdout, pci[i].bus_id);
                printf(",\"asleep\":%s}", pci[i].asleep ? "true" : "false");
            }
            printf("]}\n");
        } else {
            for (int i = 0; i < npci; i++)
                printf("GPU %s: %s\n", pci[i].bus_id, pci[i].asleep ? "asleep (not checked)" : "awake");
        }
        return EXIT_OK;
    }

    struct gpu gpus[MAX_GPUS];
    unsigned count;
    if (open_all(gpus, &count) != 0) {
        if (json)
            printf(",\"gpus\":[]}\n");
        return EXIT_FAIL;
    }

    if (json)
        printf(",\"gpus\":[");
    for (unsigned i = 0; i < count; i++) {
        const struct gpu *g = &gpus[i];
        const struct request *req = state_for(v.effective, g->uuid);
        struct target t;
        int prc = policy_plan(&g->caps, req, &t);

        unsigned mem = 0, gfx = 0, mw = 0, temp = 0;
        unsigned long long reasons = 0;
        nvml_clock(g->dev, NVML_CLOCK_MEM, &mem);
        nvml_clock(g->dev, NVML_CLOCK_GRAPHICS, &gfx);
        int have_power = nvml_power_mw(g->dev, &mw) == NVML_SUCCESS;
        int have_temp = nvml_temperature(g->dev, &temp) == NVML_SUCCESS;
        if (nvml_event_reasons(g->dev, &reasons) != NVML_SUCCESS)
            reasons = 0;

        if (json) {
            json_gpu(g, req, &t, prc, mem, gfx, have_power, mw, have_temp, temp, reasons, i == 0);
            continue;
        }

        char mt[64], gt[64], lim[64];
        target_text(&t, prc, 1, mt, sizeof(mt));
        target_text(&t, prc, 0, gt, sizeof(gt));
        printf("GPU %u: %s", g->index, g->name);
        if (state_has_override(v.effective, g->uuid))
            printf(" (mode: %s)", req->mode == MODE_CUSTOM ? "locked" : mode_name(req->mode));
        printf("\n  memory    %5u MHz  (%s)\n", mem, mt);
        printf("  graphics  %5u MHz  (%s)\n", gfx, gt);
        if (have_power && have_temp)
            printf("  power     %5.0f W, %u C\n", mw / 1000.0, temp);
        else if (have_power)
            printf("  power     %5.0f W\n", mw / 1000.0);
        limits_text(reasons, lim, sizeof(lim));
        if (lim[0])
            printf("  limited   %s\n", lim);
    }
    close_all(gpus, count);
    if (json)
        printf("]}\n");
    return EXIT_OK;
}

/* ------------------------------------------------------------------------ */

int status_info(int json)
{
    struct pci_gpu pci[SYSFS_MAX_GPUS];
    int npci;
    if (any_asleep(pci, &npci)) {
        if (json)
            printf("{\"schema\":1,\"gpus\":[],\"asleep\":true}\n");
        else
            out_error("GPU asleep; wake it (e.g. start a game) and try again");
        return EXIT_OK;
    }

    struct gpu gpus[MAX_GPUS];
    unsigned count;
    if (open_all(gpus, &count) != 0)
        return EXIT_FAIL;

    char driver[NVML_DRIVER_LEN] = "";
    nvml_driver_version(driver, sizeof(driver));

    if (json) {
        printf("{\"schema\":1,\"version\":\"%s\",\"driver\":", VERSION);
        out_json_str(stdout, driver);
        printf(",\"gpus\":[");
    } else {
        printf("driver %s\n", driver);
    }

    for (unsigned i = 0; i < count; i++) {
        const struct gpu *g = &gpus[i];
        if (json) {
            printf("%s{\"index\":%u,\"name\":", i ? "," : "", g->index);
            out_json_str(stdout, g->name);
            printf(",\"uuid\":");
            out_json_str(stdout, g->uuid);
            printf(",\"arch\":\"%s\",\"can_lock_memory\":%s,\"can_lock_graphics\":%s,\"memory_clocks\":[",
                   gpu_arch_name(g->arch), g->caps.can_lock_mem ? "true" : "false",
                   g->caps.can_lock_gfx ? "true" : "false");
            for (unsigned m = 0; m < g->nmem; m++) {
                printf("%s{\"mhz\":%u,\"graphics\":[", m ? "," : "", g->mem_mhz[m]);
                for (unsigned k = 0; k < g->gfx[m].n; k++)
                    printf("%s%u", k ? "," : "", g->gfx[m].mhz[k]);
                printf("]}");
            }
            printf("]}");
            continue;
        }

        printf("GPU %u: %s (%s)\n", g->index, g->name, gpu_arch_name(g->arch));
        printf("  lock memory: %s, lock graphics: %s\n", g->caps.can_lock_mem ? "yes" : "no",
               g->caps.can_lock_gfx ? "yes" : "no");
        printf("  memory  graphics (MHz)\n");
        unsigned top = 0;
        for (unsigned m = 0; m < g->nmem; m++)
            if (g->gfx[m].n && g->gfx[m].mhz[0] > top)
                top = g->gfx[m].mhz[0];
        for (unsigned m = 0; m < g->nmem; m++) {
            const struct clocks *c = &g->gfx[m];
            printf("  %6u  ", g->mem_mhz[m]);
            if (c->n)
                printf("%u-%u", c->mhz[c->n - 1], c->mhz[0]);
            else
                printf("unknown");
            printf("%s\n", c->n && c->mhz[0] < top ? "  (limits graphics)" : "");
        }

        static const enum mode modes[] = { MODE_BALANCED, MODE_POWERSAVE, MODE_MAX };
        for (size_t k = 0; k < ARRAY_LEN(modes); k++) {
            struct request req = { .mode = modes[k] };
            struct target t;
            int prc = policy_plan(&g->caps, &req, &t);
            char mt[64], gt[64];
            target_text(&t, prc, 1, mt, sizeof(mt));
            target_text(&t, prc, 0, gt, sizeof(gt));
            if (prc != POLICY_OK)
                printf("  %-9s  not supported\n", mode_name(modes[k]));
            else
                printf("  %-9s  memory %s, graphics %s\n", mode_name(modes[k]), mt, gt);
        }
    }
    close_all(gpus, count);
    if (json)
        printf("]}\n");
    return EXIT_OK;
}
