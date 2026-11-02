/*
 * Policy tests against GPU "shapes", not just one card: every mode rule must
 * hold for any table a driver can report.
 */
#include "policy.h"

#include <stdlib.h>

#include "check.h"

/* Builds graphics tables lo..top in `step` MHz, highest first. */
static struct clocks range(unsigned lo, unsigned top, unsigned step)
{
    unsigned n = (top - lo) / step + 1;
    unsigned *v = malloc(n * sizeof(*v));
    for (unsigned i = 0; i < n; i++)
        v[i] = top - i * step;
    return (struct clocks){ v, n };
}

static void free_caps(struct gpu_caps *c)
{
    for (unsigned i = 0; i < c->mem.n; i++)
        free((void *)c->gfx[i].mhz);
    free((void *)c->gfx);
}

/* mem: highest first; tops[i]: graphics top at mem[i] (0 = table unknown). */
static struct gpu_caps make(const unsigned *mem, unsigned nmem, const unsigned *tops, unsigned step)
{
    struct clocks *gfx = calloc(nmem, sizeof(*gfx));
    for (unsigned i = 0; i < nmem; i++)
        if (tops[i])
            gfx[i] = range(405, tops[i], step);
    return (struct gpu_caps){ { mem, nmem }, gfx, 1, 1 };
}

static int plan(const struct gpu_caps *c, struct request r, struct target *t)
{
    return policy_plan(c, &r, t);
}

/* The RTX 3060 measured on the reference machine. */
static const unsigned mem3060[] = { 7501, 7301, 5001, 810, 405 };
static const unsigned top3060[] = { 2100, 2100, 2100, 2100, 420 };

static void test_rtx3060(void)
{
    struct gpu_caps c = make(mem3060, 5, top3060, 15);
    struct target t;

    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_OK);
    CHECK_EQ(t.mem, MEM_LOCK);
    CHECK_EQ(t.mem_mhz, 810);          /* lowest clock that doesn't cap the core */
    CHECK_EQ(t.gfx, GFX_RANGE);
    CHECK_EQ(t.gfx_min, 945);          /* 45% of 2100, a supported step */
    CHECK_EQ(t.gfx_max, 2100);
    CHECK_EQ(t.notes, 0);

    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_POWERSAVE }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 810);
    CHECK_EQ(t.gfx, GFX_CAP_BASE);
    CHECK_EQ(t.gfx_fallback_min, 405);
    CHECK_EQ(t.gfx_fallback_max, 1320); /* 63% of 2100 = 1323 -> nearest step */

    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_MAX }, &t), POLICY_OK);
    CHECK_EQ(t.mem, MEM_LOCK);
    CHECK_EQ(t.mem_mhz, 7501);
    CHECK_EQ(t.gfx, GFX_RESET);         /* never pinned high */

    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_OFF }, &t), POLICY_OK);
    CHECK_EQ(t.mem, MEM_RESET);
    CHECK_EQ(t.gfx, GFX_RESET);
    free_caps(&c);
}

static void test_custom(void)
{
    struct gpu_caps c = make(mem3060, 5, top3060, 15);
    struct target t;

    /* Snaps to the nearest supported clock; never sends an unsupported one
     * (the real driver would round 1000 up to 5001). */
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .mem = 1000 }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 810);
    CHECK(t.notes & NOTE_MEM_SNAPPED);
    CHECK_EQ(t.gfx, GFX_RESET);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .mem = 7000 }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 7301);

    /* A memory clock that caps the core needs --force. */
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .mem = 405 }, &t), POLICY_MEM_CAPS_CORE);
    CHECK_EQ(t.core_cap, 420);
    CHECK_EQ(t.core_max, 2100);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .mem = 405, .mem_force = 1 }, &t), POLICY_OK);
    CHECK(t.notes & NOTE_MEM_CAPS_CORE);

    /* Graphics ranges snap to the table for the chosen memory clock. */
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .gfx_min = 900, .gfx_max = 1800 }, &t), POLICY_OK);
    CHECK_EQ(t.mem, MEM_RESET);
    CHECK_EQ(t.gfx_min, 900);
    CHECK_EQ(t.gfx_max, 1800);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .gfx_min = 1503, .gfx_max = 1503 }, &t), POLICY_OK);
    CHECK_EQ(t.gfx_min, 1500);
    CHECK(t.notes & NOTE_GFX_SNAPPED);

    /* Caps below 45% of max need --force; floors never do. */
    CHECK_EQ(t.safe_cap_min, 945);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .gfx_min = 405, .gfx_max = 600 }, &t),
             POLICY_GFX_CAP_TOO_LOW);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .gfx_min = 405, .gfx_max = 600, .gfx_force = 1 }, &t),
             POLICY_OK);
    CHECK(t.notes & NOTE_GFX_LOW_CAP);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .gfx_min = 945, .gfx_max = 945 }, &t), POLICY_OK);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .gfx_min = 2000, .gfx_max = 2100 }, &t), POLICY_OK);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .gfx_min = 1800, .gfx_max = 900 }, &t),
             POLICY_RANGE_INVERTED);

    /* Memory and graphics together: graphics checked against that memory
     * clock's table. With 405 forced, the table tops out at 420. */
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .mem = 405, .mem_force = 1,
                                        .gfx_min = 1500, .gfx_max = 1500, .gfx_force = 1 }, &t), POLICY_OK);
    CHECK_EQ(t.gfx_min, 420);
    free_caps(&c);
}

