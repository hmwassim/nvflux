#ifndef NVFLUX_CLI_H
#define NVFLUX_CLI_H

#include <stddef.h>

#include "policy.h"

enum cmd_kind {
    CMD_STATUS,
    CMD_MODE,
    CMD_LOCK,
    CMD_RELOAD,
    CMD_INFO,
    CMD_DOCTOR,
    CMD_HELP,
    CMD_VERSION,
};

struct cmd {
    enum cmd_kind kind;
    enum mode mode;          /* CMD_MODE */
    const char *alias;       /* the word typed, if it was an alias */
    int old_lock;            /* the old 'lock <MHz>' form was used */

    int lock_mem;            /* CMD_LOCK */
    unsigned mem;
    int lock_gfx;
    unsigned gfx_min, gfx_max;

    int force;
    int temporary;
    int json;
    int gpu_set;
    unsigned gpu;
    unsigned wait;           /* reload: seconds to wait for the driver */
};

/* Fills cmd, or writes a message (with a fix) into err and returns -1. */
int cli_parse(int argc, char **argv, struct cmd *cmd, char *err, size_t errlen);

/* Whether the command changes clocks (and so needs root). */
int cli_needs_root(const struct cmd *cmd);

#endif
