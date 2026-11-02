#include "state.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"

#define UUID "GPU-0726ca8b-8dbb-9075-bd95-645ebcdf678f"

static int parse(const char *text, struct state *s, int *migrated)
{
    return state_parse(text, strlen(text), s, migrated);
}

static void roundtrip(const struct state *in)
{
    char buf[STATE_MAX_SIZE];
    struct state out;
    CHECK_EQ(state_format(in, buf, sizeof(buf)), 0);
    CHECK_EQ(parse(buf, &out, NULL), STATE_LOADED);
    CHECK_EQ(memcmp(&in->all, &out.all, sizeof(in->all)), 0);
    CHECK_EQ(in->ngpu, out.ngpu);
    for (unsigned i = 0; i < in->ngpu && i < out.ngpu; i++) {
        CHECK_STR(in->gpu[i].uuid, out.gpu[i].uuid);
        CHECK_EQ(memcmp(&in->gpu[i].req, &out.gpu[i].req, sizeof(struct request)), 0);
    }
}

static void test_roundtrip(void)
{
    struct state s;
    state_init(&s);
    roundtrip(&s);

    state_set_all(&s, &(struct request){ .mode = MODE_BALANCED });
    roundtrip(&s);

    state_set_all(&s, &(struct request){ .mode = MODE_CUSTOM, .mem = 5001, .mem_force = 1,
                                         .gfx_min = 900, .gfx_max = 1800, .gfx_force = 1 });
    roundtrip(&s);

    state_set_all(&s, &(struct request){ .mode = MODE_CUSTOM, .gfx_min = 1500, .gfx_max = 1500 });
    roundtrip(&s);

    state_set_all(&s, &(struct request){ .mode = MODE_POWERSAVE });
    CHECK_EQ(state_set_gpu(&s, UUID, &(struct request){ .mode = MODE_MAX }), 0);
    CHECK_EQ(state_set_gpu(&s, "GPU-1111", &(struct request){ .mode = MODE_CUSTOM, .mem = 810 }), 0);
    roundtrip(&s);
    CHECK_EQ(state_for(&s, UUID)->mode, MODE_MAX);
    CHECK_EQ(state_for(&s, "GPU-2222")->mode, MODE_POWERSAVE);
    CHECK_EQ(state_set_gpu(&s, "not-a-uuid", &(struct request){ .mode = MODE_MAX }), -1);
    CHECK_EQ(state_set_gpu(&s, "GPU-a/b", &(struct request){ .mode = MODE_MAX }), -1);
}

static void test_parse(void)
{
    struct state s;
    CHECK_EQ(parse("# comment\nmode=max\n", &s, NULL), STATE_LOADED);
    CHECK_EQ(s.all.mode, MODE_MAX);
    CHECK_EQ(parse("mode=custom\ngraphics=900-1800\n", &s, NULL), STATE_LOADED);
    CHECK_EQ(s.all.gfx_min, 900);
    CHECK_EQ(s.all.gfx_max, 1800);
    CHECK_EQ(parse(UUID ".mode=balanced\n", &s, NULL), STATE_LOADED);
    CHECK_EQ(s.all.mode, MODE_OFF);
    CHECK_EQ(s.ngpu, 1);
    CHECK_EQ(parse("", &s, NULL), STATE_INVALID);       /* old format, no words */
    CHECK_EQ(parse("# only a comment\nmode=off", &s, NULL), STATE_LOADED);

    /* Rejected: anything not exactly right. */
    static const char *bad[] = {
        "mode=turbo\n",
        "mode=max\nmode=max\n",                 /* duplicate */
        "mode=max\nmemory=5001\n",              /* expert value without custom */
        "mode=custom\n",                        /* custom without values */
        "mode=custom\nmemory_force=1\ngraphics=900\n", /* force without its value */
        "mode=custom\nmemory=0\n",
        "mode=custom\nmemory=-5\n",
        "mode=custom\nmemory=5001x\n",
        "mode=custom\nmemory=999999\n",
        "mode=custom\ngraphics=1800-900\n",
        "mode=custom\ngraphics=900-\n",
        "mode=custom\nmemory=5001\nmemory_force=yes\n",
        "colour=blue\n",
        "mode = max\n",
        "mode=max\r\n",
        "GPU-../../x.mode=max\n",
        "MIG-1234.mode=max\n",
        "mode=max\n" UUID ".memory=5001\n",      /* override without a mode */
        "garbage",
        "lock\n",
        "lock abc\n",
        "lock 405 --force --force\n",
        "lock 405 810\n",
        "powersave extra\n",
        "ultra\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        int rc = parse(bad[i], &s, NULL);
        if (rc != STATE_INVALID)
            fprintf(stderr, "accepted bad state: \"%s\"\n", bad[i]);
        CHECK_EQ(rc, STATE_INVALID);
        CHECK_EQ(s.all.mode, MODE_OFF); /* a rejected file leaves nothing behind */
        CHECK_EQ(s.ngpu, 0);
    }

    /* Embedded NUL and oversize input. */
    CHECK_EQ(state_parse("mode=max\0x", 10, &s, NULL), STATE_INVALID);
    char *big = malloc(STATE_MAX_SIZE + 10);
    memset(big, '#', STATE_MAX_SIZE + 9);
    big[STATE_MAX_SIZE + 9] = '\0';
    CHECK_EQ(parse(big, &s, NULL), STATE_INVALID);
    free(big);
}

