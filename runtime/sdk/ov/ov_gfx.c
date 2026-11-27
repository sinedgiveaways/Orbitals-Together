// Adapted from MacNativeGames games/totk/runtime/frontend.m (nn::vi, lines 345-359) and gfx_bridge.c (nv, lines
// 74-86) @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors). The nvnBootstrapLoader stop takes the place
// of MacNativeGames' NVN entry (third_party/d2r_gfx/gfx/nvn/hle_nvn.c:115-118); no code of it is copied.
// Graphics bring-up:
//   nv::*        record the game's allocator callbacks and graphics work memory (guest addresses) for the NVN layer
//   nn::vi::*    display, layer and native window identities; the vsync event is bound here; HDR status (gfx_hdr.c)
//   nvnBootstrapLoader   the end of --boot (rt_boot_event "nvn"); otherwise the synthetic address of an NVN proc (nvn_procs.c)
#include <stdint.h>
#include <stdlib.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "kernel.h"
#include "ov.h"
#include "platform/platform.h"

#define VI_R_INVALID 0xf601u   // MacNativeGames' result for a call on an unknown display or layer

static OvNvState nv;
static PlatMutex lock = PLAT_MUTEX_INIT;

const OvNvState *ov_nv_state(void) { return &nv; }

static void set_allocator(Cpu *c, OvNvAllocator *a) {
    plat_mutex_lock(&lock);
    *a = (OvNvAllocator){arg_x(c, 0), arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
    plat_mutex_unlock(&lock);
}

// void nv::SetGraphicsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_graphics_allocator(Cpu *c) { set_allocator(c, &nv.graphics); }
// void nv::SetGraphicsDevtoolsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_devtools_allocator(Cpu *c) { set_allocator(c, &nv.devtools); }

// void nv::InitializeGraphics(void *memory, size_t size)
static void ov_initialize_graphics(Cpu *c) {
    uint64_t mem = arg_x(c, 0), size = arg_x(c, 1);
    // Switch 2 titles pass (0, 0): graphics memory comes from the allocator set by nv::SetGraphicsAllocator (MKW
    // main+0xcf3a00). Only a block without a size, or a size without a block, is wrong.
    if (!mem != !size) rt_fatal("nv::InitializeGraphics: work memory %#llx with size %#llx", (unsigned long long)mem,
                                (unsigned long long)size);
    plat_mutex_lock(&lock);
    nv.memory = mem;
    nv.memory_size = size;
    nv.initialized = 1;
    plat_mutex_unlock(&lock);
    rt_log("nv: graphics work memory %#llx+%#llx recorded", (unsigned long long)mem,
           (unsigned long long)size);
}

// void nv::InitCheck(): the SDK's aborts unless its own nv::InitializeGraphics ran, which ov_initialize_graphics
// replaces. MKW calls it before nn::codec::video::Initialize (main+0x19e5748), when the world loads its ad-board movies
// (run-race1 aborted there at frame 1134).
static void ov_init_check(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    int ok = nv.initialized;
    plat_mutex_unlock(&lock);
    if (!ok) rt_fatal("nv::InitCheck before nv::InitializeGraphics");
}

// ---- nn::vi: the identities are small zeroed blocks in the guest-visible kernel region, in case SDK code reads them
static int vi_ready;
static void *display, *layer, *window;

// void nn::vi::Initialize()
static void ov_vi_initialize(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    if (!display) {
        display = mem_kernel_alloc(64, 16);
        layer = mem_kernel_alloc(64, 16);
        window = mem_kernel_alloc(64, 16);
    }
    vi_ready = 1;
    plat_mutex_unlock(&lock);
}

static int vi_up(void) {
    plat_mutex_lock_shared(&lock);
    int r = vi_ready;
    plat_mutex_unlock_shared(&lock);
    return r;
}

// Result nn::vi::OpenDefaultDisplay(Display **out)
static void ov_open_display(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up()) { ret_x(c, VI_R_INVALID); return; }
    *out = display;
    ret_x(c, 0);
}

// Result nn::vi::CreateLayer(Layer **out, Display *display)
static void ov_create_layer(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    *out = layer;
    ret_x(c, 0);
}

// Result nn::vi::GetNativeWindow(void **out, Layer *layer): a placeholder the NVN layer recognises
static void ov_native_window(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != layer) { ret_x(c, VI_R_INVALID); return; }
    *out = window;
    ret_x(c, 0);
}

// Result nn::vi::GetDisplayVsyncEvent(os::SystemEventType *event, Display *display): auto-clear system event
static void ov_vsync_event(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    kern_bind_system_event(c, ev, 1);
    gfx_vsync_bind(ev);
    ret_x(c, 0);
}

