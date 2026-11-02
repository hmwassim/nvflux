#ifndef NVFLUX_POLICY_H
#define NVFLUX_POLICY_H

/*
 * Decides exactly which clocks to lock, from a request (a mode or an expert
 * lock) and the GPU's own supported-clock tables. Pure: no NVML, no I/O, so
 * every rule is unit-tested against tables recorded from real GPUs.
 *
 * Every MHz value in a target comes from the GPU's tables. Nothing is ever
 * hardcoded, and nothing outside the tables is ever sent to the driver (the
 * driver rounds unsupported values *up*, e.g. 1000 -> 5001).
 */

enum mode {
    MODE_OFF,
    MODE_BALANCED,
    MODE_POWERSAVE,
    MODE_MAX,
    MODE_CUSTOM, /* expert locks */
};

/* What the user asked for. For MODE_CUSTOM, 0 means "not locked". */
struct request {
    enum mode mode;
    unsigned mem;          /* custom: memory clock */
    int mem_force;         /* allow a memory clock that caps the core */
    unsigned gfx_min;      /* custom: graphics range (min == max: one clock) */
    unsigned gfx_max;
    int gfx_force;         /* allow a graphics cap below the safe minimum */
};

/* A list of clocks in MHz, highest first, without duplicates. */
struct clocks {
    const unsigned *mhz;
    unsigned n;
};

/* What a GPU supports. gfx[i] lists the graphics clocks allowed while memory
 * runs at mem.mhz[i]; an empty list means the driver didn't say. */
struct gpu_caps {
    struct clocks mem;
    const struct clocks *gfx;
    int can_lock_mem;
    int can_lock_gfx;
};

enum mem_action { MEM_KEEP, MEM_RESET, MEM_LOCK };
enum gfx_action { GFX_KEEP, GFX_RESET, GFX_RANGE, GFX_CAP_BASE };

/* Notes explain parts of a target that differ from what a user might expect. */
enum {
    NOTE_NO_MEM_LOCK   = 1 << 0, /* memory part skipped: GPU can't lock memory */
    NOTE_NO_GFX_LOCK   = 1 << 1, /* graphics part skipped: GPU can't lock graphics */
    NOTE_MEM_SNAPPED   = 1 << 2, /* memory moved to the nearest supported clock */
    NOTE_GFX_SNAPPED   = 1 << 3, /* graphics moved to the nearest supported clock */
    NOTE_MEM_CAPS_CORE = 1 << 4, /* forced: this memory clock caps the core */
    NOTE_GFX_LOW_CAP   = 1 << 5, /* forced: graphics capped below the safe minimum */
};

struct target {
    enum mem_action mem;
    unsigned mem_mhz;
    enum gfx_action gfx;
    unsigned gfx_min, gfx_max;    /* GFX_RANGE */
    unsigned gfx_fallback_min;    /* GFX_CAP_BASE: numeric cap if the driver */
    unsigned gfx_fallback_max;    /* rejects symbolic values */
    unsigned notes;
    /* Context for messages. */
    unsigned core_max;            /* highest graphics clock the card allows */
    unsigned core_cap;            /* graphics limit at mem_mhz, if it caps the core */
    unsigned safe_cap_min;        /* lowest graphics cap allowed without --force */
};

enum policy_result {
    POLICY_OK = 0,
    POLICY_UNSUPPORTED,     /* nothing in the request can be applied on this GPU */
    POLICY_NO_CLOCKS,       /* the driver reported no supported memory clocks */
    POLICY_CAPS_UNKNOWN,    /* can't tell which clocks are safe; use 'lock' */
    POLICY_MEM_CAPS_CORE,   /* memory clock caps the core; needs --force */
    POLICY_GFX_CAP_TOO_LOW, /* graphics cap below the safe minimum; needs --force */
    POLICY_RANGE_INVERTED,  /* graphics min > max */
};

/* The share of the card's top graphics clock used by the modes:
 *   floor for 'balanced', and the lowest cap allowed without --force;
 *   numeric cap for 'powersave' when the driver rejects the symbolic one. */
#define POLICY_FLOOR_PERCENT    45
#define POLICY_FALLBACK_PERCENT 63

int policy_plan(const struct gpu_caps *caps, const struct request *req, struct target *t);

/* The supported clock nearest to mhz; ties go to the higher clock. */
unsigned policy_nearest(const struct clocks *c, unsigned mhz);

const char *mode_name(enum mode m);

#endif
