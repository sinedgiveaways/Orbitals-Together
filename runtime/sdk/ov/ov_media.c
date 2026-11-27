// Adapted from MacNativeGames games/totk/runtime/opus_codec.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// The hardware Opus decoder (nn::codec), decoded with libopus on the host. The software keyboard (nn::swkbd) is
// in swkbd.c.
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>

#include "host.h"
#include "platform/platform.h"

#define CODEC_R_INVALID 0x1fcd1u   // MacNativeGames' "invalid argument" result for nn::codec
#define OPUS_WORK_SIZE 0x9000u     // what the game is told to reserve: libopus stereo decoder state (about 26 KiB) plus 4 KiB

// One record per initialised decoder object (the guest's HardwareOpusDecoderType). The registry lock only guards the
// list; a decoder's own lock serialises its libopus state, so different streams decode in parallel. Lock order:
// registry, then decoder (a caller takes the decoder lock before it drops the registry lock, so Finalize cannot free a
// decoder another thread is using).
// A decode request of the newer, queued API (EnqueueOpusDataToHardwareOpusDecoder): the packet, then its samples once
// ExecuteHardwareOpusDecoderDecoding has decoded it. Requests leave in order (Dequeue...).
typedef struct {
    uint8_t *pkt;
    uint32_t n, range;
    int16_t *pcm;
    int samples, done;   // samples per channel; done: decoded (or failed, samples 0)
} Req;

typedef struct Decoder {
    uint64_t guest;
    OpusDecoder *dec;
    int rate, channels;
    PlatMutex use;
    uint64_t mgr;        // HardwareOpusDecoderManagerType of the queued API, 0 for the direct one
    Req *reqs;           // use lock; oldest first
    int nreq, creq;
    struct Decoder *next;
} Decoder;
static Decoder *decoders;
static PlatMutex lock = PLAT_MUTEX_INIT;

static int valid_format(int rate, int channels) {
    return (rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) &&
           (channels == 1 || channels == 2);
}

// Returns the decoder with its use lock held.
static Decoder *acquire(uint64_t guest, const char *fn) {
    plat_mutex_lock_shared(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) plat_mutex_lock(&d->use);
    plat_mutex_unlock_shared(&lock);
    if (!d) rt_fatal("nn::codec::%s: unknown hardware Opus decoder %#llx", fn, (unsigned long long)guest);
    return d;
}

static void release(Decoder *d) { plat_mutex_unlock(&d->use); }

static OpusDecoder *opus_new(int rate, int channels) {
    int err = 0;
    OpusDecoder *o = opus_decoder_create(rate, channels, &err);
    if (!o || err != OPUS_OK) rt_fatal("nn::codec: opus_decoder_create(%d, %d): %s", rate, channels, opus_strerror(err));
    return o;
}

// size_t GetHardwareOpusDecoderWorkBufferSize(int rate, int channels, int option)
static void ov_work_size(Cpu *c) {
    ret_x(c, valid_format((int)arg_x(c, 0), (int)arg_x(c, 1)) ? OPUS_WORK_SIZE : 0);
}

// Result InitializeHardwareOpusDecoder(HardwareOpusDecoderType *d, int rate, int channels, void *work, size_t size,
// int option). The state lives in libopus on the host; the guest's work buffer is reserved but unused. Initialising
// the same object again without Finalize replaces its decoder.
static void ov_initialize(Cpu *c) {
    uint64_t guest = arg_x(c, 0), size = arg_x(c, 4);
    int rate = (int)arg_x(c, 1), channels = (int)arg_x(c, 2);
    if (!guest || !valid_format(rate, channels) || size < OPUS_WORK_SIZE) { ret_x(c, CODEC_R_INVALID); return; }
    OpusDecoder *fresh = opus_new(rate, channels);
    plat_mutex_lock(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) {
        plat_mutex_lock(&d->use);   // waits for a decode in flight
        OpusDecoder *old = d->dec;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        plat_mutex_unlock(&d->use);
        plat_mutex_unlock(&lock);
        opus_decoder_destroy(old);
    } else {
        d = calloc(1, sizeof *d);
        if (!d) rt_fatal("nn::codec: decoder allocation");
        plat_mutex_init(&d->use);
        d->guest = guest;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        d->next = decoders;
        decoders = d;
        plat_mutex_unlock(&lock);
    }
    ret_x(c, 0);
}

