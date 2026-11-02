#define _GNU_SOURCE
#include "cli.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "common.h"

struct word {
    const char *name;
    enum cmd_kind kind;
    enum mode mode;
    int alias; /* accepted, but the output teaches the real name */
};

static const struct word words[] = {
    { "balanced",    CMD_MODE,    MODE_BALANCED,  0 },
    { "powersave",   CMD_MODE,    MODE_POWERSAVE, 0 },
    { "max",         CMD_MODE,    MODE_MAX,       0 },
    { "off",         CMD_MODE,    MODE_OFF,       0 },
    { "lock",        CMD_LOCK,    MODE_CUSTOM,    0 },
    { "status",      CMD_STATUS,  MODE_OFF,       0 },
    { "info",        CMD_INFO,    MODE_OFF,       0 },
    { "doctor",      CMD_DOCTOR,  MODE_OFF,       0 },
    { "reload",      CMD_RELOAD,  MODE_OFF,       0 },
    { "help",        CMD_HELP,    MODE_OFF,       0 },
    { "version",     CMD_VERSION, MODE_OFF,       0 },
    { "performance", CMD_MODE,    MODE_MAX,       1 },
    { "power-saver", CMD_MODE,    MODE_POWERSAVE, 1 },
    { "auto",        CMD_MODE,    MODE_OFF,       1 },
    { "reset",       CMD_MODE,    MODE_OFF,       1 },
    { "default",     CMD_MODE,    MODE_OFF,       1 },
    { "restore",     CMD_RELOAD,  MODE_OFF,       1 }, /* the old nvflux's name */
};

int cli_needs_root(const struct cmd *c)
{
    return c->kind == CMD_MODE || c->kind == CMD_LOCK || c->kind == CMD_RELOAD;
}

static unsigned edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned row[32];
    if (la >= 32 || lb >= 32)
        return ~0u;
    for (size_t j = 0; j <= lb; j++)
        row[j] = (unsigned)j;
    for (size_t i = 1; i <= la; i++) {
        unsigned diag = row[0];
        row[0] = (unsigned)i;
        for (size_t j = 1; j <= lb; j++) {
            unsigned up = row[j];
            unsigned best = diag + (tolower((unsigned char)a[i - 1]) != b[j - 1]);
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

static const char *suggest(const char *w)
{
    const char *best = NULL;
    unsigned best_d = 3; /* only suggest close matches */
    for (size_t i = 0; i < ARRAY_LEN(words); i++) {
        if (words[i].alias)
            continue;
        unsigned d = edit_distance(w, words[i].name);
        if (d < best_d) {
            best_d = d;
            best = words[i].name;
        }
    }
    return best;
}

/* A clock in MHz: digits only, 1..100000. */
static int parse_mhz(const char *s, unsigned *out)
{
    if (!*s || strlen(s) > 6)
        return -1;
    unsigned long v = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return -1;
        v = v * 10 + (unsigned long)(*p - '0');
    }
    if (v == 0 || v > 100000)
        return -1;
    *out = (unsigned)v;
    return 0;
}

/* A small number such as a GPU index or seconds: digits only, 0..999. */
static int parse_count(const char *s, unsigned *out)
{
    if (!*s || strlen(s) > 3)
        return -1;
    unsigned v = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return -1;
        v = v * 10 + (unsigned)(*p - '0');
    }
    *out = v;
    return 0;
}

static int parse_range(const char *s, unsigned *min, unsigned *max)
{
    char buf[16];
    if (strlen(s) >= sizeof(buf))
        return -1;
    strcpy(buf, s);
    char *dash = strchr(buf, '-');
    if (!dash)
        return parse_mhz(buf, min) == 0 ? (*max = *min, 0) : -1;
    *dash = '\0';
    return parse_mhz(buf, min) == 0 && parse_mhz(dash + 1, max) == 0 ? 0 : -1;
}

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

static const char *real_name(const struct cmd *c)
{
    for (size_t i = 0; i < ARRAY_LEN(words); i++)
        if (!words[i].alias && words[i].kind == c->kind &&
            (c->kind != CMD_MODE || words[i].mode == c->mode))
            return words[i].name;
    return "?";
}

