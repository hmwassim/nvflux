/*
 * nvflux - lock NVIDIA memory clocks to stop HDMI/DisplayPort audio dropouts.
 *
 * The driver raises and lowers the memory clock as load changes, and every
 * switch briefly interrupts HDMI/DP audio. Locking the memory clock to one
 * value stops the switching.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "gpu.h"
#include "log.h"
#include "priv.h"
#include "profile.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2 };

static const char *const commands[] = {
    "powersave", "balanced", "performance", "lock", "auto", "status", "restore", "help", "version",
};

static void usage(FILE *f)
{
    fputs("Usage: nvflux <command>\n"
          "\n"
          "Lock the NVIDIA memory clock to stop HDMI/DisplayPort audio dropouts.\n"
          "\n"
          "Profiles (saved and re-applied at login):\n"
          "  powersave       lock memory to its lowest clock that doesn't limit the\n"
          "                  GPU core (desktop, video)\n"
          "  balanced        lock memory to a middle clock that doesn't limit the core\n"
          "  performance     lock memory to its highest clock (gaming)\n"
          "  lock <MHz>      lock memory to the supported clock nearest to <MHz>;\n"
          "                  add --force to allow a clock that limits the core\n"
          "  auto            unlock; let the driver manage the memory clock\n"
          "\n"
          "Other commands:\n"
          "  status          show the saved profile and current clocks (default)\n"
          "  restore         re-apply the saved profile\n"
          "  help            show this help\n"
          "  version         show the version\n",
          f);
}

/* ------------------------------------------------------------------------ */

static int open_gpus(unsigned *count)
{
    int rc = gpu_init();
    if (rc == 0)
        rc = gpu_count(count);
    if (rc == 0 && *count == 0)
        log_error("no NVIDIA GPU found");
    else if (rc != 0)
        log_error("%s", gpu_strerror(rc));
    else
        return 0;
    gpu_shutdown();
    return -1;
}

/*
 * Choose a memory clock for the profile from the GPU's supported clocks
 * (highest first). core_caps[i] is the highest core clock allowed with
 * clocks[i], or 0 if unknown.
 *
 * The lowest memory clocks belong to idle power states that also cap the
 * core clock: an RTX 3060 at 405 MHz memory allows at most 420 MHz core,
 * against 2100 MHz otherwise. Locking there makes even the desktop crawl, so
 * powersave and balanced only choose among clocks that keep the full core
 * range. On an RTX 3060 those are 7501/7301/5001/810 MHz: powersave takes
 * 810 and balanced 5001.
 */
static unsigned pick_clock(const struct profile *p, const unsigned *clocks,
                           const unsigned *core_caps, unsigned n)
{
    if (p->mode == MODE_PERFORMANCE)
        return clocks[0];

    if (p->mode == MODE_LOCK) {
        unsigned best = clocks[0];
        for (unsigned i = 1; i < n; i++) {
            unsigned d = clocks[i] > p->mhz ? clocks[i] - p->mhz : p->mhz - clocks[i];
            unsigned best_d = best > p->mhz ? best - p->mhz : p->mhz - best;
            if (d < best_d)
                best = clocks[i];
        }
        return best;
    }

    unsigned top = 0, usable[GPU_MAX_CLOCKS], nusable = 0;
    for (unsigned i = 0; i < n; i++)
        if (core_caps[i] > top)
            top = core_caps[i];
    for (unsigned i = 0; i < n; i++)
        if (core_caps[i] == top)
            usable[nusable++] = clocks[i];

    if (nusable == 0) /* only if n == 0, which callers rule out */
        return clocks[0];
    return p->mode == MODE_POWERSAVE ? usable[nusable - 1] : usable[nusable / 2];
}

