#ifndef NVFLUX_PROFILE_H
#define NVFLUX_PROFILE_H

#include <stddef.h>

enum mode {
    MODE_AUTO,        /* memory clock managed by the driver */
    MODE_POWERSAVE,   /* lowest memory clock that doesn't cap the core */
    MODE_BALANCED,    /* middle memory clock that doesn't cap the core */
    MODE_PERFORMANCE, /* highest supported memory clock */
    MODE_LOCK,        /* supported memory clock nearest to mhz */
};

struct profile {
    enum mode mode;
    unsigned mhz; /* MODE_LOCK only */
    int force;    /* MODE_LOCK only: allow a clock that caps the core */
};

enum {
    PROFILE_OK = 0,
    PROFILE_UNKNOWN = -1, /* not a profile name */
    PROFILE_BAD_ARGS = -2, /* a profile name with missing or invalid arguments */
};

/* Parse the words of a profile, e.g. {"powersave"} or {"lock", "405", "--force"}.
 * The same words are used on the command line and in the state file. */
int profile_parse(int argc, char *const argv[], struct profile *p);
void profile_format(const struct profile *p, char *buf, size_t len);

enum {
    STATE_LOADED = 1,
    STATE_NONE = 0,
    STATE_UNREADABLE = -1,
    STATE_INVALID = -2,
};

int state_load(struct profile *p);
int state_save(const struct profile *p);
const char *state_path(void);

#endif
