#ifndef NVFLUX_STATE_H
#define NVFLUX_STATE_H

#include <stddef.h>

#include "policy.h"

/*
 * What nvflux should apply: one request for every GPU, plus optional
 * per-GPU overrides keyed by UUID (so adding or reordering cards never applies
 * a setting to the wrong one).
 *
 * Two files use this format:
 *   the saved state, which reload applies at boot and after sleep;
 *   the temporary state in /run, written by --temporary. /run is emptied at
 *   every boot, and reload removes it.
 *
 *   # nvflux state, format 1. Written by nvflux; edit with care.
 *   mode=balanced
 *   GPU-0726ca8b-....mode=custom
 *   GPU-0726ca8b-....memory=5001
 *   GPU-0726ca8b-....graphics=900-1800
 */

#define STATE_MAX_GPUS 16
#define STATE_UUID_LEN 96
#define STATE_MAX_SIZE 4096
#define STATE_MHZ_MAX  100000

struct gpu_setting {
    char uuid[STATE_UUID_LEN];
    struct request req;
};

struct state {
    struct request all;
    struct gpu_setting gpu[STATE_MAX_GPUS];
    unsigned ngpu;
};

enum {
    STATE_LOADED     = 1,
    STATE_NONE       = 0,  /* no file */
    STATE_UNREADABLE = -1, /* errno is set */
    STATE_INVALID    = -2,
};

void state_init(struct state *s);
const struct request *state_for(const struct state *s, const char *uuid);
/* Sets the request for every GPU and drops all per-GPU overrides. */
void state_set_all(struct state *s, const struct request *r);
/* Returns -1 if there is no room for another GPU. */
int state_set_gpu(struct state *s, const char *uuid, const struct request *r);
int state_has_override(const struct state *s, const char *uuid);

/* Parses either format; old nvflux files (bare words such as "powersave" or
 * "lock 5001") are converted by behaviour and *migrated is set. */
int state_parse(const char *text, size_t len, struct state *s, int *migrated);
/* Returns 0, or -1 if buf is too small. */
int state_format(const struct state *s, char *buf, size_t len);

int state_load(const char *path, struct state *s, int *migrated);
int state_save(const char *path, const struct state *s); /* atomic; errno on failure */
int state_remove(const char *path);

const char *state_saved_path(void);
const char *state_manifest_path(void); /* written by install.sh */
const char *state_temp_path(void);

int state_valid_uuid(const char *uuid);

#endif