static int apply_to_gpu(unsigned index, const struct profile *p)
{
    struct gpu gpu;
    int rc = gpu_get(index, &gpu);
    if (rc != 0) {
        log_error("GPU %u: %s", index, gpu_strerror(rc));
        return -1;
    }

    if (p->mode == MODE_AUTO) {
        if ((rc = gpu_unlock_mem(&gpu)) != 0) {
            log_error("GPU %u: cannot unlock memory clock: %s", index, gpu_strerror(rc));
            return -1;
        }
        printf("GPU %u (%s): memory clock unlocked\n", index, gpu.name);
        return 0;
    }

    unsigned clocks[GPU_MAX_CLOCKS], n = GPU_MAX_CLOCKS;
    if ((rc = gpu_mem_clocks(&gpu, clocks, &n)) != 0 || n == 0) {
        log_error("GPU %u: cannot read supported memory clocks: %s", index,
                  rc ? gpu_strerror(rc) : "none reported");
        return -1;
    }

    unsigned core_caps[GPU_MAX_CLOCKS], top_core = 0;
    for (unsigned i = 0; i < n; i++) {
        if (gpu_max_core_clock(&gpu, clocks[i], &core_caps[i]) != 0)
            core_caps[i] = 0;
        if (core_caps[i] > top_core)
            top_core = core_caps[i];
    }
    if (top_core == 0 && (p->mode == MODE_POWERSAVE || p->mode == MODE_BALANCED)) {
        log_error("GPU %u: cannot tell which memory clocks limit the core clock; "
                  "use 'nvflux lock <MHz>' instead", index);
        return -1;
    }

    unsigned mhz = pick_clock(p, clocks, core_caps, n);
    unsigned cap = 0;
    for (unsigned i = 0; i < n; i++)
        if (clocks[i] == mhz)
            cap = core_caps[i];

    /* A capped clock slows everything down, down to the mouse cursor, so it
     * has to be asked for explicitly. */
    int capped = cap && cap < top_core;
    if (capped && !p->force) {
        log_error("GPU %u: at %u MHz memory the core is limited to %u MHz (instead of %u), "
                  "which makes everything sluggish; add --force to lock it anyway",
                  index, mhz, cap, top_core);
        return -1;
    }

    if ((rc = gpu_keep_initialized(&gpu)) != 0)
        log_warn("GPU %u: cannot enable persistence mode (%s); the lock may be lost "
                 "while the GPU is idle", index, gpu_strerror(rc));

    if ((rc = gpu_lock_mem(&gpu, mhz)) != 0) {
        log_error("GPU %u: cannot lock memory clock to %u MHz: %s", index, mhz,
                  gpu_strerror(rc));
        return -1;
    }

    printf("GPU %u (%s): memory locked to %u MHz", index, gpu.name, mhz);
    if (p->mode == MODE_LOCK && mhz != p->mhz)
        printf(" (nearest supported to %u)", p->mhz);
    putchar('\n');

    if (capped)
        log_warn("GPU %u: at %u MHz memory the core is limited to %u MHz (instead of %u); "
                 "expect sluggish graphics", index, mhz, cap, top_core);
    return 0;
}

static int apply(const struct profile *p, int save)
{
    unsigned count, applied = 0;
    if (open_gpus(&count) != 0)
        return EXIT_FAIL;
    for (unsigned i = 0; i < count; i++)
        if (apply_to_gpu(i, p) == 0)
            applied++;
    gpu_shutdown();

    /* On a mixed system, remember the profile as long as one GPU took it. */
    if (save && applied > 0 && state_save(p) != 0)
        log_warn("cannot save profile to %s: %s", state_path(), strerror(errno));
    return applied == count ? EXIT_OK : EXIT_FAIL;
}

/* ------------------------------------------------------------------------ */

static int cmd_status(void)
{
    struct profile p;
    char saved[40];

    switch (state_load(&p)) {
    case STATE_LOADED:
        profile_format(&p, saved, sizeof(saved));
        break;
    case STATE_NONE:
        snprintf(saved, sizeof(saved), "auto");
        break;
    case STATE_INVALID:
        snprintf(saved, sizeof(saved), "invalid");
        break;
    default:
        snprintf(saved, sizeof(saved), "unreadable");
        break;
    }
    printf("profile: %s\n", saved);

    unsigned count;
    if (open_gpus(&count) != 0)
        return EXIT_FAIL;

    int status = EXIT_OK;
    for (unsigned i = 0; i < count; i++) {
        struct gpu gpu;
        unsigned mem, core;
        int rc = gpu_get(i, &gpu);
        if (rc == 0)
            rc = gpu_current_clock(&gpu, GPU_CLOCK_MEM, &mem);
        if (rc == 0)
            rc = gpu_current_clock(&gpu, GPU_CLOCK_CORE, &core);
        if (rc != 0) {
            log_error("GPU %u: %s", i, gpu_strerror(rc));
            status = EXIT_FAIL;
            continue;
        }
        printf("GPU %u (%s): memory %u MHz, core %u MHz\n", i, gpu.name, mem, core);
    }
    gpu_shutdown();
    return status;
}

