#ifndef NVFLUX_COMMON_H
#define NVFLUX_COMMON_H

#ifndef VERSION
#define VERSION "unknown"
#endif

/* Exit codes are part of the CLI contract (scripts and the future tray rely
 * on them), so they never change meaning. */
enum exit_code {
    EXIT_OK          = 0,
    EXIT_FAIL        = 1, /* GPU or driver error, or a refused value */
    EXIT_USAGE       = 2,
    EXIT_PERMISSION  = 3, /* not in the nvflux group, setuid bit missing */
    EXIT_UNSUPPORTED = 4, /* this GPU or driver can't do it */
    EXIT_PARTIAL     = 5, /* applied to some GPUs, not all */
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof(*(a)))

#endif
