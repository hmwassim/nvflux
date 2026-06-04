/*
 * nvflux — Minimal NVIDIA GPU clock profile manager
 * Fixes HDMI/DP audio dropouts by locking memory clocks
 *
 * Works on: Any Linux distro, DE, WM, init system, display server
 * Usage: nvflux <command>
 * Commands: powersave|balanced|performance|ultra|auto|status|clock|--restore
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>

#define VERSION "1.0.0"
#define MAX_CLOCKS 16
#define STATE_DIR  "/var/lib/nvflux"
#define STATE_PATH STATE_DIR "/state"

/* Profile enum - PROFILE_INVALID must be -1 for error checking */
typedef enum { PROFILE_INVALID = -1, PROFILE_AUTO, PROFILE_POWERSAVE, PROFILE_BALANCED, PROFILE_PERFORMANCE, PROFILE_ULTRA } Profile;

/* Global nvidia-smi path */
static char nvsmi[PATH_MAX];

/* ───────────────────────────────────────────────────────────────────────────
 * Helper: find nvidia-smi (search common paths + PATH)
 * ─────────────────────────────────────────────────────────────────────────── */
static int find_nvidia_smi(void) {
    const char *paths[] = {
        "/usr/bin/nvidia-smi",
        "/usr/local/bin/nvidia-smi",
        "/usr/bin/nvidia-smi.bin",
        NULL,
    };
    for (int i = 0; paths[i]; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (access(paths[i], X_OK) != 0) continue;
        strncpy(nvsmi, paths[i], sizeof(nvsmi)-1);
        nvsmi[sizeof(nvsmi)-1] = '\0';
        return 0;
    }
    return -1;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Helper: run nvidia-smi and capture output
 * ─────────────────────────────────────────────────────────────────────────── */
static void close_all_fds(void) {
    int max = (int)sysconf(_SC_OPEN_MAX);
    if (max <= 0) max = 1024;
    for (int i = 3; i < max; i++) close(i);
}

static int run_capture(char *const argv[], char *buf, size_t len) {
    int fd[2]; if (pipe(fd) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        close(fd[0]); dup2(fd[1], STDOUT_FILENO); dup2(fd[1], STDERR_FILENO); close(fd[1]);
        close_all_fds();
        execv(argv[0], argv); _exit(127);
    }
    close(fd[1]);
    size_t total = 0; ssize_t r;
    while (total + 1 < len && (r = read(fd[0], buf + total, len - total - 1)) > 0) total += r;
    close(fd[0]); buf[total] = '\0';
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int run_silent(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close_all_fds();
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        execv(argv[0], argv); _exit(127);
    }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Parse clocks from CSV output (sorted descending)
 * ─────────────────────────────────────────────────────────────────────────── */
static int parse_clocks(const char *txt, int *clocks, int max) {
    int n = 0;
    for (const char *p = txt; *p && n < max; ) {
        while (*p && !isdigit((unsigned char)*p)) p++;
        if (!*p) break;
        char *end; clocks[n++] = (int)strtol(p, &end, 10); p = end;
    }
    /* Sort descending */
    for (int i = 0; i < n; i++) {
        int hi = i;
        for (int j = i+1; j < n; j++) if (clocks[j] > clocks[hi]) hi = j;
        if (hi != i) { int t = clocks[i]; clocks[i] = clocks[hi]; clocks[hi] = t; }
    }
    return n;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Query supported memory clocks
 * ─────────────────────────────────────────────────────────────────────────── */
static int get_mem_clocks(int *clocks, int max) {
    char buf[4096], arg[64];
    snprintf(arg, sizeof(arg), "--query-supported-clocks=memory");
    char *argv[] = {nvsmi, arg, "--format=csv,noheader,nounits", NULL};
    if (run_capture(argv, buf, sizeof(buf)) != 0) return -1;
    return parse_clocks(buf, clocks, max);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Get current memory clock
 * ─────────────────────────────────────────────────────────────────────────── */
static int get_current_mem(void)
{
    char buf[256], arg[64];
    snprintf(arg, sizeof(arg), "--query-gpu=clocks.current.memory");
    char *argv[] = {nvsmi, arg, "--format=csv,noheader,nounits", NULL};
    if (run_capture(argv, buf, sizeof(buf)) != 0) return -1;
    const char *p = buf;
    while (*p && !isdigit((unsigned char)*p)) p++;
    return (*p) ? (int)strtol(p, NULL, 10) : -1;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Get current GPU core clock
 * ─────────────────────────────────────────────────────────────────────────── */
static int get_current_gpu(void)
{
    char buf[256], arg[64];
    snprintf(arg, sizeof(arg), "--query-gpu=clocks.current.graphics");
    char *argv[] = {nvsmi, arg, "--format=csv,noheader,nounits", NULL};
    if (run_capture(argv, buf, sizeof(buf)) != 0) return -1;
    const char *p = buf;
    while (*p && !isdigit((unsigned char)*p)) p++;
    return (*p) ? (int)strtol(p, NULL, 10) : -1;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Lock memory clock
 * ─────────────────────────────────────────────────────────────────────────── */
static int lock_mem(int mhz) {
    char arg[64];
    snprintf(arg, sizeof(arg), "--lock-memory-clocks=%d,%d", mhz, mhz);
    char *argv[] = {nvsmi, arg, NULL};
    if (run_silent(argv) == 0) return 0;
    /* Hopper+ fallback */
    snprintf(arg, sizeof(arg), "--lock-memory-clocks-deferred=%d", mhz);
    char *argv2[] = {nvsmi, arg, NULL};
    return run_silent(argv2);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Lock GPU core clock
 * ─────────────────────────────────────────────────────────────────────────── */
static int lock_gpu(int mhz) {
    char arg[64], mode_arg[16];
    /* mode=0: highest frequency accuracy (default, best for stability) */
    snprintf(arg, sizeof(arg), "--lock-gpu-clocks=%d,%d", mhz, mhz);
    snprintf(mode_arg, sizeof(mode_arg), "--mode=0");
    char *argv[] = {nvsmi, arg, mode_arg, NULL};
    return run_silent(argv);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Reset GPU core clocks
 * ─────────────────────────────────────────────────────────────────────────── */
static int reset_gpu(void) {
    const char *opts[] = {"--reset-gpu-clocks", "-rgc", NULL};
    for (const char **o = opts; *o; o++) { char *argv[] = {nvsmi, (char *)*o, NULL}; if (run_silent(argv) == 0) return 0; }
    return -1;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Query supported GPU core clocks
 * ─────────────────────────────────────────────────────────────────────────── */
static int get_gpu_clocks(int *clocks, int max) {
    char buf[4096], arg[64];
    snprintf(arg, sizeof(arg), "--query-supported-clocks=graphics");
    char *argv[] = {nvsmi, arg, "--format=csv,noheader,nounits", NULL};
    if (run_capture(argv, buf, sizeof(buf)) != 0) return -1;
    return parse_clocks(buf, clocks, max);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Reset memory clocks
 * ─────────────────────────────────────────────────────────────────────────── */
static int reset_mem(void) {
    const char *opts[] = {"--reset-memory-clocks", "-rmc", "--reset-memory-clocks-deferred", "-rmcd", NULL};
    for (const char **o = opts; *o; o++) { char *argv[] = {nvsmi, (char *)*o, NULL}; if (run_silent(argv) == 0) return 0; }
    return -1;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Enable persistence mode
 * ─────────────────────────────────────────────────────────────────────────── */
static int enable_persistence(void) {
    char *argv[] = {nvsmi, "-pm", "1", NULL};
    return run_silent(argv);
}

/* ───────────────────────────────────────────────────────────────────────────
 * State file: /var/lib/nvflux/state (system-wide, all users)
 * ─────────────────────────────────────────────────────────────────────────── */
static int state_write(const char *mode) {
    /* Ensure state directory exists */
    if (mkdir(STATE_DIR, 0755) != 0 && errno != EEXIST) return -1;

    int fd = open(STATE_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return -1;
    fchmod(fd, 0644);  /* Enforce permissions regardless of umask */
    ssize_t w = write(fd, mode, strlen(mode));
    close(fd);
    return (w > 0) ? 0 : -1;
}

static int state_read(char *buf, size_t len) {
    int fd = open(STATE_PATH, O_RDONLY); if (fd < 0) return 0;
    ssize_t r = read(fd, buf, len-1); close(fd);
    if (r <= 0) return 0;
    buf[r] = '\0';
    while (r > 0 && isspace((unsigned char)buf[r-1])) buf[--r] = '\0';
    return 1;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Profile helpers
 * ─────────────────────────────────────────────────────────────────────────── */
static const char *profile_name(Profile p) {
    switch (p) {
        case PROFILE_POWERSAVE: return "powersave";
        case PROFILE_BALANCED: return "balanced";
        case PROFILE_PERFORMANCE: return "performance";
        case PROFILE_ULTRA: return "ultra";
        default: return "auto";
    }
}

static Profile profile_parse(const char *s) {
    if (strcmp(s, "powersave") == 0) return PROFILE_POWERSAVE;
    if (strcmp(s, "balanced") == 0) return PROFILE_BALANCED;
    if (strcmp(s, "performance") == 0) return PROFILE_PERFORMANCE;
    if (strcmp(s, "ultra") == 0) return PROFILE_ULTRA;
    if (strcmp(s, "auto") == 0) return PROFILE_AUTO;
    return PROFILE_INVALID;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Apply profile
 * ─────────────────────────────────────────────────────────────────────────── */
static int apply_profile(Profile p) {
    if (p == PROFILE_AUTO) {
        if (reset_mem() != 0) { fprintf(stderr, "error: failed to reset memory clocks\n"); return -1; }
        if (reset_gpu() != 0) { fprintf(stderr, "error: failed to reset GPU clocks\n"); return -1; }
        printf("Clocks unlocked (driver-managed)\n");
        return 0;
    }

    if (enable_persistence() != 0) { fprintf(stderr, "error: failed to enable persistence mode\n"); return -1; }

    /* Lock memory clock */
    int mem_clocks[MAX_CLOCKS];
    int n = get_mem_clocks(mem_clocks, MAX_CLOCKS);
    if (n <= 0) { fprintf(stderr, "error: failed to query memory clocks\n"); return -1; }

    int mem_target;
    if (p == PROFILE_PERFORMANCE || p == PROFILE_ULTRA) mem_target = mem_clocks[0];  /* highest */
    else if (p == PROFILE_BALANCED) mem_target = mem_clocks[n/2];                    /* middle */
    else mem_target = mem_clocks[n-1];                                               /* lowest */

    if (lock_mem(mem_target) != 0) { fprintf(stderr, "error: failed to lock memory to %d MHz\n", mem_target); return -1; }

    /* Ultra mode: lock GPU core clock to max */
    if (p == PROFILE_ULTRA) {
        int gpu_clocks[MAX_CLOCKS];
        int ng = get_gpu_clocks(gpu_clocks, MAX_CLOCKS);
        if (ng <= 0) { fprintf(stderr, "error: failed to query GPU clocks\n"); return -1; }
        if (lock_gpu(gpu_clocks[0]) != 0) { fprintf(stderr, "error: failed to lock GPU to %d MHz\n", gpu_clocks[0]); return -1; }
        printf("Memory: %d MHz | GPU Core: %d MHz (ultra)\n", mem_target, gpu_clocks[0]);
    } else {
        /* Not ultra - reset GPU clock to driver-managed */
        reset_gpu();  /* Best effort, ignore errors */
        printf("Memory locked to %d MHz (%s)\n", mem_target, profile_name(p));
    }
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Help
 * ─────────────────────────────────────────────────────────────────────────── */
static void print_help(void) {
    printf("nvflux %s — NVIDIA GPU clock manager\n\n"
           "Usage: nvflux <command>\n\n"
           "Commands:\n"
           "  powersave     Lock memory to lowest tier (audio fix)\n"
           "  balanced      Lock memory to mid tier\n"
           "  performance   Lock memory to highest tier\n"
           "  ultra         Lock memory + GPU core to max\n"
           "  auto          Unlock (driver-managed)\n"
           "  status        Show saved profile\n"
           "  clocks        Show current memory and GPU clocks\n"
           "  --restore     Re-apply saved profile\n"
           "  --help        Show this help\n"
           "  --version     Show version\n", VERSION);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Fuzzy match: suggest similar commands
 * ─────────────────────────────────────────────────────────────────────────── */
static const char *valid_cmds[] = {
    "powersave", "balanced", "performance", "ultra", "auto",
    "status", "clocks", "clock", "--restore", "--help", "--version", "-h", "-v",
    NULL
};

static int levenshtein(const char *s1, const char *s2) {
    size_t len1 = strlen(s1), len2 = strlen(s2);
    if (len1 > len2) { const char *t = s1; s1 = s2; s2 = t; size_t tl = len1; len1 = len2; len2 = tl; }
    if (len1 == 0) return (int)len2;
    
    unsigned int *prev = malloc((len2 + 1) * sizeof(unsigned int));
    if (!prev) return -1;
    unsigned int *curr = malloc((len2 + 1) * sizeof(unsigned int));
    if (!curr) { free(prev); return -1; }
    
    for (size_t j = 0; j <= len2; j++) prev[j] = (unsigned int)j;
    
    for (size_t i = 1; i <= len1; i++) {
        curr[0] = (unsigned int)i;
        for (size_t j = 1; j <= len2; j++) {
            unsigned int cost = (s1[i-1] == s2[j-1]) ? 0 : 1;
            unsigned int del = prev[j] + 1;
            unsigned int ins = curr[j-1] + 1;
            unsigned int sub = prev[j-1] + cost;
            curr[j] = del < ins ? del : ins;
            curr[j] = curr[j] < sub ? curr[j] : sub;
        }
        unsigned int *t = prev; prev = curr; curr = t;
    }
    int result = (int)prev[len2];
    free(prev); free(curr);
    return result;
}

static void suggest_command(const char *bad_cmd) {
    int best_dist = 999;
    const char *best_match = NULL;
    
    for (int i = 0; valid_cmds[i]; i++) {
        int dist = levenshtein(bad_cmd, valid_cmds[i]);
        if (dist >= 0 && dist < best_dist) {
            best_dist = dist;
            best_match = valid_cmds[i];
        }
    }
    
    fprintf(stderr, "error: unknown command '%s'\n", bad_cmd);
    if (best_match && best_dist <= 2) {
        fprintf(stderr, "did you mean '%s'?\n", best_match);
    }
    fprintf(stderr, "Run 'nvflux --help' for usage.\n");
}

/* ───────────────────────────────────────────────────────────────────────────
 * Main
 * ─────────────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) { print_help(); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) { print_help(); return 0; }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) { printf("nvflux %s\n", VERSION); return 0; }

    /* status: no root needed */
    if (strcmp(cmd, "status") == 0) {
        if (setuid(getuid()) != 0) {
            fprintf(stderr, "warning: failed to drop privileges\n");
        }
        char mode[64] = {0};
        if (state_read(mode, sizeof(mode))) {
            if (mode[0] >= 'a' && mode[0] <= 'z') mode[0] += 'A' - 'a';
            printf("%s\n", mode);
        } else printf("Auto\n");  /* Default is auto/driver-managed */
        return 0;
    }

    /* Find nvidia-smi */
    if (find_nvidia_smi() != 0) {
        fprintf(stderr, "error: nvidia-smi not found\n");
        return 2;
    }

    /* clock/clocks: no root needed */
    if (strcmp(cmd, "clock") == 0 || strcmp(cmd, "clocks") == 0) {
        if (setuid(getuid()) != 0) {
            fprintf(stderr, "warning: failed to drop privileges\n");
        }
        int mem = get_current_mem();
        int gpu = get_current_gpu();
        if (mem < 0) { fprintf(stderr, "error: failed to query memory clock\n"); return 1; }
        if (gpu < 0) { fprintf(stderr, "error: failed to query GPU clock\n"); return 1; }
        printf("Memory: %d MHz\n", mem);
        printf("GPU Core: %d MHz\n", gpu);
        return 0;
    }

    /* Everything else needs root */
    if (geteuid() != 0) {
        fprintf(stderr, "error: requires root (setuid bit should be set)\n");
        return 3;
    }

    /* --restore: read saved profile and apply */
    Profile p;
    if (strcmp(cmd, "--restore") == 0) {
        char mode[64] = {0};
        if (!state_read(mode, sizeof(mode))) { fprintf(stderr, "error: no saved profile\n"); return 1; }
        p = profile_parse(mode);
        if (p == PROFILE_INVALID) { fprintf(stderr, "error: invalid saved profile '%s'\n", mode); return 1; }
    } else {
        p = profile_parse(cmd);
        if (p == PROFILE_INVALID) { suggest_command(cmd); return 4; }
    }

    if (apply_profile(p) != 0) return 1;
    if (state_write(profile_name(p)) != 0) {
        fprintf(stderr, "warning: failed to save profile state\n");
    }
    if (setuid(getuid()) != 0) {
        fprintf(stderr, "warning: failed to drop privileges\n");
    }
    return 0;
}
