#include "policy.h"

#include <string.h>

const char *mode_name(enum mode m)
{
    switch (m) {
    case MODE_OFF:       return "off";
    case MODE_BALANCED:  return "balanced";
    case MODE_POWERSAVE: return "powersave";
    case MODE_MAX:       return "max";
    case MODE_CUSTOM:    return "custom";
    }
    return "?";
}

unsigned policy_nearest(const struct clocks *c, unsigned mhz)
{
    unsigned best = c->mhz[0], best_d = ~0u;
    for (unsigned i = 0; i < c->n; i++) {
        unsigned v = c->mhz[i];
        unsigned d = v > mhz ? v - mhz : mhz - v;
        if (d < best_d) { /* strict: on a tie the earlier, higher clock wins */
            best = v;
            best_d = d;
        }
    }
    return best;
}

static unsigned percent_of(unsigned mhz, unsigned pct)
{
    return (unsigned)(((unsigned long)mhz * pct + 50) / 100);
}

/* Highest graphics clock allowed at memory clock i (0 = unknown). */
static unsigned gfx_top_at(const struct gpu_caps *c, unsigned i)
{
    return c->gfx[i].n ? c->gfx[i].mhz[0] : 0;
}

/* Highest graphics clock the card allows at any memory clock. */
static unsigned core_max(const struct gpu_caps *c)
{
    unsigned top = 0;
    for (unsigned i = 0; i < c->mem.n; i++)
        if (gfx_top_at(c, i) > top)
            top = gfx_top_at(c, i);
    return top;
}

static unsigned mem_index(const struct gpu_caps *c, unsigned mhz)
{
    for (unsigned i = 0; i < c->mem.n; i++)
        if (c->mem.mhz[i] == mhz)
            return i;
    return 0;
}

/*
 * The lowest memory clock that doesn't cap the core. The lowest memory clocks
 * belong to idle power states that also cap the graphics clock (an RTX 3060
 * at 405 MHz memory allows at most 420 MHz core, against 2100 otherwise);
 * locking there makes even the cursor stutter.
 */
static unsigned steady_mem_index(const struct gpu_caps *c, unsigned top)
{
    unsigned best = 0;
    for (unsigned i = 0; i < c->mem.n; i++)
        if (gfx_top_at(c, i) == top)
            best = i; /* the list is highest first, so keep the last match */
    return best;
}

/* The memory part shared by 'balanced' and 'powersave'. Returns the index of
 * the memory clock whose graphics table the graphics part must use. */
static int plan_steady_mem(const struct gpu_caps *c, unsigned top, struct target *t,
                           unsigned *table)
{
    if (!c->can_lock_mem) {
        t->mem = MEM_KEEP;
        t->notes |= NOTE_NO_MEM_LOCK;
        *table = 0; /* memory is free to run at its highest clock */
        return POLICY_OK;
    }
    if (top == 0)
        return POLICY_CAPS_UNKNOWN;
    *table = steady_mem_index(c, top);
    t->mem = MEM_LOCK;
    t->mem_mhz = c->mem.mhz[*table];
    return POLICY_OK;
}

