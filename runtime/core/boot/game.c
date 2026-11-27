// The game clock the shared runtime reads (host.h frame_step_*): counted from presents. totk-pc-recomp's resolution.c
// and frame_rate.c (patch sites and fixed addresses of TotK) have no counterpart here yet: a frame-rate setting needs
// the game's own frame object, not found for Orbitals (runtime/core/boot/version.c).
#include <stdatomic.h>

#include "host.h"

static _Atomic uint64_t ticks;   // 60 Hz ticks since the first present

uint64_t frame_step_ticks(void) { return atomic_load_explicit(&ticks, memory_order_relaxed); }

// The world clock of the A/B blocks (ORBITALS_AB, perf.c rt_ab) and of timed script lines: game time since the first
// present, -1 before it. No scene of Orbitals is known to start it later, so the blocks count from the start.
int64_t frame_step_world_us(void) {
    uint64_t t = frame_step_ticks();
    return t ? (int64_t)(t * 1000000 / 60) : -1;
}

void frame_step_present(uint64_t display_ns, int interval) {
    (void)display_ns;
    atomic_fetch_add_explicit(&ticks, interval > 0 ? (uint64_t)interval : 1, memory_order_relaxed);
}