// Result nn::vi::QueryDisplayHdrStatus(bool *on, HdrOutputFormat *format, u16 *peak_nits, const Display *display):
// HDR10 (format 1) when the window shows it (gfx_hdr.c). MKW asks every frame from main+0x3a9e28, behind its own gate
// (display object + 0x1cd); unreplaced, the SDK's call read a Display of zeros.
static void ov_hdr_status(Cpu *c) {
    uint8_t *on = arg_p(c, 0);
    uint32_t *format = arg_p(c, 1);
    uint16_t *peak = arg_p(c, 2), p, w;
    int hdr = gfx_hdr_output(&p, &w);
    static int logged = -1;
    if (logged != hdr) logged = hdr, rt_log("vi: QueryDisplayHdrStatus -> %s (peak %u nits)", hdr ? "HDR10" : "SDR", p);
    if (on) *on = (uint8_t)hdr;
    if (format) *format = hdr ? 1u : 0u;
    if (peak) *peak = p;
    ret_x(c, 0);
}

// Result nn::vi::QueryDisplayHdrPaperWhite(u16 *nits, const Display *display)
static void ov_hdr_paper_white(Cpu *c) {
    uint16_t *nits = arg_p(c, 0), p, w;
    gfx_hdr_output(&p, &w);
    if (nits) *nits = w;
    ret_x(c, 0);
}

// Result nn::vi::SetLayerNextHdr{Standard,Extended}ContentLuminanceRangeMetadata(Layer *, u16 min, u16 max): the
// game's own range for its next frame; logged when it changes
static void ov_hdr_range(Cpu *c, const char *which) {
    static uint32_t last[2];
    uint32_t v = (uint32_t)(arg_x(c, 1) & 0xffff) << 16 | (uint32_t)(arg_x(c, 2) & 0xffff), *l = &last[*which == 'e'];
    if (*l != v) *l = v, rt_log("vi: %s content luminance %u-%u nits", which, v >> 16, v & 0xffff);
    ret_x(c, 0);
}
static void ov_hdr_standard(Cpu *c) { ov_hdr_range(c, "standard"); }
static void ov_hdr_extended(Cpu *c) { ov_hdr_range(c, "extended"); }

// void *nvnBootstrapLoader(const char *name): the first NVN call
static void ov_nvn_bootstrap(Cpu *c) {
    const char *name = arg_p(c, 0);
    if (!name) name = "(null)";
    static int32_t first = 1;
    if (rt_config()->boot && plat_xchg32(&first, 0)) rt_boot_event("nvn", c, name);   // ends --boot; returns
                                                                                                // when --seconds goes on
    ret_x(c, nvn_proc_address(name));
}

// The NVN procs need their synthetic addresses before hostfn_seal; the loader seals at the end of rt_boot, so the
// registration runs at start-up instead of through boot.c.
__attribute__((constructor)) static void ov_nvn_procs_register(void) { nvn_procs_register(); }

HOST_OVERRIDE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_graphics_allocator);
HOST_OVERRIDE("_ZN2nv18InitializeGraphicsEPvm", ov_initialize_graphics);
HOST_OVERRIDE("_ZN2nv9InitCheckEv", ov_init_check);
HOST_OVERRIDE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_devtools_allocator);
HOST_OVERRIDE("_ZN2nn2vi10InitializeEv", ov_vi_initialize);
HOST_OVERRIDE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", ov_open_display);
HOST_OVERRIDE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", ov_create_layer);
HOST_OVERRIDE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", ov_native_window);
HOST_OVERRIDE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", ov_vsync_event);
HOST_OVERRIDE("nvnBootstrapLoader", ov_nvn_bootstrap);
HOST_OVERRIDE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", ov_hdr_status);
HOST_OVERRIDE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", ov_hdr_paper_white);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", ov_hdr_standard);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", ov_hdr_extended);

// ---- the rest of nn::vi's HDR metadata and VRR that 1.8.0 calls: the SDK's versions reach the vi2 layer object
// behind nn::vi::Layer, which the layer made above does not have (1.8.0: a call through a null pointer from
// main+0x959a44). The gamut, tone-mapping and Bezier metadata describe the game's next frame for the console's
// display pipeline, which the HDR10 swapchain does not take; VRR: a display without it.
static void ov_vi_nothing(Cpu *c) { ret_x(c, 0); }
// Result QueryDisplayVrrSupported(bool *, const Display *)
static void ov_vi_vrr(Cpu *c) {
    uint8_t *b = arg_p(c, 0);
    if (b) *b = 0;
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", ov_vi_vrr);
HOST_OVERRIDE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", ov_vi_nothing);
HOST_OVERRIDE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", ov_vi_nothing);
HOST_OVERRIDE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", ov_vi_nothing);
HOST_OVERRIDE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", ov_vi_nothing);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", ov_vi_nothing);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", ov_vi_nothing);