/* Old nvflux files are converted by behaviour, not by name. */
static void test_migration(void)
{
    struct state s;
    int migrated;
    CHECK_EQ(parse("powersave\n", &s, &migrated), STATE_LOADED);
    CHECK_EQ(migrated, 1);
    CHECK_EQ(s.all.mode, MODE_BALANCED); /* old powersave only held memory low */
    CHECK_EQ(parse("balanced\n", &s, &migrated), STATE_LOADED);
    CHECK_EQ(s.all.mode, MODE_BALANCED);
    CHECK_EQ(parse("performance\n", &s, &migrated), STATE_LOADED);
    CHECK_EQ(s.all.mode, MODE_MAX);
    CHECK_EQ(parse("auto\n", &s, &migrated), STATE_LOADED);
    CHECK_EQ(s.all.mode, MODE_OFF);
    CHECK_EQ(parse("lock 5001\n", &s, &migrated), STATE_LOADED);
    CHECK_EQ(s.all.mode, MODE_CUSTOM);
    CHECK_EQ(s.all.mem, 5001);
    CHECK_EQ(s.all.mem_force, 0);
    CHECK_EQ(parse("lock --force 405\n", &s, &migrated), STATE_LOADED);
    CHECK_EQ(s.all.mem, 405);
    CHECK_EQ(s.all.mem_force, 1);
    CHECK_EQ(parse("mode=max\n", &s, &migrated), STATE_LOADED);
    CHECK_EQ(migrated, 0);
}

/* A cheap fuzz pass: random bytes and mutations of valid files must never
 * crash, and anything accepted must survive a format/parse round trip. */
static void test_fuzz(void)
{
    static const char *seeds[] = {
        "mode=balanced\n",
        "mode=custom\nmemory=5001\nmemory_force=1\ngraphics=900-1800\n",
        UUID ".mode=custom\n" UUID ".graphics=1500\nmode=max\n",
        "lock 405 --force\n",
    };
    srand(12345);
    char buf[512];
    for (int iter = 0; iter < 200000; iter++) {
        size_t len;
        if (iter % 3 == 0) {
            len = (size_t)(rand() % (int)sizeof(buf));
            for (size_t i = 0; i < len; i++)
                buf[i] = (char)(rand() % 256);
        } else {
            const char *seed = seeds[rand() % 4];
            len = strlen(seed);
            memcpy(buf, seed, len);
            for (int m = rand() % 4; m >= 0; m--) {
                size_t at = (size_t)rand() % len;
                buf[at] = "=-.\n0123456789abcdefmoderyGPU# \t"[rand() % 31];
            }
        }
        struct state s;
        if (state_parse(buf, len, &s, NULL) == STATE_LOADED) {
            char out[STATE_MAX_SIZE];
            struct state again;
            if (state_format(&s, out, sizeof(out)) != 0 ||
                state_parse(out, strlen(out), &again, NULL) != STATE_LOADED ||
                memcmp(&s.all, &again.all, sizeof(s.all)) != 0 || s.ngpu != again.ngpu) {
                failures++;
                fprintf(stderr, "fuzz: round trip failed for \"%.*s\"\n", (int)len, buf);
                return;
            }
        }
    }
    checks++;
}

int main(void)
{
    test_roundtrip();
    test_parse();
    test_migration();
    test_fuzz();
    return check_report("state");
}
