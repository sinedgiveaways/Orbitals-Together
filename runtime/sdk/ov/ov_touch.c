// nn::hid touch screen for MKW. Touch is an optional console feature (README "Decisiones"), off: almost no PC has a
// touch screen. Initialization does nothing and every read is one sample with no touches, so the game behaves as if
// nobody touched the screen. totk-pc-recomp has no touch overrides (TotK never reads the touch screen).
#include <string.h>

#include "host.h"

// TouchScreenState<N>: sampling number (8), touch count (4), reserved (4), N TouchState of 0x28 bytes.
#define TOUCH_STATE_SIZE(n) (0x10 + (n) * 0x28)

static uint64_t samples;

// void nn::hid::InitializeTouchScreen()
static void ov_initialize_touch(Cpu *c) { (void)c; }

// int nn::hid::GetTouchScreenStates<N>(TouchScreenState<N> *out, int count): the newest sample first, one sample.
static void touch_states(Cpu *c, int touches) {
    uint8_t *out = arg_p(c, 0);
    int count = (int)arg_x(c, 1);
    if (!out || count < 1) {
        ret_x(c, 0);
        return;
    }
    memset(out, 0, TOUCH_STATE_SIZE(touches));
    uint64_t n = __atomic_add_fetch(&samples, 1, __ATOMIC_RELAXED);
    memcpy(out, &n, sizeof n);
    ret_x(c, 1);
}

// void nn::hid::GetTouchScreenState<N>(TouchScreenState<N> *out): the newest sample alone.
static void touch_state(Cpu *c, int touches) {
    uint8_t *out = arg_p(c, 0);
    if (!out) return;
    memset(out, 0, TOUCH_STATE_SIZE(touches));
    uint64_t n = __atomic_add_fetch(&samples, 1, __ATOMIC_RELAXED);
    memcpy(out, &n, sizeof n);
}

// Each N is its own template instance with its own name: MKW reads GetTouchScreenStates<16>, Orbitals (Unreal)
// GetTouchScreenState<16> (main+0x4219418), whose SDK body aborted in nn::diag::detail::Abort (svc Break at
// sdk+0x1a4540, result 0x52ca) without the HID shared memory. Both forms, all 16 sizes.
#define TOUCH_N(n)                                                                                                   \
    static void ov_touch_states_##n(Cpu *c) { touch_states(c, n); }                                                  \
    static void ov_touch_state_##n(Cpu *c) { touch_state(c, n); }                                                    \
    HOST_OVERRIDE("_ZN2nn3hid20GetTouchScreenStatesILm" #n "EEEiPNS0_16TouchScreenStateIXT_EEEi", ov_touch_states_##n); \
    HOST_OVERRIDE("_ZN2nn3hid19GetTouchScreenStateILm" #n "EEEvPNS0_16TouchScreenStateIXT_EEE", ov_touch_state_##n);

TOUCH_N(1) TOUCH_N(2) TOUCH_N(3) TOUCH_N(4) TOUCH_N(5) TOUCH_N(6) TOUCH_N(7) TOUCH_N(8)
TOUCH_N(9) TOUCH_N(10) TOUCH_N(11) TOUCH_N(12) TOUCH_N(13) TOUCH_N(14) TOUCH_N(15) TOUCH_N(16)

HOST_OVERRIDE("_ZN2nn3hid21InitializeTouchScreenEv", ov_initialize_touch);
