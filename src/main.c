/*
 * nvflux - make NVIDIA GPU clocks behave, safely, with one short command.
 *
 * Installed setuid root, mode 4750, group nvflux: members of the group can
 * change clocks without sudo; everyone else uses sudo. Read-only commands
 * give root up before doing anything.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apply.h"
#include "cli.h"
#include "doctor.h"
#include "common.h"
#include "gpu.h"
#include "nvml.h"
#include "out.h"
#include "policy.h"
#include "priv.h"
#include "state.h"
#include "status.h"
#include "sysfs.h"

#define MAX_GPUS 16

/* ------------------------------------------------------------------------ */
/* Privileges                                                                */

/* People outside the nvflux group can't start the installed program at all;
 * this is reached when nvflux runs without its setuid bit. */
static int permission_error(void)
{
    out_error("changing clocks needs root; use sudo or join the nvflux group");
    return EXIT_PERMISSION;
}

/* ------------------------------------------------------------------------ */
/* GPUs                                                                      */

struct gpus {
    struct gpu gpu[MAX_GPUS];
    struct pci_gpu pm[MAX_GPUS];
    int has_pm[MAX_GPUS];
    unsigned count;
};

static void close_gpus(struct gpus *gs)
{
    for (unsigned i = 0; i < gs->count; i++)
        gpu_close(&gs->gpu[i]);
    gs->count = 0;
    nvml_close();
}

/* Opens NVML and every GPU. With wait > 0, retries while the driver comes up
 * (used by the boot and resume hooks). Reports errors itself. */
static int open_gpus(struct gpus *gs, unsigned wait)
{
    int rc;
    memset(gs, 0, sizeof(*gs));
    for (unsigned tries = 0;; tries++) {
        rc = nvml_open();
        if (rc == NVML_SUCCESS || rc == NVML_UNTRUSTED_LIBRARY || tries >= wait)
            break;
        nvml_close();
        sleep(1);
    }
    unsigned n = 0;
    if (rc == NVML_SUCCESS)
        rc = nvml_count(&n);
    if (rc != NVML_SUCCESS) {
        out_error("%s", nvml_strerror(rc));
        nvml_close();
        return -1;
    }
    if (n == 0) {
        out_error("no NVIDIA GPU found");
        nvml_close();
        return -1;
    }
    if (n > MAX_GPUS)
        n = MAX_GPUS;
    for (unsigned i = 0; i < n; i++) {
        if ((rc = gpu_open(i, &gs->gpu[gs->count])) != NVML_SUCCESS) {
            out_error("GPU %u: %s", i, nvml_strerror(rc));
            continue;
        }
        struct gpu *g = &gs->gpu[gs->count];
        gs->has_pm[gs->count] = g->bus_id[0] && sysfs_gpu(g->bus_id, &gs->pm[gs->count]) == 0;
        gs->count++;
    }
    if (gs->count == 0) {
        nvml_close();
        return -1;
    }
    return 0;
}

