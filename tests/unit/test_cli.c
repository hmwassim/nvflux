#include "cli.h"

#include "check.h"

static int parse(struct cmd *c, char *err, int argc, char **argv)
{
    return cli_parse(argc, argv, c, err, 512);
}

#define ARGS(...) (char *[]){ "nvflux", __VA_ARGS__, NULL }
#define N(...) ((int)(sizeof((char *[]){ "nvflux", __VA_ARGS__ }) / sizeof(char *)))
#define OK(...) CHECK_EQ(parse(&c, err, N(__VA_ARGS__), ARGS(__VA_ARGS__)), 0)
#define BAD(...) CHECK_EQ(parse(&c, err, N(__VA_ARGS__), ARGS(__VA_ARGS__)), -1)

int main(void)
{
    struct cmd c;
    char err[512];

    CHECK_EQ(parse(&c, err, 1, (char *[]){ "nvflux", NULL }), 0);
    CHECK_EQ(c.kind, CMD_STATUS);

    OK("balanced");
    CHECK_EQ(c.kind, CMD_MODE);
    CHECK_EQ(c.mode, MODE_BALANCED);
    CHECK(c.alias == NULL);
    CHECK(cli_needs_root(&c));

    OK("BALANCED");                 /* case doesn't matter */
    CHECK_EQ(c.mode, MODE_BALANCED);
    OK("performance");              /* aliases work and are remembered */
    CHECK_EQ(c.mode, MODE_MAX);
    CHECK_STR(c.alias, "performance");
    OK("auto");
    CHECK_EQ(c.mode, MODE_OFF);
    OK("power-saver");
    CHECK_EQ(c.mode, MODE_POWERSAVE);

    OK("max", "--temporary", "--gpu", "1");
    CHECK(c.temporary);
    CHECK(c.gpu_set);
    CHECK_EQ(c.gpu, 1);
    OK("--gpu", "0", "off");        /* options anywhere */
    CHECK_EQ(c.gpu, 0);

    OK("lock", "memory", "5001");
    CHECK_EQ(c.kind, CMD_LOCK);
    CHECK(c.lock_mem && !c.lock_gfx);
    CHECK_EQ(c.mem, 5001);
    OK("lock", "graphics", "900-1800", "--force");
    CHECK(c.lock_gfx && c.force);
    CHECK_EQ(c.gfx_min, 900);
    CHECK_EQ(c.gfx_max, 1800);
    OK("lock", "graphics", "1500");
    CHECK_EQ(c.gfx_min, 1500);
    CHECK_EQ(c.gfx_max, 1500);
    OK("lock", "memory", "810", "graphics", "945-2100");
    CHECK(c.lock_mem && c.lock_gfx);

    OK("restore");                  /* the old nvflux's name for reload */
    CHECK_EQ(c.kind, CMD_RELOAD);
    CHECK_STR(c.alias, "restore");
    OK("lock", "5001");             /* the old nvflux's lock syntax */
    CHECK(c.lock_mem && c.old_lock);
    CHECK_EQ(c.mem, 5001);
    OK("lock", "5001", "--force");
    CHECK(c.force);
    BAD("lock", "5001", "6001");

    OK("status", "--json");
    CHECK(c.json);
    CHECK(!cli_needs_root(&c));
    OK("reload", "--wait", "10");
    CHECK_EQ(c.wait, 10);
    OK("--help");
    CHECK_EQ(c.kind, CMD_HELP);
    OK("-V");
    CHECK_EQ(c.kind, CMD_VERSION);

    BAD("balance");
    CHECK(strstr(err, "did you mean 'balanced'?") != NULL);
    BAD("frobnicate");
    CHECK(strstr(err, "see 'nvflux help'") != NULL);
    BAD("lock");
    BAD("lock", "memory");
    BAD("lock", "memory", "abc");
    BAD("lock", "memory", "0");
    BAD("lock", "memory", "-5");
    BAD("lock", "fans", "100");
    BAD("lock", "graphics", "1800-900");
    CHECK(strstr(err, "900-1800") != NULL);
    BAD("balanced", "--force");     /* --force only with lock */
    BAD("balanced", "now");
    BAD("status", "--temporary");
    BAD("status", "--gpu", "0");
    BAD("balanced", "--json");
    BAD("balanced", "--gpu");
    BAD("balanced", "--gpu", "x");
    BAD("--wait", "5");
    BAD("--frob");
    return check_report("cli");
}