static int cmd_restore(void)
{
    struct profile p;
    switch (state_load(&p)) {
    case STATE_LOADED:
        break;
    case STATE_NONE:
        return EXIT_OK;
    case STATE_INVALID:
        log_error("saved profile in %s is invalid; set a new one with 'nvflux <profile>'",
                  state_path());
        return EXIT_FAIL;
    default:
        log_error("cannot read %s: %s", state_path(), strerror(errno));
        return EXIT_FAIL;
    }

    /* Clocks start unlocked after boot, so "auto" needs no action. Doing
     * nothing also leaves alone locks that were set by other tools. */
    if (p.mode == MODE_AUTO)
        return EXIT_OK;
    return apply(&p, 0);
}

/* ------------------------------------------------------------------------ */

static unsigned edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned row[32];

    if (la >= 32 || lb >= 32)
        return ~0U;
    for (size_t j = 0; j <= lb; j++)
        row[j] = (unsigned)j;
    for (size_t i = 1; i <= la; i++) {
        unsigned diag = row[0];
        row[0] = (unsigned)i;
        for (size_t j = 1; j <= lb; j++) {
            unsigned up = row[j];
            unsigned best = diag + (a[i - 1] != b[j - 1]);
            if (up + 1 < best)
                best = up + 1;
            if (row[j - 1] + 1 < best)
                best = row[j - 1] + 1;
            row[j] = best;
            diag = up;
        }
    }
    return row[lb];
}

static void unknown_command(const char *cmd)
{
    const char *suggestion = NULL;
    unsigned best = 3; /* only suggest close matches */

    for (size_t i = 0; i < sizeof(commands) / sizeof(*commands); i++) {
        unsigned d = edit_distance(cmd, commands[i]);
        if (d < best) {
            best = d;
            suggestion = commands[i];
        }
    }

    log_error("unknown command '%s'", cmd);
    if (suggestion)
        fprintf(stderr, "Did you mean '%s'?\n", suggestion);
    fputs("Run 'nvflux help' for usage.\n", stderr);
}

static int is_any(const char *s, const char *a, const char *b, const char *c)
{
    return !strcmp(s, a) || !strcmp(s, b) || !strcmp(s, c);
}

int main(int argc, char **argv)
{
    priv_init();

    const char *cmd = argc > 1 ? argv[1] : "status";
    struct profile p;
    int parsed = profile_parse(argc - 1, argv + 1, &p);
    int restore = !strcmp(cmd, "restore") && argc == 2;

    /* Only changing clocks needs root; everything else gives it up first. */
    if (parsed == PROFILE_OK || restore) {
        if (priv_elevate() != 0) {
            priv_drop();
            log_error("changing clocks needs root; install nvflux with "
                      "'sudo ./install.sh' (it sets the setuid bit) or use sudo");
            return EXIT_FAIL;
        }
        return restore ? cmd_restore() : apply(&p, 1);
    }

    if (priv_drop() != 0) {
        log_error("cannot drop privileges");
        return EXIT_FAIL;
    }

    if (is_any(cmd, "help", "--help", "-h")) {
        usage(stdout);
        return EXIT_OK;
    }
    if (is_any(cmd, "version", "--version", "-V")) {
        printf("nvflux %s\n", VERSION);
        return EXIT_OK;
    }
    if (!strcmp(cmd, "status") && argc <= 2)
        return cmd_status();

    if (!strcmp(cmd, "lock"))
        log_error("usage: nvflux lock <MHz> [--force], e.g. 'nvflux lock 3000'");
    else if (parsed == PROFILE_BAD_ARGS || !strcmp(cmd, "status") || !strcmp(cmd, "restore"))
        log_error("'%s' takes no arguments", cmd);
    else
        unknown_command(cmd);
    return EXIT_USAGE;
}
