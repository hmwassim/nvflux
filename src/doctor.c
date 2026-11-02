#define _GNU_SOURCE
#include "doctor.h"

#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "gpu.h"
#include "nvml.h"
#include "policy.h"
#include "state.h"
#include "sysfs.h"

#define MAX_GPUS 16

static int failures;

__attribute__((format(printf, 2, 3)))
static void say(const char *level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%-5s ", level);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    if (!strcmp(level, "FAIL"))
        failures++;
}

/* ------------------------------------------------------------------------ */

static void check_permission(void)
{
    struct group *gr = getgrnam("nvflux");
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    struct stat st;
    if (n > 0) {
        self[n] = '\0';
        if (stat(self, &st) == 0 && st.st_uid == 0 && (st.st_mode & S_ISUID)) {
            if (gr && st.st_gid == gr->gr_gid && !(st.st_mode & S_IXOTH))
                say("ok", "program: setuid root, group nvflux only");
            else
                say("ok", "program: setuid root, all users");
        } else {
            say("warn", "program: %s is not setuid root; changing clocks needs sudo; run install.sh", self);
            return;
        }
    }

    if (getuid() == 0) {
        say("ok", "permission: running as root");
        return;
    }
    if (!gr) {
        say("warn", "permission: no nvflux group; use sudo or run install.sh");
        return;
    }
    gid_t groups[256];
    int ng = getgroups(256, groups);
    for (int i = 0; i < ng; i++)
        if (groups[i] == gr->gr_gid) {
            say("ok", "permission: in the nvflux group");
            return;
        }
    struct passwd *pw = getpwuid(getuid());
    const char *me = pw ? pw->pw_name : "";
    for (char **m = gr->gr_mem; m && *m; m++)
        if (!strcmp(*m, me)) {
            say("warn", "permission: added to the nvflux group but not active yet; log out and back in");
            return;
        }
    say("warn", "permission: not in the nvflux group; use sudo or 'sudo usermod -aG nvflux %s'", me);
}

/* The installer records which boot and sleep hooks it set up. */
static void check_hooks(void)
{
    FILE *f = fopen(state_manifest_path(), "r");
    if (!f) {
        say("warn", "hooks: not installed; the mode won't come back after reboot; run install.sh");
        return;
    }
    char line[512], boot[64] = "", sleep[128] = "";
    int missing = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *val = strchr(line, ' ');
        if (!val)
            continue;
        *val++ = '\0';
        if (!strcmp(line, "boot"))
            snprintf(boot, sizeof(boot), "%s", val);
        else if (!strcmp(line, "sleep"))
            snprintf(sleep + strlen(sleep), sizeof(sleep) - strlen(sleep), "%s%s", sleep[0] ? ", " : "", val);
        else if (!strcmp(line, "file") && access(val, F_OK) != 0) {
            say("FAIL", "hooks: %s is missing; run install.sh again", val);
            missing++;
        }
    }
    fclose(f);

    if (!boot[0] || !strcmp(boot, "none"))
        say("warn", "boot hook: none for this init system; add 'nvflux reload' to your startup");
    else if (!missing)
        say("ok", "boot hook: %s", boot);
    if (!sleep[0])
        say("warn", "sleep hook: none; locks stay on during sleep; run 'nvflux reload' after resume");
    else if (!missing)
        say("ok", "sleep hook: %s", sleep);
}

static void check_state(struct state *saved, struct state *temp, int *has_temp)
{
    int rc = state_load(state_saved_path(), saved, NULL);
    *has_temp = state_load(state_temp_path(), temp, NULL) == STATE_LOADED;
    if (rc == STATE_INVALID)
        say("FAIL", "saved mode: %s is damaged; choose a mode again", state_saved_path());
    else if (rc == STATE_UNREADABLE)
        say("FAIL", "saved mode: cannot read %s", state_saved_path());
    if (rc != STATE_LOADED)
        state_init(saved);

    if (sysfs_cmdline_has("nvflux=off"))
        say("warn", "boot: nvflux=off on the kernel command line; reload does nothing");
}

/* Compares what nvflux should have set with the clocks the GPU runs now.
 * There's no way to read locks back, but at idle a locked clock shows up
 * exactly; graphics floors show as at least the floor. */
static void check_applied(const struct gpu *g, const struct request *req)
{
    struct target t;
    int rc = policy_plan(&g->caps, req, &t);
    const char *name = req->mode == MODE_CUSTOM ? "locked" : mode_name(req->mode);
    if (rc != POLICY_OK) {
        say("FAIL", "GPU %u: mode %s can't be applied on this GPU; choose another mode", g->index, name);
        return;
    }
    if (req->mode == MODE_OFF)
        return;

    unsigned mem = 0, gfx = 0;
    nvml_clock(g->dev, NVML_CLOCK_MEM, &mem);
    nvml_clock(g->dev, NVML_CLOCK_GRAPHICS, &gfx);
    if (t.mem == MEM_LOCK && mem < t.mem_mhz)
        say("FAIL", "GPU %u: %s not applied (memory %u MHz, expected %u); run 'sudo nvflux reload'",
            g->index, name, mem, t.mem_mhz);
    else if (t.mem == MEM_LOCK && mem > t.mem_mhz)
        say("warn", "GPU %u: memory runs at %u MHz, not %u; the displays need more, or the lock was lost",
            g->index, mem, t.mem_mhz);
    else if (t.gfx == GFX_RANGE && gfx < t.gfx_min)
        say("FAIL", "GPU %u: %s not applied (graphics %u MHz, floor %u); run 'sudo nvflux reload'",
            g->index, name, gfx, t.gfx_min);
    else
        say("ok", "GPU %u: %s applied", g->index, name);
}