// void FinalizeHardwareOpusDecoder(HardwareOpusDecoderType *d)
static void ov_finalize(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&lock);
    Decoder **p = &decoders;
    while (*p && (*p)->guest != guest) p = &(*p)->next;
    if (!*p) rt_fatal("nn::codec::FinalizeHardwareOpusDecoder: unknown decoder %#llx", (unsigned long long)guest);
    Decoder *d = *p;
    plat_mutex_lock(&d->use);   // a decode in flight finishes first; the registry lock keeps new ones out
    *p = d->next;
    plat_mutex_unlock(&lock);
    opus_decoder_destroy(d->dec);
    for (int i = 0; i < d->nreq; i++) free(d->reqs[i].pkt), free(d->reqs[i].pcm);
    free(d->reqs);
    free(d);
}

// int GetHardwareOpusDecoderSampleRate / GetHardwareOpusDecoderChannelCount(const HardwareOpusDecoderType *d)
static void ov_sample_rate(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderSampleRate");
    int v = d->rate;
    release(d);
    ret_x(c, (uint64_t)v);
}

static void ov_channel_count(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderChannelCount");
    int v = d->channels;
    release(d);
    ret_x(c, (uint64_t)v);
}

// Result ResetHardwareOpusDecoder(HardwareOpusDecoderType *d)
static void ov_reset(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "ResetHardwareOpusDecoder");
    int ok = opus_decoder_ctl(d->dec, OPUS_RESET_STATE);
    release(d);
    ret_x(c, ok == OPUS_OK ? 0 : CODEC_R_INVALID);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Result DecodeOpusInterleavedWithHardware(HardwareOpusDecoderType *d, size_t *consumed, int *samples, int16_t *out,
// size_t out_size, const void *in, size_t in_size). The input is Nintendo's frame: an 8-byte big-endian header
// {payload size, final range} and the Opus packet. A header range of 0 is not a recorded value (some encoders write 0
// into every packet), so it is not checked. A range mismatch is logged once and the samples are kept.
static void ov_decode(Cpu *c) {
    static int32_t warned;
    uint64_t guest = arg_x(c, 0), out_size = arg_x(c, 4), in_size = arg_x(c, 6);
    size_t *consumed = arg_p(c, 1);
    int *samples = arg_p(c, 2);
    int16_t *out = arg_p(c, 3);
    const uint8_t *in = arg_p(c, 5);
    if (!consumed || !samples || !out || !in || in_size < 8) { ret_x(c, CODEC_R_INVALID); return; }
    uint32_t n = be32(in), range = be32(in + 4);
    if (n > in_size - 8 || n > INT_MAX) { ret_x(c, CODEC_R_INVALID); return; }
    Decoder *d = acquire(guest, "DecodeOpusInterleavedWithHardware");
    size_t capacity = out_size / (sizeof(int16_t) * (size_t)d->channels);
    if (capacity > INT_MAX) { release(d); ret_x(c, CODEC_R_INVALID); return; }
    int got = opus_decode(d->dec, in + 8, (opus_int32)n, out, (int)capacity, 0);
    opus_uint32 final = 0;
    if (got >= 0) opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final));
    release(d);
    if (got < 0) {
        rt_log("nn::codec: opus_decode: %s", opus_strerror(got));
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    if (n && range && final != range && !plat_xchg32(&warned, 1))
        rt_log("nn::codec: Opus final range %#x != stream %#x (logged once)", (unsigned)final, (unsigned)range);
    *consumed = 8 + n;
    *samples = got;
    ret_x(c, 0);
}

// size_t GetHardwareOpusDecoderManagerWorkBufferSize(int sample_rate, int channels, TimeSpan, int, int): the newer
// SDK's decoder manager (Orbitals, port-step1x: from main+0x6a46c68, with 48000, then the SDK asks the hwopus service
// through IPC). The meaning of the last three arguments is not known yet; they are logged. The memory is the game's
// and the host does not use it, so the answer only has to be large enough for what the game puts there.
#define OPUS_MANAGER_WORK_SIZE 0x40000u
static void ov_manager_work_size(Cpu *c) {
    static volatile int32_t n;
    if (plat_inc32(&n) <= 8)
        rt_log("nn::codec: manager work buffer for %d Hz, %d channels, span %lld ns, %d, %d: %#x",
               (int)arg_x(c, 0), (int)arg_x(c, 1), (long long)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 4),
               OPUS_MANAGER_WORK_SIZE);
    ret_x(c, OPUS_MANAGER_WORK_SIZE);
}

