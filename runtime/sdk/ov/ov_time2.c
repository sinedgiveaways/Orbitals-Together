// nn::time functions MKW imports that totk-pc-recomp does not replace (ov_system.c has the clocks and calendars).
// The host clock is the network clock (ov_system.c answers StandardNetworkSystemClock from it), so it is accurate and
// the network clock never needs a correction from the ntc service.
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "host.h"
#include "platform/platform.h"

// bool StandardNetworkSystemClock::IsAccuracySufficient()
static void ov_accuracy(Cpu *c) { ret_x(c, 1); }

// void GetDeviceLocationName(LocationName *out): char[0x24], the host's time zone (plat_time_zone), else UTC
static void ov_location(Cpu *c) {
    char *out = arg_p(c, 0);
    if (!out) return;
    memset(out, 0, 0x24);
    if (!plat_time_zone(out, 0x24)) memcpy(out, "UTC", 3);
    static int logged;
    if (!logged++) rt_log("time: device location %s", out);
}

// Result LoadTimeZoneRule(TimeZoneRule *out, const LocationName &): MKW converts times with ToCalendarTime, which
// ov_system.c answers in host local time without a rule; the rule is left empty.
static void ov_load_rule(Cpu *c) { ret_x(c, 0); }

// AsyncEnsureNetworkClockAvailabilityTask wraps nn::ntc's correction task; its constructor, Initialize and
// GetFinishEvent stay the SDK's (they create and hand out the finish event). StartTask finishes at once: it signals
// that event, and GetResult reports success.
static uint64_t sdk_fn(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("time: SDK function %s not found", name);
    return a;
}
static void ov_task_start(Cpu *c) {
    static uint64_t finish_event, signal;
    if (!signal) {
        finish_event = sdk_fn("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv");
        signal = sdk_fn("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    }
    uint64_t self = arg_x(c, 0);
    uint64_t ev = rt_call(c, finish_event, 1, self);
    rt_call(c, signal, 1, ev);
    ret_x(c, 0);
}
static void ov_task_result(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", ov_accuracy);
HOST_OVERRIDE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", ov_location);
HOST_OVERRIDE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", ov_load_rule);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", ov_task_start);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", ov_task_result);

// ---- the C and C++ clocks (Orbitals: libc++'s std::chrono::steady_clock::now threw a system_error, its
// clock_gettime having failed in the SDK's musl, port-step1q). From the host's clocks: CLOCK_REALTIME (0) the wall
// clock, every other id (MONOTONIC 1, BOOTTIME 7...) the monotonic one. musl's timespec and timeval are two int64s.
static void ov_clock_gettime(Cpu *c) {
    int64_t *ts = arg_p(c, 1);
    if (!ts) { ret_x(c, (uint64_t)-1); return; }
    uint64_t ns = (int32_t)arg_x(c, 0) == 0 ? plat_unix_time_ns() : plat_now_ns();
    ts[0] = (int64_t)(ns / 1000000000u), ts[1] = (int64_t)(ns % 1000000000u);
    ret_x(c, 0);
}
static void ov_gettimeofday(Cpu *c) {   // int gettimeofday(struct timeval *, void *tz)
    int64_t *tv = arg_p(c, 0);
    uint64_t ns = plat_unix_time_ns();
    if (tv) tv[0] = (int64_t)(ns / 1000000000u), tv[1] = (int64_t)(ns % 1000000000u / 1000u);
    ret_x(c, 0);
}
static void ov_time(Cpu *c) {   // time_t time(time_t *)
    int64_t *t = arg_p(c, 0), s = (int64_t)(plat_unix_time_ns() / 1000000000u);
    if (t) *t = s;
    ret_x(c, (uint64_t)s);
}
// steady_clock::time_point steady_clock::now(): nanoseconds; system_clock::now(): microseconds since 1970 (libc++'s
// system_clock duration, as its to_time_t import's mangled name says)
void ov_musl_clock_hooks(const char *when);
// A timed condition_variable wait reads steady_clock::now for its deadline, then musl's clock: the pointers are checked
// again here (two loads once set up), in case something sets the SDK's stubs back after the boot.
static void ov_steady_now(Cpu *c) {
    ov_musl_clock_hooks("steady_clock::now");
    ret_x(c, plat_now_ns());
}
static void ov_system_now(Cpu *c) { ret_x(c, plat_unix_time_ns() / 1000u); }
HOST_OVERRIDE("clock_gettime", ov_clock_gettime);
HOST_OVERRIDE("gettimeofday", ov_gettimeofday);
HOST_OVERRIDE("time", ov_time);
HOST_OVERRIDE("_ZNSt3__16chrono12steady_clock3nowEv", ov_steady_now);
HOST_OVERRIDE("_ZNSt3__16chrono12system_clock3nowEv", ov_system_now);
// ---- the SDK's musl clocks (Orbitals, port-step1w). The overrides above reach only calls through a GOT. musl's own
// callers (pthread_cond_clockwait / timedwait, sdk+0x173970 in Orbitals' SDK) call its clock_gettime with a bl, and
// its body (sdk+0xb5430) reads CLOCK_REALTIME and CLOCK_MONOTONIC through two function pointers in the SDK's .data:
// `bl thunk; cbnz w0, EAGAIN`, each thunk `adrp x8, page; ldr x1, [x8, #off]; br x1`. Left as they are, those
// pointers return nonzero, so musl sets errno 11 (EAGAIN): libc++'s condition_variable::wait_until then threw
// "condition_variable timed_wait failed: Resource temporarily unavailable". The pointers get host clocks here: the
// same two clocks clock_gettime above returns, so a deadline from steady_clock::now matches musl's own reading.
// They take (timespec *) in x0 and return 0.
static void ov_musl_realtime(Cpu *c) {
    int64_t *ts = arg_p(c, 0);
    uint64_t ns = plat_unix_time_ns();
    if (ts) ts[0] = (int64_t)(ns / 1000000000u), ts[1] = (int64_t)(ns % 1000000000u);
    ret_x(c, ts ? 0 : 22);
}
static void ov_musl_monotonic(Cpu *c) {
    int64_t *ts = arg_p(c, 0);
    uint64_t ns = plat_now_ns();
    if (ts) ts[0] = (int64_t)(ns / 1000000000u), ts[1] = (int64_t)(ns % 1000000000u);
    ret_x(c, ts ? 0 : 22);
}
HOST_FUNCTION("<musl clock realtime>", ov_musl_realtime);
HOST_FUNCTION("<musl clock monotonic>", ov_musl_monotonic);

static uint64_t branch_target(uint64_t pc, uint32_t w) {   // b / bl imm26
    int64_t imm = (int64_t)((uint64_t)(w & 0x3FFFFFFu) << 38) >> 36;
    return pc + (uint64_t)imm;
}

// The .data slot a `adrp x8, page; ldr x1, [x8, #off]; br x1` thunk at a jumps through, 0 for other code.
static uint64_t thunk_slot(uint64_t a) {
    const uint32_t *w = (const uint32_t *)(uintptr_t)a;
    if ((w[0] & 0x9F00001Fu) != 0x90000008u || (w[1] & 0xFFC003FFu) != 0xF9400101u || w[2] != 0xD61F0020u) return 0;
    int64_t imm = (int64_t)((uint64_t)(((w[0] >> 5) & 0x7FFFFu) << 2 | ((w[0] >> 29) & 3u)) << 43) >> 31;   // 21 bits, << 12
    return (a & ~0xFFFull) + (uint64_t)imm + ((w[1] >> 10) & 0xFFFu) * 8;
}

static int in_data(uint64_t a) {   // a writable word of a loaded module (after .text/.rodata, which are sealed)
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a >= m->base + m->data_off && a + 8 <= m->base + m->size) return 1;
    }
    return 0;
}