int cli_parse(int argc, char **argv, struct cmd *c, char *err, size_t errlen)
{
    memset(c, 0, sizeof(*c));
    c->kind = CMD_STATUS;
    int have_cmd = 0;
    const char *lock_part = NULL; /* "memory"/"graphics" awaiting its value */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (lock_part) {
            if (!strcmp(lock_part, "memory")) {
                if (parse_mhz(a, &c->mem) != 0)
                    FAIL("'%s' is not a clock in MHz; see 'nvflux info'", a);
                c->lock_mem = 1;
            } else {
                if (parse_range(a, &c->gfx_min, &c->gfx_max) != 0)
                    FAIL("'%s' is not a clock or range in MHz (e.g. 1500 or 900-1800)", a);
                if (c->gfx_min > c->gfx_max)
                    FAIL("range is reversed; use 'nvflux lock graphics %u-%u'", c->gfx_max, c->gfx_min);
                c->lock_gfx = 1;
            }
            lock_part = NULL;
            continue;
        }

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            c->kind = CMD_HELP;
            return 0;
        }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            c->kind = CMD_VERSION;
            return 0;
        }
        if (!strcmp(a, "--temporary")) { c->temporary = 1; continue; }
        if (!strcmp(a, "--force"))     { c->force = 1; continue; }
        if (!strcmp(a, "--json"))      { c->json = 1; continue; }
        if (!strcmp(a, "--gpu") || !strcmp(a, "--wait")) {
            unsigned v;
            if (i + 1 >= argc || parse_count(argv[i + 1], &v) != 0) {
                if (!strcmp(a, "--gpu"))
                    FAIL("--gpu needs a GPU number; see 'nvflux status'");
                FAIL("--wait needs a number of seconds");
            }
            if (!strcmp(a, "--gpu")) {
                c->gpu_set = 1;
                c->gpu = v;
            } else {
                c->wait = v > 60 ? 60 : v;
            }
            i++;
            continue;
        }
        if (a[0] == '-')
            FAIL("unknown option '%s'; see 'nvflux help'", a);

        if (have_cmd && c->kind == CMD_LOCK) {
            /* The old nvflux's 'lock <MHz>' meant the memory clock. */
            if (!c->lock_mem && !c->lock_gfx && parse_mhz(a, &c->mem) == 0) {
                c->lock_mem = 1;
                c->old_lock = 1;
                continue;
            }
            if (!strcasecmp(a, "memory") || !strcasecmp(a, "mem")) { lock_part = "memory"; continue; }
            if (!strcasecmp(a, "graphics") || !strcasecmp(a, "gpu") || !strcasecmp(a, "core")) {
                lock_part = "graphics";
                continue;
            }
            FAIL("cannot lock '%s'; use 'lock memory' or 'lock graphics'", a);
        }
        if (have_cmd)
            FAIL("unexpected '%s' after '%s'; see 'nvflux help'", a, real_name(c));

        size_t w = 0;
        while (w < ARRAY_LEN(words) && strcasecmp(a, words[w].name) != 0)
            w++;
        if (w == ARRAY_LEN(words)) {
            const char *s = suggest(a);
            if (s)
                FAIL("unknown command '%s'; did you mean '%s'?", a, s);
            FAIL("unknown command '%s'; see 'nvflux help'", a);
        }
        have_cmd = 1;
        c->kind = words[w].kind;
        c->mode = words[w].mode;
        if (words[w].alias)
            c->alias = words[w].name;
    }

    if (lock_part)
        FAIL("'lock %s' needs a clock in MHz", lock_part);
    if (c->kind == CMD_LOCK && !c->lock_mem && !c->lock_gfx)
        FAIL("use 'lock memory <MHz>' or 'lock graphics <MHz>'");

    /* Options only make sense with some commands. */
    int changes = c->kind == CMD_MODE || c->kind == CMD_LOCK;
    if (c->temporary && !changes)
        FAIL("--temporary only works with a mode or lock");
    if (c->gpu_set && !changes)
        FAIL("--gpu only works with a mode or lock");
    if (c->force && c->kind != CMD_LOCK)
        FAIL("--force only works with lock");
    if (c->json && c->kind != CMD_STATUS && c->kind != CMD_INFO)
        FAIL("--json only works with status and info");
    if (c->wait && c->kind != CMD_RELOAD)
        FAIL("--wait only works with reload");
    return 0;
}
