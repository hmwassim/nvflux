#define _GNU_SOURCE
#include "state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef STATE_DIR
#define STATE_DIR "/var/lib/nvflux"
#endif
#ifndef RUN_DIR
#define RUN_DIR "/run/nvflux"
#endif

#define HEADER "# nvflux state, format 1. Written by nvflux; edit with care.\n"

const char *state_saved_path(void) { return STATE_DIR "/state"; }
const char *state_temp_path(void) { return RUN_DIR "/current"; }
const char *state_manifest_path(void) { return STATE_DIR "/installed"; }

void state_init(struct state *s)
{
    memset(s, 0, sizeof(*s));
    s->all.mode = MODE_OFF;
}

static int find_gpu(const struct state *s, const char *uuid)
{
    for (unsigned i = 0; i < s->ngpu; i++)
        if (strcmp(s->gpu[i].uuid, uuid) == 0)
            return (int)i;
    return -1;
}

const struct request *state_for(const struct state *s, const char *uuid)
{
    int i = uuid ? find_gpu(s, uuid) : -1;
    return i >= 0 ? &s->gpu[i].req : &s->all;
}

int state_has_override(const struct state *s, const char *uuid)
{
    return find_gpu(s, uuid) >= 0;
}

void state_set_all(struct state *s, const struct request *r)
{
    s->all = *r;
    s->ngpu = 0;
}

int state_set_gpu(struct state *s, const char *uuid, const struct request *r)
{
    int i = find_gpu(s, uuid);
    if (i < 0) {
        if (s->ngpu == STATE_MAX_GPUS || !state_valid_uuid(uuid))
            return -1;
        i = (int)s->ngpu++;
        snprintf(s->gpu[i].uuid, sizeof(s->gpu[i].uuid), "%s", uuid);
    }
    s->gpu[i].req = *r;
    return 0;
}

int state_valid_uuid(const char *uuid)
{
    size_t n = strlen(uuid);
    if (n < 5 || n >= STATE_UUID_LEN || strncmp(uuid, "GPU-", 4) != 0)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = uuid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-'))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */

static int parse_mhz(const char *s, unsigned *out)
{
    if (*s < '1' || *s > '9')
        return -1;
    unsigned long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (unsigned long)(*s - '0');
        if (v > STATE_MHZ_MAX)
            return -1;
    }
    *out = (unsigned)v;
    return 0;
}

/* "1500" or "900-1800" */
static int parse_range(const char *s, unsigned *min, unsigned *max)
{
    char buf[32];
    if (strlen(s) >= sizeof(buf))
        return -1;
    strcpy(buf, s);
    char *dash = strchr(buf, '-');
    if (!dash)
        return parse_mhz(buf, min) == 0 ? (*max = *min, 0) : -1;
    *dash = '\0';
    if (parse_mhz(buf, min) != 0 || parse_mhz(dash + 1, max) != 0 || *min > *max)
        return -1;
    return 0;
}

static int parse_mode(const char *s, enum mode *m)
{
    static const enum mode modes[] = { MODE_OFF, MODE_BALANCED, MODE_POWERSAVE, MODE_MAX, MODE_CUSTOM };
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); i++)
        if (strcmp(s, mode_name(modes[i])) == 0) {
            *m = modes[i];
            return 0;
        }
    return -1;
}

/* Which keys were seen for one request, to reject duplicates and stray keys. */
enum { K_MODE = 1, K_MEM = 2, K_MEMF = 4, K_GFX = 8, K_GFXF = 16 };

struct entry {
    struct request req;
    unsigned seen;
};