// Called at nn::time::Initialize and again after each module's DT_INIT (core/boot/boot.c): port-step1w stored the
// host clocks at nn::time::Initialize, yet the run still failed and no "<musl clock ...>" host call was logged, so a
// later writer (the SDK's own constructors run after nn::time::Initialize) had put its stub back. A slot that does not
// hold the host clock is logged with what it held and set again.
void ov_musl_clock_hooks(const char *when) {
    if (!rt_game_present() || !rt_game()->port) return;   // MKW's SDK: its clock_gettime works as it is
    static int searched;
    static uint64_t slot[2];
    if (!searched) {
        searched = 1;
        uint64_t f = ldr_symbol("clock_gettime");
        if (!f) { rt_log("time: no clock_gettime in the SDK; musl's clock hooks left alone"); return; }
        uint32_t w = *(const uint32_t *)(uintptr_t)f;
        if ((w & 0xFC000000u) == 0x14000000u) f = branch_target(f, w);   // the export is `b body`
        // The body: the first two bl to a thunk, CLOCK_REALTIME's first (Orbitals: sdk+0xb5478 and sdk+0xb5488)
        int n = 0;
        for (int i = 0; i < 48 && n < 2; i++) {
            uint64_t pc = f + 4u * (uint64_t)i;
            uint32_t x = *(const uint32_t *)(uintptr_t)pc;
            if ((x & 0xFC000000u) != 0x94000000u) continue;
            uint64_t s = thunk_slot(branch_target(pc, x));
            if (s && in_data(s) && (!n || s != slot[0])) slot[n++] = s;
        }
        if (n != 2) {
            char d[160];
            rt_log("time: musl's clock_gettime at %s has not the two clock pointers (%d found); left alone",
                   ldr_describe(f, d, sizeof d), n);
            slot[0] = slot[1] = 0;
            return;
        }
    }
    if (!slot[0]) return;
    const uint64_t want[2] = {hostfn_address(ov_musl_realtime), hostfn_address(ov_musl_monotonic)};
    static const char *const kind[2] = {"realtime", "monotonic"};
    for (int i = 0; i < 2; i++) {
        uint64_t *p = (uint64_t *)(uintptr_t)slot[i], was = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        if (was == want[i]) continue;
        char d[160], b[160];
        rt_log("time: %s: musl's %s clock pointer %s held %s; set to the host clock", when, kind[i],
               ldr_describe(slot[i], d, sizeof d), ldr_describe(was, b, sizeof b));
        __atomic_store_n(p, want[i], __ATOMIC_RELEASE);
    }
}
