#define _GNU_SOURCE
#include "profile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef STATE_DIR
#define STATE_DIR "/var/lib/nvflux"
#endif
#define STATE_FILE STATE_DIR "/state"

#define MHZ_MAX 100000

static const char *const mode_names[] = {
    [MODE_AUTO]        = "auto",
    [MODE_POWERSAVE]   = "powersave",
    [MODE_BALANCED]    = "balanced",
    [MODE_PERFORMANCE] = "performance",
    [MODE_LOCK]        = "lock",
};

static int parse_mhz(const char *s, unsigned *mhz)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *end || s[0] == '-' || v == 0 || v > MHZ_MAX)
        return -1;
    *mhz = (unsigned)v;
    return 0;
}

int profile_parse(int argc, char *const argv[], struct profile *p)
{
    if (argc < 1)
        return PROFILE_UNKNOWN;

    for (size_t m = 0; m < sizeof(mode_names) / sizeof(*mode_names); m++) {
        if (strcmp(argv[0], mode_names[m]) != 0)
            continue;

        p->mode = (enum mode)m;
        p->mhz = 0;
        p->force = 0;
        if (p->mode != MODE_LOCK)
            return argc == 1 ? PROFILE_OK : PROFILE_BAD_ARGS;

        /* lock <MHz> [--force], with --force allowed on either side */
        int have_mhz = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--force") && !p->force)
                p->force = 1;
            else if (!have_mhz && parse_mhz(argv[i], &p->mhz) == 0)
                have_mhz = 1;
            else
                return PROFILE_BAD_ARGS;
        }
        return have_mhz ? PROFILE_OK : PROFILE_BAD_ARGS;
    }
    return PROFILE_UNKNOWN;
}

void profile_format(const struct profile *p, char *buf, size_t len)
{
    if (p->mode == MODE_LOCK)
        snprintf(buf, len, "lock %u%s", p->mhz, p->force ? " --force" : "");
    else
        snprintf(buf, len, "%s", mode_names[p->mode]);
}

const char *state_path(void)
{
    return STATE_FILE;
}

int state_load(struct profile *p)
{
    int fd = open(STATE_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? STATE_NONE : STATE_UNREADABLE;

    char buf[64];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len < 0)
        return STATE_UNREADABLE;
    buf[len] = '\0';

    char *words[3], *save = NULL;
    int n = 0;
    for (char *w = strtok_r(buf, " \t\r\n", &save); w; w = strtok_r(NULL, " \t\r\n", &save)) {
        if (n == 3)
            return STATE_INVALID;
        words[n++] = w;
    }
    return profile_parse(n, words, p) == PROFILE_OK ? STATE_LOADED : STATE_INVALID;
}

/* Write a temporary file and rename it over the old one, so the saved
 * profile is never left half-written. */
int state_save(const struct profile *p)
{
    static const char tmp[] = STATE_FILE ".tmp";
    char text[40];
    profile_format(p, text, sizeof(text));
    strcat(text, "\n");

    if (mkdir(STATE_DIR, 0755) != 0 && errno != EEXIST)
        return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;

    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len && fsync(fd) == 0;
    if (close(fd) != 0 || !ok || rename(tmp, STATE_FILE) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    return 0;
}