static int set_key(struct entry *e, const char *key, const char *val)
{
    unsigned bit;
    if (strcmp(key, "mode") == 0) {
        bit = K_MODE;
        if (parse_mode(val, &e->req.mode) != 0)
            return -1;
    } else if (strcmp(key, "memory") == 0) {
        bit = K_MEM;
        if (parse_mhz(val, &e->req.mem) != 0)
            return -1;
    } else if (strcmp(key, "graphics") == 0) {
        bit = K_GFX;
        if (parse_range(val, &e->req.gfx_min, &e->req.gfx_max) != 0)
            return -1;
    } else if (strcmp(key, "memory_force") == 0 || strcmp(key, "graphics_force") == 0) {
        bit = key[0] == 'm' ? K_MEMF : K_GFXF;
        if (strcmp(val, "1") != 0)
            return -1;
        if (bit == K_MEMF)
            e->req.mem_force = 1;
        else
            e->req.gfx_force = 1;
    } else {
        return -1;
    }
    if (e->seen & bit)
        return -1;
    e->seen |= bit;
    return 0;
}

/* A request must be complete and consistent: a mode, and expert values only
 * for custom (which needs at least one of them). */
static int check_entry(const struct entry *e)
{
    if (!(e->seen & K_MODE))
        return -1;
    int custom = e->req.mode == MODE_CUSTOM;
    if (!custom && (e->seen & (K_MEM | K_MEMF | K_GFX | K_GFXF)))
        return -1;
    if (custom && !(e->seen & (K_MEM | K_GFX)))
        return -1;
    if (((e->seen & K_MEMF) && !(e->seen & K_MEM)) || ((e->seen & K_GFXF) && !(e->seen & K_GFX)))
        return -1;
    return 0;
}

/* The old nvflux stored bare words: "powersave", "lock 5001 --force", ...
 * They're converted by behaviour, not by name: old "powersave" only held the
 * memory clock low, which is what "balanced" does now. */
static int parse_old(char *text, struct state *s)
{
    char *words[4], *save = NULL;
    int n = 0;
    for (char *w = strtok_r(text, " \t\r\n", &save); w; w = strtok_r(NULL, " \t\r\n", &save)) {
        if (n == 3)
            return STATE_INVALID;
        words[n++] = w;
    }
    if (n == 0)
        return STATE_INVALID;

    struct request r = { .mode = MODE_OFF };
    if (n == 1 && (!strcmp(words[0], "powersave") || !strcmp(words[0], "balanced")))
        r.mode = MODE_BALANCED;
    else if (n == 1 && !strcmp(words[0], "performance"))
        r.mode = MODE_MAX;
    else if (n == 1 && !strcmp(words[0], "auto"))
        r.mode = MODE_OFF;
    else if (n >= 2 && !strcmp(words[0], "lock")) {
        r.mode = MODE_CUSTOM;
        int have_mhz = 0;
        for (int i = 1; i < n; i++) {
            if (!strcmp(words[i], "--force") && !r.mem_force)
                r.mem_force = 1;
            else if (!have_mhz && parse_mhz(words[i], &r.mem) == 0)
                have_mhz = 1;
            else
                return STATE_INVALID;
        }
        if (!have_mhz)
            return STATE_INVALID;
    } else {
        return STATE_INVALID;
    }
    state_set_all(s, &r);
    return STATE_LOADED;
}

static int parse_new(char *text, struct state *s)
{
    struct entry all = { .req = { .mode = MODE_OFF } };
    struct entry gpus[STATE_MAX_GPUS];
    char uuids[STATE_MAX_GPUS][STATE_UUID_LEN];
    unsigned ngpu = 0;
    char *save = NULL;

    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            return STATE_INVALID;
        *eq = '\0';
        char *key = line, *val = eq + 1;

        struct entry *e = &all;
        char *dot = strrchr(key, '.');
        if (dot) {
            *dot = '\0';
            if (!state_valid_uuid(key))
                return STATE_INVALID;
            unsigned i = 0;
            while (i < ngpu && strcmp(uuids[i], key) != 0)
                i++;
            if (i == ngpu) {
                if (ngpu == STATE_MAX_GPUS)
                    return STATE_INVALID;
                snprintf(uuids[i], sizeof(uuids[i]), "%s", key);
                memset(&gpus[i], 0, sizeof(gpus[i]));
                ngpu++;
            }
            e = &gpus[i];
            key = dot + 1;
        }
        if (set_key(e, key, val) != 0)
            return STATE_INVALID;
    }

    if (all.seen && check_entry(&all) != 0)
        return STATE_INVALID;
    state_set_all(s, &all.req);
    for (unsigned i = 0; i < ngpu; i++) {
        if (check_entry(&gpus[i]) != 0)
            return STATE_INVALID;
        state_set_gpu(s, uuids[i], &gpus[i].req);
    }
    return STATE_LOADED;
}