static int plan_mode(const struct gpu_caps *c, enum mode mode, unsigned top, struct target *t)
{
    unsigned table = 0;
    int rc;

    switch (mode) {
    case MODE_OFF:
        t->mem = c->can_lock_mem ? MEM_RESET : MEM_KEEP;
        t->gfx = c->can_lock_gfx ? GFX_RESET : GFX_KEEP;
        break;

    case MODE_MAX:
        if (c->can_lock_mem) {
            t->mem = MEM_LOCK;
            t->mem_mhz = c->mem.mhz[0];
        } else {
            t->notes |= NOTE_NO_MEM_LOCK;
        }
        /* Clear any earlier floor or cap; boost already reaches the top under
         * load, and pinning it high would only add heat at idle. */
        t->gfx = c->can_lock_gfx ? GFX_RESET : GFX_KEEP;
        break;

    case MODE_BALANCED:
    case MODE_POWERSAVE:
        if ((rc = plan_steady_mem(c, top, t, &table)) != POLICY_OK)
            return rc;
        if (!c->can_lock_gfx) {
            t->notes |= NOTE_NO_GFX_LOCK;
            break;
        }
        if (c->gfx[table].n == 0)
            return POLICY_CAPS_UNKNOWN;

        const struct clocks *g = &c->gfx[table];
        unsigned gtop = g->mhz[0];
        if (mode == MODE_BALANCED) {
            /* A numeric floor: the symbolic "never below base" value had no
             * effect at idle in testing, while a numeric floor held. */
            t->gfx = GFX_RANGE;
            t->gfx_min = policy_nearest(g, percent_of(gtop, POLICY_FLOOR_PERCENT));
            t->gfx_max = gtop;
        } else {
            /* Cap at the card's own base clock; if the driver rejects the
             * symbolic value, fall back to a numeric cap. */
            t->gfx = GFX_CAP_BASE;
            t->gfx_fallback_min = g->mhz[g->n - 1];
            t->gfx_fallback_max = policy_nearest(g, percent_of(gtop, POLICY_FALLBACK_PERCENT));
        }
        break;

    case MODE_CUSTOM:
        break; /* handled by plan_custom */
    }

    if (t->mem == MEM_KEEP && t->gfx == GFX_KEEP)
        return POLICY_UNSUPPORTED;
    return POLICY_OK;
}

static int plan_custom(const struct gpu_caps *c, const struct request *r, unsigned top,
                       struct target *t)
{
    unsigned table = 0;

    if (r->mem) {
        if (!c->can_lock_mem)
            return POLICY_UNSUPPORTED;
        t->mem = MEM_LOCK;
        t->mem_mhz = policy_nearest(&c->mem, r->mem);
        if (t->mem_mhz != r->mem)
            t->notes |= NOTE_MEM_SNAPPED;
        table = mem_index(c, t->mem_mhz);

        unsigned limit = gfx_top_at(c, table);
        if (top && limit < top) {
            t->core_cap = limit;
            if (!r->mem_force)
                return POLICY_MEM_CAPS_CORE;
            t->notes |= NOTE_MEM_CAPS_CORE;
        }
    } else {
        t->mem = c->can_lock_mem ? MEM_RESET : MEM_KEEP;
    }

    if (r->gfx_max) {
        if (!c->can_lock_gfx)
            return POLICY_UNSUPPORTED;
        if (r->gfx_min > r->gfx_max)
            return POLICY_RANGE_INVERTED;
        const struct clocks *g = &c->gfx[table];
        if (g->n == 0)
            return POLICY_CAPS_UNKNOWN;

        t->gfx = GFX_RANGE;
        t->gfx_min = policy_nearest(g, r->gfx_min);
        t->gfx_max = policy_nearest(g, r->gfx_max);
        if (t->gfx_min != r->gfx_min || t->gfx_max != r->gfx_max)
            t->notes |= NOTE_GFX_SNAPPED;

        /* A cap below the safe minimum slows everything down, down to the
         * cursor; floors can't hurt, so only the upper end is checked. */
        if (t->gfx_max < t->safe_cap_min) {
            if (!r->gfx_force)
                return POLICY_GFX_CAP_TOO_LOW;
            t->notes |= NOTE_GFX_LOW_CAP;
        }
    } else {
        t->gfx = c->can_lock_gfx ? GFX_RESET : GFX_KEEP;
    }

    if (t->mem == MEM_KEEP && t->gfx == GFX_KEEP)
        return POLICY_UNSUPPORTED;
    return POLICY_OK;
}

int policy_plan(const struct gpu_caps *caps, const struct request *req, struct target *t)
{
    memset(t, 0, sizeof(*t));
    if (caps->mem.n == 0)
        return POLICY_NO_CLOCKS;

    unsigned top = core_max(caps);
    t->core_max = top;
    t->safe_cap_min = percent_of(top, POLICY_FLOOR_PERCENT);

    if (req->mode == MODE_CUSTOM)
        return plan_custom(caps, req, top, t);
    return plan_mode(caps, req->mode, top, t);
}