static struct gpu *find_gpu_by_index(struct gpus *gs, unsigned index)
{
    for (unsigned i = 0; i < gs->count; i++)
        if (gs->gpu[i].index == index)
            return &gs->gpu[i];
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Messages for one GPU                                                      */

/* One line per GPU: "GPU 0: balanced: memory 810 MHz, graphics 945-2100 MHz". */
static void describe(const struct request *req, const struct target *t, const struct apply_result *r,
                     char *buf, size_t len)
{
    char mem[64] = "", gfx[64] = "";

    if (t->mem == MEM_LOCK && !(r->flags & APPLY_MEM_SKIPPED)) {
        if (req->mode == MODE_CUSTOM && req->mem && t->mem_mhz != req->mem)
            snprintf(mem, sizeof(mem), "memory %u MHz (nearest to %u)", t->mem_mhz, req->mem);
        else
            snprintf(mem, sizeof(mem), "memory %u MHz", t->mem_mhz);
    }
    if (!(r->flags & APPLY_GFX_SKIPPED)) {
        if (t->gfx == GFX_CAP_BASE && !(r->flags & APPLY_GFX_FALLBACK))
            snprintf(gfx, sizeof(gfx), "graphics up to base clock");
        else if ((t->gfx == GFX_RANGE || t->gfx == GFX_CAP_BASE) && r->gfx_min == r->gfx_max)
            snprintf(gfx, sizeof(gfx), "graphics %u MHz", r->gfx_min);
        else if (t->gfx == GFX_RANGE || t->gfx == GFX_CAP_BASE)
            snprintf(gfx, sizeof(gfx), "graphics %u-%u MHz", r->gfx_min, r->gfx_max);
        if (t->notes & NOTE_GFX_SNAPPED) {
            size_t n = strlen(gfx);
            if (req->gfx_min == req->gfx_max)
                snprintf(gfx + n, sizeof(gfx) - n, " (nearest to %u)", req->gfx_min);
            else
                snprintf(gfx + n, sizeof(gfx) - n, " (nearest to %u-%u)", req->gfx_min, req->gfx_max);
        }
    }

    const char *name = req->mode == MODE_CUSTOM ? "locked" : mode_name(req->mode);
    if (req->mode == MODE_OFF || (!mem[0] && !gfx[0]))
        snprintf(buf, len, "%s", name);
    else
        snprintf(buf, len, "%s: %s%s%s", name, mem, mem[0] && gfx[0] ? ", " : "", gfx);
}

static void report_notes(const struct gpu *g, const struct request *req, const struct target *t,
                         const struct apply_result *r)
{
    int no_mem = (t->notes & NOTE_NO_MEM_LOCK) || (r->flags & APPLY_MEM_SKIPPED);
    int no_gfx = (t->notes & NOTE_NO_GFX_LOCK) || (r->flags & APPLY_GFX_SKIPPED);

    if (no_mem && req->mode != MODE_OFF)
        out_warn("GPU %u: memory clock can't be locked (needs RTX 30 or newer)", g->index);
    if (no_gfx && req->mode != MODE_OFF && req->mode != MODE_MAX)
        out_warn("GPU %u: graphics clock can't be locked (needs GTX 16/RTX 20 or newer)", g->index);
    if (t->notes & NOTE_MEM_CAPS_CORE)
        out_warn("GPU %u: %u MHz memory limits graphics to %u MHz", g->index, t->mem_mhz, t->core_cap);
    if (t->notes & NOTE_GFX_LOW_CAP)
        out_warn("GPU %u: graphics capped below %u MHz; the GPU will be slow", g->index, t->safe_cap_min);
    if (r->flags & APPLY_MEM_OVERRIDDEN)
        out_warn("GPU %u: memory runs at %u MHz, not %u; the displays need more", g->index,
                 r->observed_mem, t->mem_mhz);
    if (r->flags & APPLY_PERSIST_FAILED)
        out_warn("GPU %u: persistence mode failed; the lock may be lost when idle", g->index);
}

static void report_policy_error(const struct gpu *g, const struct request *req, const struct target *t,
                                int rc)
{
    switch (rc) {
    case POLICY_UNSUPPORTED:
        if (req->mode == MODE_CUSTOM && req->mem && !g->caps.can_lock_mem)
            out_error("GPU %u: memory clock can't be locked (needs RTX 30 or newer)", g->index);
        else
            out_error("GPU %u: clocks can't be locked (needs GTX 16/RTX 20 or newer)", g->index);
        break;
    case POLICY_NO_CLOCKS:
        out_error("GPU %u: driver reports no supported clocks", g->index);
        break;
    case POLICY_CAPS_UNKNOWN:
        out_error("GPU %u: driver doesn't report safe clocks; use 'nvflux lock memory <MHz>'", g->index);
        break;
    case POLICY_MEM_CAPS_CORE:
        out_error("GPU %u: %u MHz memory limits graphics to %u MHz; add --force to allow", g->index,
                  t->mem_mhz, t->core_cap);
        break;
    case POLICY_GFX_CAP_TOO_LOW:
        out_error("GPU %u: graphics cap below %u MHz; add --force to allow", g->index, t->safe_cap_min);
        break;
    case POLICY_RANGE_INVERTED:
        out_error("GPU %u: use 'nvflux lock graphics %u-%u'", g->index, req->gfx_max, req->gfx_min);
        break;
    }
}

/* Plans and applies one GPU's request. Returns 1 applied, 0 failed, -1 unsupported. */
static int run_gpu(struct gpus *gs, unsigned i, const struct request *req, int verify)
{
    struct gpu *g = &gs->gpu[i];
    struct target t;
    struct apply_result r;

    int rc = policy_plan(&g->caps, req, &t);
    if (rc != POLICY_OK) {
        report_policy_error(g, req, &t, rc);
        return rc == POLICY_UNSUPPORTED ? -1 : 0;
    }

    apply_target(g, gs->has_pm[i] ? &gs->pm[i] : NULL, &t, verify, &r);
    if (r.status == APPLY_UNSUPPORTED) {
        report_policy_error(g, req, &t, POLICY_UNSUPPORTED);
        return -1;
    }
    if (r.status == APPLY_FAILED) {
        out_error("GPU %u: failed to set %s clock: %s; nothing changed%s", g->index, r.failed,
                  nvml_strerror(r.nvml_rc), r.nvml_rc == NVML_ERROR_NO_PERMISSION ? "; use sudo" : "");
        return 0;
    }

    char what[160];
    describe(req, &t, &r, what, sizeof(what));
    out_line("GPU %u: %s", g->index, what);
    report_notes(g, req, &t, &r);
    return 1;
}

static int exit_code(unsigned ok, unsigned failed, unsigned unsupported)
{
    if (failed == 0 && unsupported == 0)
        return EXIT_OK;
    if (ok > 0)
        return EXIT_PARTIAL;
    return failed ? EXIT_FAIL : EXIT_UNSUPPORTED;
}

/* ------------------------------------------------------------------------ */
/* Commands that change clocks                                               */

static int cmd_change(const struct cmd *c)
{
    struct gpus gs;
    if (open_gpus(&gs, 0) != 0)
        return EXIT_FAIL;

    const struct gpu *only = NULL;
    if (c->gpu_set && !(only = find_gpu_by_index(&gs, c->gpu))) {
        out_error("no GPU %u; see 'nvflux status'", c->gpu);
        close_gpus(&gs);
        return EXIT_USAGE;
    }

    struct state saved, temp;
    int migrated;
    int saved_rc = state_load(state_saved_path(), &saved, &migrated);
    if (saved_rc == STATE_INVALID || saved_rc == STATE_UNREADABLE) {
        out_warn("%s unreadable; replacing it", state_saved_path());
        state_init(&saved);
    }
    int has_temp = state_load(state_temp_path(), &temp, NULL) == STATE_LOADED;
    const struct state *effective = has_temp ? &temp : &saved;
    const char *scope = only ? only->uuid : NULL;

    /* The new request. Expert locks add to an existing custom setting, so
     * 'lock memory' then 'lock graphics' keeps both. */
    struct request req = { .mode = c->mode };
    if (c->kind == CMD_LOCK) {
        const struct request *base = scope ? state_for(effective, scope) : &effective->all;
        if (base->mode == MODE_CUSTOM)
            req = *base;
        else
            memset(&req, 0, sizeof(req));
        req.mode = MODE_CUSTOM;
        if (c->lock_mem) {
            req.mem = c->mem;
            req.mem_force = c->force;
        }
        if (c->lock_gfx) {
            req.gfx_min = c->gfx_min;
            req.gfx_max = c->gfx_max;
            req.gfx_force = c->force;
        }
        /* --force accepts a slow result for the whole setting: changing the
         * memory clock can squeeze a graphics lock that's already saved. */
        if (c->force) {
            req.mem_force = req.mem != 0;
            req.gfx_force = req.gfx_max != 0;
        }
    }

    if (c->alias)
        out_warn("'%s' is now '%s'", c->alias, mode_name(req.mode));
    if (c->old_lock)
        out_warn("'lock %u' is now 'lock memory %u'", c->mem, c->mem);

    struct state dest = c->temporary ? *effective : saved;
    if (scope)
        state_set_gpu(&dest, scope, &req);
    else
        state_set_all(&dest, &req);

    unsigned ok = 0, failed = 0, unsupported = 0;
    for (unsigned i = 0; i < gs.count; i++) {
        if (only && &gs.gpu[i] != only)
            continue;
        int rc = run_gpu(&gs, i, state_for(&dest, gs.gpu[i].uuid), 1);
        if (rc > 0)
            ok++;
        else if (rc < 0)
            unsupported++;
        else
            failed++;
    }
    close_gpus(&gs);

    if (ok == 0)
        return exit_code(ok, failed, unsupported);

    /* Save as long as one GPU took it (on a mixed system the others report
     * their own problem). */
    int save_rc;
    if (c->temporary) {
        save_rc = state_save(state_temp_path(), &dest);
    } else {
        save_rc = state_save(state_saved_path(), &dest);
        if (save_rc == 0 && has_temp) {
            if (scope) {
                state_set_gpu(&temp, scope, &req);
                save_rc = state_save(state_temp_path(), &temp);
            } else {
                save_rc = state_remove(state_temp_path());
            }
        }
    }
    if (save_rc != 0)
        out_warn("could not save: %s", strerror(errno));

    const struct request *back = scope ? state_for(&saved, scope) : &saved.all;
    if (c->temporary)
        out_line("(temporary, saved: %s)", back->mode == MODE_CUSTOM ? "locked" : mode_name(back->mode));
    return exit_code(ok, failed, unsupported);
}

static int cmd_reload(const struct cmd *c)
{
    if (c->alias)
        out_warn("'%s' is now 'reload'", c->alias);
    if (sysfs_cmdline_has("nvflux=off")) {
        out_warn("disabled by nvflux=off on the kernel command line");
        return EXIT_OK;
    }

    struct state saved, temp;
    int had_temp = state_load(state_temp_path(), &temp, NULL) == STATE_LOADED;
    if (state_remove(state_temp_path()) != 0)
        out_warn("could not remove %s: %s", state_temp_path(), strerror(errno));

    int rc = state_load(state_saved_path(), &saved, NULL);
    if (rc == STATE_INVALID) {
        out_error("%s is damaged; choose a mode again", state_saved_path());
        return EXIT_FAIL;
    }
    if (rc == STATE_UNREADABLE) {
        out_error("cannot read %s: %s", state_saved_path(), strerror(errno));
        return EXIT_FAIL;
    }

    /* Nothing saved, and nothing temporary to undo: leave the GPUs alone
     * (including locks set by other tools). */
    int all_off = saved.all.mode == MODE_OFF && saved.ngpu == 0;
    if (all_off && !had_temp)
        return EXIT_OK;

    /* Never wake a sleeping laptop GPU just to lock it. */
    struct pci_gpu pci[SYSFS_MAX_GPUS];
    int npci = sysfs_nvidia_gpus(pci, SYSFS_MAX_GPUS), asleep = 0;
    for (int i = 0; i < npci; i++)
        asleep += pci[i].asleep;
    if (npci > 0 && asleep == npci) {
        out_warn("GPU asleep; not waking it");
        return EXIT_OK;
    }

    /* Only wait for the driver when the kernel shows an NVIDIA GPU: without
     * one, waiting would just delay boot. */
    struct gpus gs;
    if (open_gpus(&gs, npci > 0 ? c->wait : 0) != 0)
        return EXIT_FAIL;

    unsigned ok = 0, failed = 0, unsupported = 0;
    for (unsigned i = 0; i < gs.count; i++) {
        const struct request *req = state_for(&saved, gs.gpu[i].uuid);
        if (req->mode == MODE_OFF && !had_temp)
            continue;
        if (gs.has_pm[i] && gs.pm[i].asleep)
            continue;
        int r = run_gpu(&gs, i, req, 0);
        ok += r > 0;
        failed += r == 0;
        unsupported += r < 0;
    }
    for (unsigned s = 0; s < saved.ngpu; s++) {
        int present = 0;
        for (unsigned i = 0; i < gs.count; i++)
            present |= strcmp(gs.gpu[i].uuid, saved.gpu[s].uuid) == 0;
        if (!present)
            out_warn("%s not present; skipped", saved.gpu[s].uuid);
    }
    close_gpus(&gs);
    return exit_code(ok, failed, unsupported);
}

/* ------------------------------------------------------------------------ */

static void usage(FILE *f)
{
    fputs("usage: nvflux [command] [options]\n"
          "\n"
          "modes (saved; restored after reboot and sleep):\n"
          "  balanced    steady memory, graphics floor   (audio crackle, flicker, lag)\n"
          "  powersave   steady memory, graphics capped  (heat, fan noise, battery)\n"
          "  max         memory at full speed            (games, compute)\n"
          "  off         driver controls the clocks\n"
          "\n"
          "commands:\n"
          "  status                           mode and clocks (default)\n"
          "  info                             supported clocks\n"
          "  doctor                           check the setup\n"
          "  reload                           apply the saved mode again\n"
          "  lock memory <MHz>                lock the memory clock\n"
          "  lock graphics <MHz>|<min>-<max>  lock the graphics clock\n"
          "  help, version\n"
          "\n"
          "options:\n"
          "  --temporary   apply without saving\n"
          "  --gpu <N>     one GPU only\n"
          "  --force       allow a lock that slows the GPU (lock only)\n"
          "  --json        machine-readable output (status, info)\n"
          "\n"
          "Changing clocks needs root: use sudo or join the nvflux group.\n",
          f);
}

int main(int argc, char **argv)
{
    priv_init();

    struct cmd c;
    char err[512];
    if (cli_parse(argc, argv, &c, err, sizeof(err)) != 0) {
        priv_drop();
        out_error("%s", err);
        return EXIT_USAGE;
    }

    if (cli_needs_root(&c)) {
        if (priv_elevate() != 0) {
            priv_drop();
            return permission_error();
        }
        return c.kind == CMD_RELOAD ? cmd_reload(&c) : cmd_change(&c);
    }

    if (priv_drop() != 0) {
        out_error("could not drop privileges");
        return EXIT_FAIL;
    }
    switch (c.kind) {
    case CMD_HELP:
        usage(stdout);
        return EXIT_OK;
    case CMD_VERSION:
        printf("nvflux %s\n", VERSION);
        return EXIT_OK;
    case CMD_INFO:
        return status_info(c.json);
    case CMD_DOCTOR:
        return doctor_run();
    default:
        return status_show(c.json);
    }
}