int state_parse(const char *text, size_t len, struct state *s, int *migrated)
{
    state_init(s);
    if (migrated)
        *migrated = 0;
    if (len >= STATE_MAX_SIZE || memchr(text, '\0', len))
        return STATE_INVALID;

    char buf[STATE_MAX_SIZE];
    memcpy(buf, text, len);
    buf[len] = '\0';

    for (size_t i = 0; i < len; i++)
        if ((unsigned char)buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\t' && buf[i] != '\r')
            return STATE_INVALID;

    int rc;
    if (strchr(buf, '=')) {
        for (char *p = buf; *p; p++) /* the new format has no tabs or CRs */
            if (*p == '\t' || *p == '\r')
                return STATE_INVALID;
        rc = parse_new(buf, s);
    } else {
        rc = parse_old(buf, s);
        if (rc == STATE_LOADED && migrated)
            *migrated = 1;
    }
    if (rc != STATE_LOADED)
        state_init(s);
    return rc;
}

/* ------------------------------------------------------------------------ */

__attribute__((format(printf, 3, 4)))
static int put(char **p, size_t *left, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, *left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= *left)
        return -1;
    *p += n;
    *left -= (size_t)n;
    return 0;
}

static int format_request(char **p, size_t *left, const char *prefix, const struct request *r)
{
    const char *dot = prefix[0] ? "." : "";
    if (put(p, left, "%s%smode=%s\n", prefix, dot, mode_name(r->mode)) != 0)
        return -1;
    if (r->mode != MODE_CUSTOM)
        return 0;
    if (r->mem && put(p, left, "%s%smemory=%u\n", prefix, dot, r->mem) != 0)
        return -1;
    if (r->mem && r->mem_force && put(p, left, "%s%smemory_force=1\n", prefix, dot) != 0)
        return -1;
    if (r->gfx_max) {
        int rc = r->gfx_min == r->gfx_max
                     ? put(p, left, "%s%sgraphics=%u\n", prefix, dot, r->gfx_max)
                     : put(p, left, "%s%sgraphics=%u-%u\n", prefix, dot, r->gfx_min, r->gfx_max);
        if (rc != 0 || (r->gfx_force && put(p, left, "%s%sgraphics_force=1\n", prefix, dot) != 0))
            return -1;
    }
    return 0;
}

int state_format(const struct state *s, char *buf, size_t len)
{
    char *p = buf;
    size_t left = len;
    if (put(&p, &left, "%s", HEADER) != 0 || format_request(&p, &left, "", &s->all) != 0)
        return -1;
    for (unsigned i = 0; i < s->ngpu; i++)
        if (format_request(&p, &left, s->gpu[i].uuid, &s->gpu[i].req) != 0)
            return -1;
    return 0;
}

int state_load(const char *path, struct state *s, int *migrated)
{
    state_init(s);
    if (migrated)
        *migrated = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? STATE_NONE : STATE_UNREADABLE;

    char buf[STATE_MAX_SIZE];
    ssize_t len = read(fd, buf, sizeof(buf));
    int saved = errno;
    close(fd);
    if (len < 0) {
        errno = saved;
        return STATE_UNREADABLE;
    }
    return state_parse(buf, (size_t)len, s, migrated);
}

static int make_parent(const char *path)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return 0;
    *slash = '\0';
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Write a temporary file and rename it over the old one, so the state is
 * never left half-written. */
int state_save(const char *path, const struct state *s)
{
    char text[STATE_MAX_SIZE], tmp[300];
    if (state_format(s, text, sizeof(text)) != 0) {
        errno = ENOSPC;
        return -1;
    }
    if (make_parent(path) != 0)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len && fsync(fd) == 0;
    if (close(fd) != 0 || !ok || rename(tmp, path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    return 0;
}

int state_remove(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT)
        return -1;
    return 0;
}