/* A GDDR6X-style card: different clocks, the same rules. */
static void test_gddr6x(void)
{
    static const unsigned mem[] = { 10501, 10251, 9501, 5001, 810, 405 };
    static const unsigned top[] = { 2805, 2805, 2805, 2805, 2805, 645 };
    struct gpu_caps c = make(mem, 6, top, 15);
    struct target t;
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 810);
    CHECK_EQ(t.gfx_max, 2805);
    CHECK(t.gfx_min >= 1250 && t.gfx_min <= 1275); /* 45% of 2805 = 1262 */
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_MAX }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 10501);
    free_caps(&c);
}

/* Every low clock caps the core: balanced must never pick a capping clock. */
static void test_all_low_capped(void)
{
    static const unsigned mem[] = { 7501, 810, 405 };
    static const unsigned top[] = { 2100, 1200, 420 };
    struct gpu_caps c = make(mem, 3, top, 15);
    struct target t;
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 7501);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_POWERSAVE }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 7501);
    free_caps(&c);
}

static void test_single_clock(void)
{
    static const unsigned mem[] = { 7001 };
    static const unsigned top[] = { 1800 };
    struct gpu_caps c = make(mem, 1, top, 15);
    struct target t;
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 7001);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .mem = 100 }, &t), POLICY_OK);
    CHECK_EQ(t.mem_mhz, 7001);
    free_caps(&c);
}

/* The driver doesn't report graphics tables: modes refuse to guess, expert
 * memory locks still work. */
static void test_no_caps(void)
{
    static const unsigned mem[] = { 7501, 810, 405 };
    static const unsigned top[] = { 0, 0, 0 };
    struct gpu_caps c = make(mem, 3, top, 15);
    struct target t;
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_CAPS_UNKNOWN);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_POWERSAVE }, &t), POLICY_CAPS_UNKNOWN);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_MAX }, &t), POLICY_OK);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .mem = 810 }, &t), POLICY_OK);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .gfx_min = 900, .gfx_max = 900 }, &t),
             POLICY_CAPS_UNKNOWN);
    free_caps(&c);
}

static void test_big_table(void)
{
    static const unsigned mem[] = { 7501, 810 };
    static const unsigned top[] = { 2100, 2100 };
    struct gpu_caps c = make(mem, 2, top, 4); /* 424 entries */
    struct target t;
    CHECK(c.gfx[0].n > 256);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_OK);
    CHECK(t.gfx_min >= 943 && t.gfx_min <= 947);
    free_caps(&c);
}

/* Turing: graphics locks only. Modes still apply their graphics part. */
static void test_turing(void)
{
    static const unsigned mem[] = { 7001, 405 };
    static const unsigned top[] = { 1950, 1950 };
    struct gpu_caps c = make(mem, 2, top, 15);
    c.can_lock_mem = 0;
    struct target t;
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_OK);
    CHECK_EQ(t.mem, MEM_KEEP);
    CHECK(t.notes & NOTE_NO_MEM_LOCK);
    CHECK_EQ(t.gfx, GFX_RANGE);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_MAX }, &t), POLICY_OK);
    CHECK_EQ(t.mem, MEM_KEEP);
    CHECK_EQ(t.gfx, GFX_RESET);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_OFF }, &t), POLICY_OK);
    CHECK_EQ(t.mem, MEM_KEEP);
    CHECK_EQ(t.gfx, GFX_RESET);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_CUSTOM, .mem = 7001 }, &t), POLICY_UNSUPPORTED);
    free_caps(&c);
}

/* Pascal and older: nothing can be locked. */
static void test_pascal(void)
{
    static const unsigned mem[] = { 5005, 405 };
    static const unsigned top[] = { 1900, 1900 };
    struct gpu_caps c = make(mem, 2, top, 15);
    c.can_lock_mem = c.can_lock_gfx = 0;
    struct target t;
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_UNSUPPORTED);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_MAX }, &t), POLICY_UNSUPPORTED);
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_OFF }, &t), POLICY_UNSUPPORTED);
    free_caps(&c);
}

static void test_no_clocks(void)
{
    struct gpu_caps c = { { NULL, 0 }, NULL, 1, 1 };
    struct target t;
    CHECK_EQ(plan(&c, (struct request){ .mode = MODE_BALANCED }, &t), POLICY_NO_CLOCKS);
}

static void test_nearest(void)
{
    static const unsigned v[] = { 2100, 1500, 900 };
    struct clocks c = { v, 3 };
    CHECK_EQ(policy_nearest(&c, 1200), 1500); /* tie goes to the higher clock */
    CHECK_EQ(policy_nearest(&c, 1199), 900);
    CHECK_EQ(policy_nearest(&c, 99999), 2100);
    CHECK_EQ(policy_nearest(&c, 1), 900);
}

int main(void)
{
    test_rtx3060();
    test_custom();
    test_gddr6x();
    test_all_low_capped();
    test_single_clock();
    test_no_caps();
    test_big_table();
    test_turing();
    test_pascal();
    test_no_clocks();
    test_nearest();
    return check_report("policy");
}