// ---- frame history (Orbitals' Unreal reads it each frame; main+0x153f990 and main+0x1540460 call it).
// What the console's SDK answers, measured on 17/11/2026 with lichii-probe on Lichii, which runs Orbitals' own
// nn::vi (sdk+0x5d5ac0 GetLatestFrameNumber, sdk+0x5d5ad0 ListFrameHistory) over its nx2:vi2 service: 400 calls
// on the flip tracker's thread, frames 1 to 3, and GetLatestFrameNumber up to frame 418.
//   - ListFrameHistory(NULL, 0, layer), the size query, answers 8 every time, before any frame and after: it is
//     the history's capacity, not the frames recorded. The tracker then passes an array of 8 (count = 8).
//   - ListFrameHistory(array, 8, layer) fills one 40-byte FrameInfo per frame shown, newest first, and returns how
//     many: 0, then 1 ([1]), 2 ([2, 1]), 3 ([3, 2, 1]). Entries past the count stay as the caller zeroed them.
//   - A FrameInfo is five u64: +0x00 = 2 in all 580 entries, +0x08 the frame number (from 1), +0x10 a tick taken
//     when the game queued the present, +0x18 and +0x20 a tick taken when it was shown, the two always equal;
//     +0x18 - +0x10 was 0.1 to 0.8 ms. Ticks are the guest clock (31.25 MHz, RT_TICK_HZ).
//   - GetLatestFrameNumber counts the presents queued, which runs ahead of the history (2 while it held [1]); before
//     the first present it failed with 0x1672 (module 114, vi; description 11).
// The previous answer (one entry, +0x10 zero, the size query equal to the frames held, newest only) dropped every
// frame but the newest when two were shown between polls, and the RenderThread waits for each frame's record.
// ORBITALS_FRAME_HISTORY=<n> sets the capacity (default 8, the console's; 0 answers "none", the old stall);
// ORBITALS_FRAME_BIAS=<n> adds n to GetLatestFrameNumber.
#define VI_HISTORY_MAX 8
#define VI_R_NO_FRAME 0x1672u   // GetLatestFrameNumber before the first present (measured, see above)

typedef struct { uint64_t frame, queued, shown; } ViFrame;
static ViFrame g_hist[VI_HISTORY_MAX];          // ring, by frame number
static uint64_t g_hist_newest;                  // newest frame shown (0: none)
static uint64_t g_queued_ticks[VI_HISTORY_MAX]; // +0x10 of each frame, by present number
static uint64_t g_presents_queued;
static uint64_t g_latest_calls, g_history_calls, g_history_real;

static uint64_t frame_bias(void) {
    static uint64_t bias;
    static int done;
    if (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        const char *b = cfg_get("ORBITALS_FRAME_BIAS");
        bias = b && *b ? strtoull(b, NULL, 10) : 0;
        __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    }
    return bias;
}

static uint64_t history_cap(void) {
    static uint64_t cap;
    static int done;
    if (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        const char *v = cfg_get("ORBITALS_FRAME_HISTORY");
        cap = v && *v ? strtoull(v, NULL, 10) : VI_HISTORY_MAX;
        if (cap > VI_HISTORY_MAX) cap = VI_HISTORY_MAX;
        __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    }
    return cap;
}

// The game queued a present (vk_be_present, on its thread): the frame's +0x10 time and GetLatestFrameNumber's count.
void vi_history_queued(void) {
    uint64_t now = rt_ticks();
    plat_mutex_lock(&lock);
    g_queued_ticks[g_presents_queued % VI_HISTORY_MAX] = now;
    g_presents_queued++;
    plat_mutex_unlock(&lock);
}

// Present number `frame` (from 1, in queue order) reached the screen: its +0x18 and +0x20 time.
void vi_history_shown(uint64_t frame) {
    uint64_t now = rt_ticks();
    plat_mutex_lock(&lock);
    uint64_t queued = frame && frame <= g_presents_queued && g_presents_queued - frame < VI_HISTORY_MAX
                          ? g_queued_ticks[(frame - 1) % VI_HISTORY_MAX] : now;
    g_hist[frame % VI_HISTORY_MAX] = (ViFrame){frame, queued > now ? now : queued, now};
    if (frame > g_hist_newest) g_hist_newest = frame;
    plat_mutex_unlock(&lock);
}

