// The rows of version.h. Orbitals' row holds only what is known from its dump (recomp/versions.py, port_check.md of
// 16/11/2026): no guest address has been measured for it yet, so every one is 0 and the feature it serves is off, with
// a log line (version.h). The NVN2 proc table comes from the dump, found by setup (RtGame.port, tools/build/build.py
// port_glue). To measure, from a run with logs on: nvn_ret is the address boot.c logs as the first NVN call ("boot:
// first NVN call nvnBootstrapLoader ... from main+X": X + 4), and inits the DT_INIT counts it logs ("boot: DT_INIT
// <module> state=2 (<entries>)", in load order).
#include <string.h>

#include "version.h"
#include "host.h"

static const RtVersion versions[] = {
    {
        .number = 1,
        .display = "unknown",   // TODO: the update's display version (its control.nacp); not in the exefs
    },
};

// Version 0: a build this port has no row for, set up from its own exefs (recomp/versions.py unknown()).
static const RtVersion unknown = {.number = 0, .display = "unknown"};

// The row in use, with the NVN2 proc table of RtGame.port where the row has none.
static RtVersion cur_row;

const RtVersion *rt_version(void) {
    static const RtVersion *cur;
    if (cur) return cur;
    int v = rt_game()->version;
    const RtVersion *row = v == 0 ? &unknown : NULL;
    for (size_t i = 0; i < sizeof versions / sizeof *versions && !row; i++)
        if (versions[i].number == v) row = &versions[i];
    if (!row) rt_fatal("version: the game code is for game version %d, which this runtime has no row for", v);
    cur_row = *row;
    const RtPort *p = rt_game()->port;
    if (!cur_row.sdk_pointers && p) {
        cur_row.sdk_name_pool = p->sdk_name_pool;
        cur_row.sdk_name_offsets = p->sdk_name_offsets;
        cur_row.sdk_pointers = p->sdk_pointers;
        cur_row.sdk_last_index = p->sdk_last_index;
    }
    rt_log("version: %s, game version %d (%s); NVN2 proc table %s", p && p->title ? p->title : "game code", v,
           cur_row.display, cur_row.sdk_pointers ? (p && p->sdk_pointers == cur_row.sdk_pointers ? "from the dump"
           : "from the version row") : "not found (driver procs off)");
    return cur = &cur_row;
}

uint64_t rt_main_addr(uint64_t off) {
    if (!off) return 0;
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, "main")) return ldr_module(i)->base + off;
    rt_fatal("version: main is not loaded");
}