static void check_gpus(const struct state *effective)
{
    struct pci_gpu pci[SYSFS_MAX_GPUS];
    int npci = sysfs_nvidia_gpus(pci, SYSFS_MAX_GPUS);
    int has_battery = 0;
    sysfs_on_battery(&has_battery);
    for (int i = 0; i < npci; i++) {
        if (pci[i].asleep) {
            say("ok", "GPU %s: asleep; GPU checks skipped so it stays asleep", pci[i].bus_id);
            if (has_battery)
                say("warn", "GPU %s: laptop GPU; locks may not survive it powering off (untested)",
                    pci[i].bus_id);
            return;
        }
    }

    char kernel[NVML_DRIVER_LEN] = "";
    int have_kernel = sysfs_kernel_driver_version(kernel, sizeof(kernel)) == 0;
    if (!have_kernel && npci > 0)
        say("FAIL", "driver: NVIDIA kernel module not loaded; install the driver or reboot");

    int rc = nvml_open();
    if (rc != NVML_SUCCESS) {
        say("FAIL", "driver: %s", nvml_strerror(rc));
        nvml_close();
        return;
    }
    char lib[NVML_DRIVER_LEN] = "";
    nvml_driver_version(lib, sizeof(lib));
    if (have_kernel && strcmp(kernel, lib) != 0)
        say("FAIL", "driver: updated without reboot (kernel %s, library %s); reboot", kernel, lib);
    else
        say("ok", "driver: %s", lib);

    unsigned n = 0;
    nvml_count(&n);
    if (n == 0)
        say("FAIL", "GPU: no NVIDIA GPU found");
    for (unsigned i = 0; i < n && i < MAX_GPUS; i++) {
        struct gpu g;
        if ((rc = gpu_open(i, &g)) != NVML_SUCCESS) {
            say("FAIL", "GPU %u: %s", i, nvml_strerror(rc));
            continue;
        }
        if (!g.caps.can_lock_gfx && !g.caps.can_lock_mem)
            say("FAIL", "GPU %u: %s (%s) can't lock clocks; needs GTX 16/RTX 20 or newer", i, g.name,
                gpu_arch_name(g.arch));
        else if (!g.caps.can_lock_mem)
            say("warn", "GPU %u: %s (%s) can't lock memory; balanced won't stop audio crackle", i,
                g.name, gpu_arch_name(g.arch));
        else
            say("ok", "GPU %u: %s (%s)", i, g.name, gpu_arch_name(g.arch));

        check_applied(&g, state_for(effective, g.uuid));

        /* nvflux never forces persistence mode on a GPU with runtime power
         * management: on a laptop that would keep it awake and drain the
         * battery. */
        struct pci_gpu pm;
        int runtime_pm = g.bus_id[0] && sysfs_gpu(g.bus_id, &pm) == 0 && pm.runtime_pm;
        int on = 0;
        if (nvml_persistence(g.dev, &on) == NVML_SUCCESS) {
            if (on)
                say("ok", "GPU %u: persistence mode on", i);
            else if (runtime_pm)
                say("ok", "GPU %u: persistence mode off (runtime power management is on)", i);
            else
                say("warn", "GPU %u: persistence mode off; the lock may be lost when idle; "
                    "enable nvidia-persistenced", i);
        }
        if (runtime_pm && has_battery)
            say("warn", "GPU %u: laptop GPU; locks may not survive it powering off (untested)", i);

        unsigned long long r = 0;
        if (nvml_event_reasons(g.dev, &r) == NVML_SUCCESS) {
            if (r & (NVML_REASON_HW_THERMAL | NVML_REASON_SW_THERMAL))
                say("warn", "GPU %u: slowed down by heat; check cooling", i);
            if (r & NVML_REASON_HW_POWER_BRAKE)
                say("warn", "GPU %u: slowed down by the power supply", i);
        }
        gpu_close(&g);
    }
    nvml_close();
}

int doctor_run(void)
{
    failures = 0;
    printf("nvflux %s\n", VERSION);

    struct state saved, temp;
    int has_temp;
    check_state(&saved, &temp, &has_temp);
    const struct state *effective = has_temp ? &temp : &saved;
    const char *label = effective->ngpu ? "per-gpu"
                        : effective->all.mode == MODE_CUSTOM ? "locked"
                        : mode_name(effective->all.mode);
    say("ok", "mode: %s%s", label, has_temp ? " (temporary)" : "");

    check_permission();
    check_hooks();
    check_gpus(effective);
    return failures ? EXIT_FAIL : EXIT_OK;
}
