// The fixed values of each game build the port knows: one row per executable, chosen by RtGame.version (the number
// recomp/versions.py gives the dump the game code was generated from). Every guest address the runtime reads or calls
// by number lives here, so a new build is one more row and the code that uses them stays the same.
// A field at 0 is not known for that build: the feature that needs it is off, with a log line, never a guess.
#pragma once
#include <stdint.h>

#include "gamelib.h"

typedef struct { const char *name; uint64_t entries; } RtInitCount;

typedef struct {
    int number;                // RtGame.version: 1 Orbitals; 0 a build not listed
    const char *display;       // as the game shows it (nn::oe::GetDisplayVersion)
    // main offsets
    uint64_t nvn_ret;          // return address of the first nvnBootstrapLoader call (boot.c checkpoint)
    // sdk offsets: the NVN2 driver's proc table (tools/gpu/nvn2_procs.py TABLES, the same rows); 0: RtGame.port's,
    // found in the dump at setup
    uint64_t sdk_name_pool, sdk_name_offsets, sdk_pointers;
    int sdk_last_index;
    // the NVN2 device values nvnDeviceGetInteger answers (nvn_api_core.c): 16 for API 231.16's table, 39 for 231.39's;
    // 0 is 231.16
    int nvn_api_minor;
    // DT_INIT_ARRAYSZ / 8 of each module, in load order (boot.c): a different count is a different build; none: each
    // module's DT_INIT runs and its count is logged
    RtInitCount inits[RT_MAX_MODULES];
} RtVersion;

const RtVersion *rt_version(void);                 // the row of the game code in use; fatal for a version not listed
uint64_t rt_main_addr(uint64_t main_offset);       // main's base + offset, 0 for an offset of 0