// ---- the queued API of the newer SDK (Orbitals): a HardwareOpusDecoderManagerType owns a system event and a number
// of IO buffers; decoders are initialised against it, the game enqueues Opus packets, calls Execute, and dequeues PCM.
// The SDK's version talks to the hwopus service over IPC. Here Execute decodes every waiting request of the manager's
// decoders with libopus, synchronously, then signals the manager's event; no request is ever "executing". The
// meaning of the manager's last two ints (128, 255 for mono; 72, 160 for stereo) is not known: the larger is taken as
// the IO buffer count, a request holding one buffer from Enqueue to Dequeue.
typedef struct Mgr {
    uint64_t guest, ev;
    int rate, channels, a, b, capacity;
    struct Mgr *next;
} Mgr;
static Mgr *mgrs;   // lock

static volatile int32_t qtrace[6];
#define QTRACE(k, ...) do { if (plat_inc32(&qtrace[k]) <= 6) rt_log(__VA_ARGS__); } while (0)

static Mgr *mgr_find(uint64_t guest) {   // lock held
    Mgr *m = mgrs;
    while (m && m->guest != guest) m = m->next;
    return m;
}

// requests held by the manager's decoders; lock held (shared or not)
static int mgr_outstanding(uint64_t guest) {
    int n = 0;
    for (Decoder *d = decoders; d; d = d->next)
        if (d->mgr == guest) {
            plat_mutex_lock(&d->use);
            n += d->nreq;
            plat_mutex_unlock(&d->use);
        }
    return n;
}