// Result GetLatestFrameNumber(uint64_t *out, const Layer *): the presents queued so far
static void ov_latest_frame(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    plat_mutex_lock(&lock);
    uint64_t queued = g_presents_queued;
    plat_mutex_unlock(&lock);
    uint64_t n = queued + frame_bias();
    uint32_t r = queued ? 0 : VI_R_NO_FRAME;
    uint64_t k = __atomic_add_fetch(&g_latest_calls, 1, __ATOMIC_RELAXED);
    if (k <= 5 || !(k & (k - 1))) {
        char lr[96];
        rt_log("vi: GetLatestFrameNumber #%llu -> %#x, %llu (queued %llu, shown %llu, bias %llu) from %s",
               (unsigned long long)k, r, (unsigned long long)n, (unsigned long long)queued,
               (unsigned long long)gfx_stats().frames, (unsigned long long)frame_bias(),
               ldr_describe(c->x[30] - 4, lr, sizeof lr));
    }
    if (out && !r) *out = n;
    ret_x(c, out ? r : 1);
}

// ORBITALS_THREADS_AT=<frame>[,<frame>...] (diagnosis, up to 4): as each of those many frames is presented, every guest
// thread's wait and call chain go to the log (kern_threads_dump, otherwise written only at a fatal abort), to see what a game
// that presents frames but draws black waits for; a thread found at the same wait in every dump is stuck there. The flip
// tracker calls ListFrameHistory each frame, so it is the place that sees the count go by. Each dump also logs the call
// count of every host function (hostfn_report, otherwise only at the end of --boot): between two dumps, what the game calls
// once a frame. The import census (ORBITALS_IMPORT_CENSUS) counts the SDK functions that are not replaced.
static void threads_dump_at(uint64_t presented) {
    static uint64_t at[4];
    static int n, next, state;   // state 0 unread, 1 armed, 2 done or off
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2) return;
    if (state == 0) {
        const char *v = cfg_get("ORBITALS_THREADS_AT");
        for (char *e; v && *v && n < 4; v = *e == ',' ? e + 1 : e) {
            uint64_t f = strtoull(v, &e, 10);
            if (e == v) break;
            if (f) at[n++] = f;
        }
        __atomic_store_n(&state, n ? 1 : 2, __ATOMIC_RELEASE);
        if (!n) return;
    }
    plat_mutex_lock(&lock);
    int due = next < n && presented >= at[next];
    uint64_t which = due ? at[next] : 0;
    if (due && ++next == n) __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    if (due) {
        rt_log("threads: dump at frame %llu (ORBITALS_THREADS_AT %llu)", (unsigned long long)presented,
               (unsigned long long)which);
        kern_threads_dump();
        hostfn_report();
    }
}

// int ListFrameHistory(FrameInfo *out, int count, const Layer *): the size query answers the capacity; with an
// array, the frames shown, newest first, as many as fit (see the measurements above).
static void ov_frame_history(Cpu *c) {
    uint64_t out = arg_x(c, 0);
    int count = (int)arg_x(c, 1);
    uint64_t cap = history_cap();
    threads_dump_at(gfx_stats().frames);
    uint64_t ret = 0, newest = 0;
    if (!out && count == 0) {
        ret = cap;                                     // the size query: the capacity, frames or not
    } else if (out && count > 0) {
        uint64_t *e = arg_p(c, 0);
        plat_mutex_lock(&lock);
        newest = g_hist_newest;
        for (uint64_t f = newest; f >= 1 && ret < cap && ret < (uint64_t)count; f--) {
            const ViFrame *h = &g_hist[f % VI_HISTORY_MAX];
            if (h->frame != f) break;                  // older than the ring holds
            uint64_t *o = e + ret * 5;
            o[0] = 2, o[1] = f, o[2] = h->queued, o[3] = h->shown, o[4] = h->shown;
            ret++;
        }
        plat_mutex_unlock(&lock);
    }
    uint64_t k = __atomic_add_fetch(&g_history_calls, 1, __ATOMIC_RELAXED);
    int with_buffer = out || count;
    uint64_t j = with_buffer ? __atomic_add_fetch(&g_history_real, 1, __ATOMIC_RELAXED) : 0;
    if (k <= 5 || !(k & (k - 1)) || (with_buffer && j <= 20)) {
        char lr[96];
        rt_log("vi: ListFrameHistory #%llu count=%d out=%#llx -> %llu (newest %llu, shown %llu, cap %llu) from %s",
               (unsigned long long)k, count, (unsigned long long)out, (unsigned long long)ret,
               (unsigned long long)newest, (unsigned long long)gfx_stats().frames, (unsigned long long)cap,
               ldr_describe(c->x[30] - 4, lr, sizeof lr));
    }
    ret_x(c, ret);
}
HOST_OVERRIDE("_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", ov_latest_frame);
HOST_OVERRIDE("_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", ov_frame_history);
HOST_OVERRIDE("_ZN2nn2vi36SetDisplay120HzRefreshRateCapabilityEPNS0_7DisplayEb", ov_vi_nothing);
HOST_OVERRIDE("_ZN2nn2vi8FinalizeEv", ov_vi_nothing);