// Result InitializeHardwareOpusDecoderManager(HardwareOpusDecoderManagerType *m, os::SystemEventType *ev, int rate,
// int channels, TimeSpan frame, int a, int b, void *work, size_t size). The event is the game's, created here.
static void ov_mgr_init(Cpu *c) {
    uint64_t guest = arg_x(c, 0), ev = arg_x(c, 1);
    int rate = (int)arg_x(c, 2), channels = (int)arg_x(c, 3), a = (int)arg_x(c, 5), b = (int)arg_x(c, 6);
    rt_log("nn::codec: decoder manager %#llx: %d Hz, %d channels, span %lld ns, %d, %d, event %#llx, work %#llx+%#llx",
           (unsigned long long)guest, rate, channels, (long long)arg_x(c, 4), a, b, (unsigned long long)ev,
           (unsigned long long)arg_x(c, 7), (unsigned long long)arg_x(c, 8));
    if (!guest || !valid_format(rate, channels)) { ret_x(c, CODEC_R_INVALID); return; }
    if (ev) kern_bind_system_event(c, ev, 1);
    plat_mutex_lock(&lock);
    Mgr *m = mgr_find(guest);
    if (!m) {
        m = calloc(1, sizeof *m);
        if (!m) rt_fatal("nn::codec: manager allocation");
        m->guest = guest;
        m->next = mgrs;
        mgrs = m;
    }
    m->ev = ev, m->rate = rate, m->channels = channels, m->a = a, m->b = b;
    m->capacity = a > b ? a : b;
    if (m->capacity < 1) m->capacity = 1;
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// void FinalizeHardwareOpusDecoderManager(HardwareOpusDecoderManagerType *m). The event stays bound (the kernel's
// table binds an address once).
static void ov_mgr_fini(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&lock);
    for (Mgr **p = &mgrs; *p; p = &(*p)->next)
        if ((*p)->guest == guest) { Mgr *m = *p; *p = m->next; free(m); break; }
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// int GetHardwareOpusDecoderManagerFreeIoBufferCount(const HardwareOpusDecoderManagerType *m)
static void ov_mgr_free_io(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock_shared(&lock);
    Mgr *m = mgr_find(guest);
    int free_n = m ? m->capacity - mgr_outstanding(guest) : 0;
    plat_mutex_unlock_shared(&lock);
    if (free_n < 0) free_n = 0;
    QTRACE(0, "nn::codec: manager %#llx free IO buffers %d", (unsigned long long)guest, free_n);
    ret_x(c, (uint64_t)free_n);
}

// Result InitializeHardwareOpusDecoder(HardwareOpusDecoderType *d, int rate, int channels, TimeSpan frame,
// HardwareOpusDecoderManagerType *m)
static void ov_q_initialize(Cpu *c) {
    uint64_t guest = arg_x(c, 0), mgr = arg_x(c, 4);
    int rate = (int)arg_x(c, 1), channels = (int)arg_x(c, 2);
    rt_log("nn::codec: queued decoder %#llx: %d Hz, %d channels, span %lld ns, manager %#llx", (unsigned long long)guest,
           rate, channels, (long long)arg_x(c, 3), (unsigned long long)mgr);
    if (!guest || !valid_format(rate, channels)) { ret_x(c, CODEC_R_INVALID); return; }
    OpusDecoder *fresh = opus_new(rate, channels);
    plat_mutex_lock(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) rt_fatal("nn::codec::InitializeHardwareOpusDecoder: decoder %#llx initialised twice", (unsigned long long)guest);
    d = calloc(1, sizeof *d);
    if (!d) rt_fatal("nn::codec: decoder allocation");
    plat_mutex_init(&d->use);
    d->guest = guest, d->dec = fresh, d->rate = rate, d->channels = channels, d->mgr = mgr;
    d->next = decoders;
    decoders = d;
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// Result EnqueueOpusDataToHardwareOpusDecoder(HardwareOpusDecoderType *d, size_t *consumed, const void *in,
// size_t size). One packet per call: Nintendo's frame (8-byte big-endian {size, final range}, then the packet) when
// the header's size fits the input, else a raw Opus packet.
static void ov_q_enqueue(Cpu *c) {
    uint64_t guest = arg_x(c, 0), size = arg_x(c, 3);
    size_t *consumed = arg_p(c, 1);
    const uint8_t *in = arg_p(c, 2);
    if (!in || !size || size > (1u << 20)) { ret_x(c, CODEC_R_INVALID); return; }
    uint32_t n = (uint32_t)size, range = 0, off = 0;
    if (size >= 8 && be32(in) <= size - 8 && be32(in) > 0) n = be32(in), range = be32(in + 4), off = 8;
    plat_mutex_lock_shared(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (!d) rt_fatal("nn::codec::EnqueueOpusDataToHardwareOpusDecoder: unknown decoder %#llx", (unsigned long long)guest);
    Mgr *m = mgr_find(d->mgr);
    if (m && mgr_outstanding(d->mgr) >= m->capacity) {
        plat_mutex_unlock_shared(&lock);
        QTRACE(1, "nn::codec: enqueue on %#llx: no free IO buffer", (unsigned long long)guest);
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    plat_mutex_lock(&d->use);
    plat_mutex_unlock_shared(&lock);
    if (d->nreq == d->creq) {
        d->creq = d->creq ? d->creq * 2 : 16;
        d->reqs = realloc(d->reqs, sizeof *d->reqs * (size_t)d->creq);
        if (!d->reqs) rt_fatal("nn::codec: request allocation");
    }
    Req *r = &d->reqs[d->nreq++];
    memset(r, 0, sizeof *r);
    r->pkt = malloc(n);
    if (!r->pkt) rt_fatal("nn::codec: packet allocation");
    memcpy(r->pkt, in + off, n);
    r->n = n, r->range = range;
    int waiting = d->nreq;
    plat_mutex_unlock(&d->use);
    if (consumed) *consumed = off + n;
    QTRACE(2, "nn::codec: enqueue on %#llx: %llu bytes (%s), packet %u, %d queued", (unsigned long long)guest,
           (unsigned long long)size, off ? "frame header" : "raw packet", n, waiting);
    ret_x(c, 0);
}

// Result ExecuteHardwareOpusDecoderDecoding(HardwareOpusDecoderManagerType *m)
static void ov_mgr_execute(Cpu *c) {
    uint64_t guest = arg_x(c, 0), ev = 0;
    int decoded = 0;
    plat_mutex_lock_shared(&lock);
    Mgr *m = mgr_find(guest);
    if (m) ev = m->ev;
    for (Decoder *d = decoders; d; d = d->next) {
        if (d->mgr != guest) continue;
        plat_mutex_lock(&d->use);
        for (int i = 0; i < d->nreq; i++) {
            Req *r = &d->reqs[i];
            if (r->done) continue;
            int cap = 5760;   // 120 ms at 48 kHz, the longest Opus packet
            r->pcm = malloc(sizeof(int16_t) * (size_t)cap * (size_t)d->channels);
            if (!r->pcm) rt_fatal("nn::codec: PCM allocation");
            int got = opus_decode(d->dec, r->pkt, (opus_int32)r->n, r->pcm, cap, 0);
            if (got < 0) {
                rt_log("nn::codec: queued opus_decode on %#llx: %s", (unsigned long long)d->guest, opus_strerror(got));
                got = 0;
            }
            r->samples = got, r->done = 1;
            free(r->pkt), r->pkt = NULL;
            decoded++;
        }
        plat_mutex_unlock(&d->use);
    }
    plat_mutex_unlock_shared(&lock);
    if (ev) kern_signal_system_event(ev);
    QTRACE(3, "nn::codec: execute on manager %#llx: %d decoded", (unsigned long long)guest, decoded);
    ret_x(c, 0);
}

// Result DequeuePcmInterleavedDataFromHardwareOpusDecoder(HardwareOpusDecoderType *d, int *samples, int16_t *out,
// size_t out_size): the oldest decoded request; samples 0 when none is decoded yet.
static void ov_q_dequeue(Cpu *c) {
    uint64_t guest = arg_x(c, 0), out_size = arg_x(c, 3);
    int *samples = arg_p(c, 1);
    int16_t *out = arg_p(c, 2);
    Decoder *d = acquire(guest, "DequeuePcmInterleavedDataFromHardwareOpusDecoder");
    int got = 0, ok = 1;
    if (d->nreq && d->reqs[0].done) {
        Req r = d->reqs[0];
        size_t need = sizeof(int16_t) * (size_t)r.samples * (size_t)d->channels;
        if (out && need <= out_size) {
            memcpy(out, r.pcm, need);
            got = r.samples;
        } else ok = 0;
        if (ok) {
            free(r.pcm);
            memmove(d->reqs, d->reqs + 1, sizeof *d->reqs * (size_t)(d->nreq - 1));
            d->nreq--;
        }
    }
    release(d);
    if (samples) *samples = got;
    QTRACE(4, "nn::codec: dequeue on %#llx: %d samples%s", (unsigned long long)guest, got, ok ? "" : " (output too small)");
    ret_x(c, ok ? 0 : CODEC_R_INVALID);
}

// int GetHardwareOpusDecoder{Waiting,Executing,Completed}DecodeRequestCount(const HardwareOpusDecoderType *d)
static int q_count(Cpu *c, int done, const char *fn) {
    Decoder *d = acquire(arg_x(c, 0), fn);
    int n = 0;
    for (int i = 0; i < d->nreq; i++) n += d->reqs[i].done == done;
    release(d);
    return n;
}
static void ov_q_waiting(Cpu *c) { ret_x(c, (uint64_t)q_count(c, 0, "GetHardwareOpusDecoderWaitingDecodeRequestCount")); }
static void ov_q_completed(Cpu *c) { ret_x(c, (uint64_t)q_count(c, 1, "GetHardwareOpusDecoderCompletedDecodeRequestCount")); }
static void ov_q_executing(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm", ov_mgr_init);
HOST_OVERRIDE("_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_fini);
HOST_OVERRIDE("_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_free_io);
HOST_OVERRIDE("_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_execute);
HOST_OVERRIDE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE", ov_q_initialize);
HOST_OVERRIDE("_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm", ov_q_enqueue);
HOST_OVERRIDE("_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm", ov_q_dequeue);
HOST_OVERRIDE("_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_q_waiting);
HOST_OVERRIDE("_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_q_executing);
HOST_OVERRIDE("_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_q_completed);
HOST_OVERRIDE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", ov_work_size);
HOST_OVERRIDE("_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii", ov_manager_work_size);
HOST_OVERRIDE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", ov_initialize);
HOST_OVERRIDE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", ov_finalize);
HOST_OVERRIDE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", ov_sample_rate);
HOST_OVERRIDE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", ov_channel_count);
HOST_OVERRIDE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", ov_reset);
HOST_OVERRIDE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", ov_decode);
