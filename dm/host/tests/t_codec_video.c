// The Switch 2's video decoder API as the host serves it (hle_codec_video.c), without a guest: the buffer protocol, results, sync handles, sync point events and the
// readback, on synthetic red key frames: 16x16 VP9 and VP8 (the ones t_movie_vpx.c decodes) and a 64x64 H.264 baseline frame (ffmpeg's libx264, SEI removed); no game data. The SystemEvent functions are stubs that record each event's state.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_movie.c"
#include "../hle_codec_video.c"
#include "../hle_movie_avc.c"
#include <assert.h>

uint64_t now_ns(void) { return 1; }
__thread _Atomic int dm_host_work;   // hle_thread.c
static struct { void *ev; int bound, set; } g_ev[16];
static int ev_slot(void *e) { for (int i = 0; i < 16; i++) if (g_ev[i].ev == e) return i; for (int i = 0; i < 16; i++) if (!g_ev[i].ev) { g_ev[i].ev = e; return i; } abort(); }
void hle_bind_system_event(void *e, int autoclear) { (void)autoclear; g_ev[ev_slot(e)].bound = 1; }
void hle_signal_system_event(void *e) { g_ev[ev_slot(e)].set = 1; }
void hle_clear_system_event(void *e) { g_ev[ev_slot(e)].set = 0; }
static int ev_set(void *e) { return g_ev[ev_slot(e)].set; }
static void destroy_ev(void *e) { g_ev[ev_slot(e)].bound = g_ev[ev_slot(e)].set = 0; }   // nn::os::DestroySystemEvent: closing the handle unbinds the event
void *guest_sym(const char *n) { return !strcmp(n, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE") ? (void *)destroy_ev : NULL; }
// AV1 (hle_movie_av1.c, VideoToolbox or dav1d) is not linked here: its decoders are made and freed through these stand-ins, and decode nothing
static int g_av1_live;
mav1 *mav1_new(void) { g_av1_live++; return (mav1 *)&g_av1_live; }
void mav1_free(mav1 *a) { assert(a == (mav1 *)&g_av1_live); g_av1_live--; }
void mav1_reset(mav1 *a) { (void)a; }
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out) { (void)a; (void)data; (void)n; (void)out; return 0; }
const char *mav1_backend(const mav1 *a) { (void)a; return "none (test)"; }
void mav1_set_time(mav1 *a, int64_t ts) { (void)a; (void)ts; }

static const uint8_t vp8[] = {
    0xf0,0x02,0x00,0x9d,0x01,0x2a,0x10,0x00,0x10,0x00,0x00,0x47,0x08,0x85,0x85,0x88,
    0x85,0x84,0x88,0x02,0x02,0x02,0x75,0xaa,0x03,0xf8,0x03,0xfa,0x02,0x0d,0x4d,0x18,
    0x00,0xfe,0xfd,0x6e,0xf3,0xff,0xe3,0x99,0x37,0x30,0xc4,0xff,0x8e,0x6d,0xff,0xf1,
    0x61,0x3c,0x0e,0x28,0xc8,0xff,0xf1,0x51,0x00,
};
static const uint8_t h264[] = {   // Annex B: SPS, PPS, one IDR slice
    0x00,0x00,0x00,0x01,0x67,0x42,0xc0,0x0a,0xdc,0x42,0x6c,0x04,0x40,0x00,0x00,0x03,0x00,0x40,0x00,0x00,0x0c,0xa3,0xc4,0x89,
    0xe0,0x00,0x00,0x00,0x01,0x68,0xce,0x0f,0xc8,0x00,0x00,0x01,0x65,0x88,0x84,0x3a,0x11,0x8a,0x00,0x02,0x18,0xf1,0xc0,0x00,
    0x40,0xf6,0x38,0x00,0x08,0x79,0x49,0xc9,0xc9,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xe0,
};
static const uint8_t vp9[] = {
    0x82,0x49,0x83,0x42,0x00,0x00,0xf0,0x00,0xf6,0x00,0x38,0x24,0x1c,0x18,0x4a,0x00,
    0x00,0x30,0x60,0x00,0x00,0x10,0xbf,0xff,0xf7,0x1d,0xaf,0xff,0xff,0xff,0x5f,0xdf,
    0xff,0xff,0xff,0xf2,0x2a,0xc0,0x00,
};

int main(void) {
    _Alignas(16) static uint8_t dec[0x400], pool[3][0x40], spt[0x80], mem[16 * 16 * 4];
    uint64_t size, align, id; uint32_t idx; cv_sync sync; uint16_t w, h, cw, ch; int64_t ts; bool eos;
    setenv("DM_CODEC_THREAD", "0", 1);   // the protocol below on the submitting thread, where a picture is in its buffer when Submit returns; the decode thread at the end

    // Pools: one picture each, NV12 (format 3, as Bananza asks)
    cv_pref pref = h_cv_pref(16, 16, 3, 0);
    assert(!h_cv_poolsize(&size, &align, &pref, 1) && size >= 16 * 16 * 4 && align == CV_NATIVE_ALIGN);   // a native image's picture on its own 16 KB page
    for (int i = 0; i < 3; i++) assert(!h_cv_pool(pool[i], mem, size, &pref, 1, 0) && pool_of(pool[i]));

    // Decoder: the submission-ready event is bound in its type and signalled
    assert(!h_cv_vp9init2(dec, 0x10000, 16, 16, 1, 1, 3));
    cv_dec *d = cv_of(dec); assert(d && d->vpx_ok);
    void *ev = NULL; assert(!h_cv_vp9event(&ev, dec) && ev == dec + CV_EVENT_OFF && g_ev[ev_slot(ev)].bound && ev_set(ev));

    // Registering makes a buffer acquirable at once, its sync handle signalled; an empty queue answers 3604
    for (uint32_t i = 0; i < 2; i++) assert(!h_cv_vp9reg(dec, i, pool[i], 0));
    for (uint32_t i = 0; i < 2; i++) { assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == i && sync.magic == CV_SYNC && sync.gen == 0 && h_gs_trywait(&sync)); }
    assert(h_cv_vp9acq(&idx, &sync, dec) == CV_NONE);
    assert(h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) == CV_NONE);

    // Released: acquirable again with a new handle that signals once a picture is in the buffer; the picture's properties and its NV12 / RGBA readback
    h_cv_vp9rel(dec, 0, sync);
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 0 && sync.gen == 1 && !h_gs_trywait(&sync));
    assert(h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) == CV_NONE);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 1000, false) && id == 1);
    assert(h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) && w == 16 && h == 16 && cw == 16 && ch == 16 && ts == 1000 && !eos);
    uint8_t nv12[16 * 16 * 3 / 2], rgba[16 * 16 * 4];
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 16, 16, nv12, 16, 256, nv12 + 256, 16, 128));
    for (int i = 0; i < 256; i++) assert(abs(nv12[i] - 81) <= 3);
    for (int i = 256; i < 384; i += 2) { assert(abs(nv12[i] - 90) <= 3); assert(abs(nv12[i + 1] - 240) <= 3); }
    uint8_t y[256], uv[128];   // separate planes
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 16, 16, y, 16, 256, uv, 16, 128) && !memcmp(y, nv12, 256) && !memcmp(uv, nv12 + 256, 128));
    assert(!h_cv_rgba(pool[0], 0, 0, 0, 16, 16, rgba, 64, sizeof rgba));
    for (int i = 0; i < 1024; i += 4) assert(rgba[i] >= 250 && rgba[i + 1] <= 5 && rgba[i + 2] <= 5 && rgba[i + 3] == 255);

    // No released buffer: pictures wait; the event goes down at CV_READY waiting, packets are refused (3603) at CV_HOLD; a release takes the oldest, and the event stays
    // down until fewer than CV_READY wait
    for (int i = 0; i < CV_HOLD; i++) { assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 2000 + i, false)); assert(ev_set(ev) == (d->nheld < CV_READY)); }
    assert(d->nheld == CV_HOLD && !ev_set(ev));
    assert(h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 9999, false) == CV_BUSY);
    h_cv_vp9rel(dec, 1, sync);
    assert(d->nheld == CV_HOLD - 1 && !ev_set(ev));
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1 && h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 2000);

    // Sync point event: armed on an unfilled buffer, signalled when the picture lands; one armed on a filled buffer is signalled at once
    assert(!h_spt_create(spt) && g_ev[ev_slot(spt)].bound);
    h_cv_vp9rel(dec, 0, sync);   // buffer 0 takes the next waiting picture at once
    assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 0);
    assert(!h_spt_request(spt, &sync) && ev_set(spt) && !SPT(spt)->armed);
    assert(!h_cv_vp9reg(dec, 2, pool[2], 0) && !h_cv_vp9acq(&idx, &sync, dec) && idx == 2);
    for (uint32_t i = 0; d->nheld; i ^= 1) { h_cv_vp9rel(dec, i, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == i); assert(ev_set(ev) == (d->nheld < CV_READY)); }   // the other waiting pictures go out; below CV_READY the event is up again
    h_cv_vp9rel(dec, 2, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 2 && !h_gs_trywait(&sync));
    assert(!h_spt_request(spt, &sync) && !ev_set(spt) && SPT(spt)->armed);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 3000, false) && ev_set(spt) && !SPT(spt)->armed && h_gs_trywait(&sync));

    // End of stream with no data: the next released buffer carries it, with no picture
    assert(!h_cv_vp9submit(&id, dec, NULL, 0, 0, true) && d->eos_pending);
    h_cv_vp9rel(dec, 2, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 2 && h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 2) && eos && w == 0 && h == 0);

    // A pool finalized while its buffer is still registered: the buffer keeps working, its picture has nowhere to go (ASan checks no freed pool is written)
    h_cv_poolfin(pool[1]); assert(!pool_of(pool[1]) && d->buf[1].pool == NULL);
    h_cv_vp9rel(dec, 1, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1 && !h_gs_trywait(&sync));
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 4000, false) && h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 4000 && w == 16);
    // A flush drops the waiting pictures and restarts the decoder; finalize frees it and destroys its event
    assert(!h_cv_vp9flush(dec) && d->nheld == 0 && d->vpx_ok && ev_set(ev));
    assert(!h_cv_vp9fin(dec) && !cv_of(dec) && !g_ev[ev_slot(ev)].bound);
    // A decoder initialised again in the same memory, cleared in between (Donkey Kong Bananza's second movie): its event is bound anew
    memset(dec, 0, sizeof dec);
    assert(!h_cv_vp9init2(dec, 0x10000, 16, 16, 1, 1, 3) && cv_of(dec) && g_ev[ev_slot(ev)].bound && ev_set(ev));
    // The session's second decoder and later (Donkey Kong Bananza's guide movies): every packet gives its picture, and the decoder reports no end of stream the player did not
    // send. Ended by the host after 4 packets (the default before), and again at every later packet, the second movie's player aborted (main+0xe1ae94)
    d = cv_of(dec); assert(d->seq >= 2 && !getenv("DM_MOVIE_SKIP"));
    for (int i = 0; i < 2; i++) assert(!h_cv_vp9reg(dec, i, pool[2 * i], 0) && !h_cv_vp9acq(&idx, &sync, dec) && idx == (uint32_t)i);
    for (int i = 0; i < 8; i++) {
        uint32_t k = (uint32_t)i & 1; h_cv_vp9rel(dec, k, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == k && !h_gs_trywait(&sync));
        assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 10000 + i, false) && h_gs_trywait(&sync));
        assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, k) && w == 16 && ts == 10000 + i && !eos);
    }
    assert(!d->eos_pending && !d->skipped && d->pictures == 8);
    // DM_MOVIE_SKIP=all (a diagnostic): the stream ends once, on the 5th packet; the packets the player still sends are dropped with no picture and no second end of stream;
    // a flush starts a new stream
    setenv("DM_MOVIE_SKIP", "all", 1);
    assert(!h_cv_vp9flush(dec) && !d->skipped && !d->stream_pkts);
    for (int i = 0; i < 5; i++) {
        uint32_t k = (uint32_t)i & 1; h_cv_vp9rel(dec, k, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == k);
        assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 20000 + i, false) && h_gs_trywait(&sync));
        assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, k) && w == 16 && ts == 20000 + i && eos == (i == 4));
    }
    assert(d->skipped);
    h_cv_vp9rel(dec, 1, sync); assert(!h_cv_vp9acq(&idx, &sync, dec) && idx == 1);
    for (int i = 0; i < 3; i++) assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 30000 + i, false));
    assert(!h_gs_trywait(&sync) && h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) == CV_NONE && !d->eos_pending && !d->nheld);
    assert(!h_cv_vp9flush(dec) && !d->skipped);
    assert(!h_cv_vp9submit(&id, dec, vp9, sizeof vp9, 40000, false) && h_gs_trywait(&sync));
    assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 40000 && !eos);
    unsetenv("DM_MOVIE_SKIP");
    for (int i = 0; i < 2; i++) assert(!h_cv_vp9unreg(dec, i));
    assert(!h_cv_vp9fin(dec) && !cv_of(dec) && !g_ev[ev_slot(ev)].bound);
    for (int i = 0; i < 3; i += 2) { h_cv_poolfin(pool[i]); assert(!pool_of(pool[i])); }
    // VP8 (libvpx) and H.264 (VideoToolbox) take the same protocol: one released buffer, one key frame, its picture read back red
    {   _Alignas(16) static uint8_t d8[0x400], p8[0x40], m8[16 * 16 * 4];
        assert(!h_cv_poolsize(&size, &align, &pref, 1) && !h_cv_pool(p8, m8, size, &pref, 1, 0));
        assert(!h_cv_vp8init2(d8, 0x10000, 16, 16, 1, 1, 3) && cv_of(d8) && cv_of(d8)->codec == CV_VP8 && cv_of(d8)->vpx_ok);
        assert(!h_cv_vp8reg(d8, 0, p8, 0) && !h_cv_vp8acq(&idx, &sync, d8) && idx == 0);
        h_cv_vp8rel(d8, 0, sync); assert(!h_cv_vp8acq(&idx, &sync, d8) && !h_gs_trywait(&sync));
        assert(!h_cv_vp8submit(&id, d8, vp8, sizeof vp8, 7000, false) && h_gs_trywait(&sync));
        assert(!h_cv_vp8props(&w, &h, &cw, &ch, &ts, &eos, d8, 0) && w == 16 && h == 16 && ts == 7000);
        assert(!h_cv_nv12(p8, 0, 0, 0, 16, 16, nv12, 16, 256, nv12 + 256, 16, 128));
        for (int i = 0; i < 256; i++) assert(abs(nv12[i] - 81) <= 3);
        for (int i = 256; i < 384; i += 2) { assert(abs(nv12[i] - 90) <= 4); assert(abs(nv12[i + 1] - 240) <= 4); }
        // Unregistered: no longer acquirable or filled
        h_cv_vp8rel(d8, 0, sync); assert(!h_cv_vp8unreg(d8, 0) && h_cv_vp8acq(&idx, &sync, d8) == CV_NONE && !cv_of(d8)->buf[0].reg);
        assert(!h_cv_vp8fin(d8) && !cv_of(d8)); h_cv_poolfin(p8);
    }
    // SDK 21 / 22 names (Mario Kart World 1.8.0, hle_codec_video.c's "SDK 22" forms): preference with a tiling mode in a 0x40-byte slot, pool without a codec mode, Initialize…ByTransferMemory / ByRemoteMemory,
    // the submission-ready event returned; a VP9 picture goes through as with the older names. (Called from C here: the AAPCS64 stack slots of out / mode, which the hooks take
    // as uint64_t so that Apple's arm64 ABI reads them at sp+0 / sp+8 too, are checked against the SDK's prologues, not by this test)
    {   _Alignas(16) static uint8_t d9[0x400], p9[0x40], m9[16 * 16 * 4]; _Alignas(16) uint8_t slot[0x40]; memset(slot, 0xee, sizeof slot);
        cv_pref p21 = h_cv_pref22(16, 16, 3, 1); assert(sizeof p21 <= sizeof slot); memcpy(slot, &p21, sizeof p21);
        assert(!h_cv_poolsize(&size, &align, (const cv_pref *)slot, 1) && size >= 16 * 16 * 4 && !h_cv_pool22(p9, m9, size, (const cv_pref *)slot, 1) && pool_of(p9));
        assert(!h_cv_vp9tm(d9, m9, 0, 0x10000, 16, 16, 1, 1, 3, 0) && cv_of(d9) && cv_of(d9)->codec == CV_VP9 && cv_of(d9)->vpx_ok);
        void *e21 = h_cv_vp9ev22(d9); assert(e21 == d9 + CV_EVENT_OFF && ev_set(e21));
        assert(!h_cv_vp9reg(d9, 0, p9, 0) && !h_cv_vp9acq(&idx, &sync, d9) && idx == 0);
        h_cv_vp9rel(d9, 0, sync); assert(!h_cv_vp9acq(&idx, &sync, d9) && !h_gs_trywait(&sync));
        assert(!h_cv_vp9submit(&id, d9, vp9, sizeof vp9, 8000, false) && h_gs_trywait(&sync));
        assert(!h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, d9, 0) && w == 16 && h == 16 && ts == 8000);
        assert(!h_cv_vp9fin(d9) && !cv_of(d9));
        memset(d9, 0, sizeof d9); assert(!h_cv_vp9rm(d9, 0x10000, 16, 16, 1, 1, 3, 0) && cv_of(d9) && h_cv_vp9ev22(d9) == d9 + CV_EVENT_OFF);
        assert(!h_cv_vp9fin(d9) && !cv_of(d9) && h_cv_vp9ev22(d9) == NULL);
        memset(d9, 0, sizeof d9); assert(!h_cv_av1tm(d9, m9, 0, 0x10000, 16, 16, 1, 1, 3, 0) && cv_of(d9) && cv_of(d9)->codec == CV_AV1 && cv_of(d9)->av1 && h_cv_av1ev22(d9) == d9 + CV_EVENT_OFF);
        assert(!h_cv_av1fin(d9) && !cv_of(d9) && !g_av1_live);
        memset(d9, 0, sizeof d9); assert(!h_cv_av1rm(d9, 0x10000, 16, 16, 1, 1, 3, 0) && cv_of(d9) && cv_of(d9)->codec == CV_AV1 && !h_cv_av1fin(d9) && !g_av1_live);
        _Alignas(16) uint8_t pa[0x40]; memset(pa, 0x55, sizeof pa); assert(h_cv_aliaspool(pa) == CV_R(2004) && !pool_of(pa));
        h_cv_poolfin(p9);
    }
    {   _Alignas(16) static uint8_t dh[0x400], ph[0x40], mh[64 * 64 * 4]; static uint8_t yuv[64 * 64 * 3 / 2];
        cv_pref p64 = h_cv_pref(64, 64, 3, 0);
        assert(!h_cv_poolsize(&size, &align, &p64, 1) && !h_cv_pool(ph, mh, size, &p64, 1, 0));
        assert(!h_cv_h264init2(dh, 0x10000, 64, 64, 1, 1, 3) && cv_of(dh) && cv_of(dh)->codec == CV_H264 && cv_of(dh)->avc);
        assert(!h_cv_h264reg(dh, 0, ph, 0) && !h_cv_h264acq(&idx, &sync, dh));
        h_cv_h264rel(dh, 0, sync); assert(!h_cv_h264acq(&idx, &sync, dh) && !h_gs_trywait(&sync));
        assert(!h_cv_h264submit(&id, dh, h264, sizeof h264, 5000000, true) && h_gs_trywait(&sync));   // end of stream: VideoToolbox gives its picture out
        assert(!h_cv_h264props(&w, &h, &cw, &ch, &ts, &eos, dh, 0) && w == 64 && h == 64 && ts == 5000000 && eos);
        assert(!h_cv_nv12(ph, 0, 0, 0, 64, 64, yuv, 64, 4096, yuv + 4096, 64, 2048));
        for (int i = 0; i < 4096; i++) assert(abs(yuv[i] - 81) <= 4);
        for (int i = 4096; i < 6144; i += 2) { assert(abs(yuv[i] - 90) <= 6); assert(abs(yuv[i + 1] - 240) <= 6); }
        assert(!h_cv_h264fin(dh) && !cv_of(dh)); h_cv_poolfin(ph);
    }
    // The decode thread (the default): Submit returns at once and the picture comes from the thread; pictures waiting and packets not yet decoded count together, Submit
    // refuses at CV_HOLD of them; a flush drops the queued packets and waits for a decode running; finalize ends the thread
    unsetenv("DM_CODEC_THREAD");
    {   _Alignas(16) static uint8_t dt[0x400], pt[0x40], mt[16 * 16 * 4];
        assert(!h_cv_poolsize(&size, &align, &pref, 1) && !h_cv_pool(pt, mt, size, &pref, 1, 0));
        assert(!h_cv_vp9init2(dt, 0x10000, 16, 16, 1, 1, 3) && cv_of(dt) && cv_of(dt)->th_ok);
        assert(!h_cv_vp9reg(dt, 0, pt, 0) && !h_cv_vp9acq(&idx, &sync, dt) && idx == 0);
        for (int i = 0; i < 4; i++) {
            h_cv_vp9rel(dt, 0, sync); assert(!h_cv_vp9acq(&idx, &sync, dt) && idx == 0);
            assert(!h_cv_vp9submit(&id, dt, vp9, sizeof vp9, 50000 + i, false));
            for (int k = 0; k < 5000 && !h_gs_trywait(&sync); k++) usleep(1000);
            assert(h_gs_trywait(&sync) && !h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dt, 0) && w == 16 && ts == 50000 + i && !eos);
        }
        int n = 0; while (n < 64 && !h_cv_vp9submit(&id, dt, vp9, sizeof vp9, 60000 + n, false)) n++;   // buffer 0 holds a picture the game has not released
        assert(n == CV_HOLD && h_cv_vp9submit(&id, dt, vp9, sizeof vp9, 70000, false) == CV_BUSY);
        assert(!h_cv_vp9flush(dt) && !cv_of(dt)->nq && !cv_of(dt)->busy && !cv_of(dt)->nheld);
        h_cv_vp9rel(dt, 0, sync); assert(!h_cv_vp9acq(&idx, &sync, dt) && idx == 0);   // after the flush the stream starts again at the next key frame
        assert(!h_cv_vp9submit(&id, dt, vp9, sizeof vp9, 80000, false));
        for (int k = 0; k < 5000 && !h_gs_trywait(&sync); k++) usleep(1000);
        assert(h_gs_trywait(&sync) && !h_cv_vp9props(&w, &h, &cw, &ch, &ts, &eos, dt, 0) && ts == 80000);
        for (int i = 0; i < 3; i++) (void)h_cv_vp9submit(&id, dt, vp9, sizeof vp9, 90000 + i, false);   // finalize with packets still queued
        assert(!h_cv_vp9fin(dt) && !cv_of(dt)); h_cv_poolfin(pt);
    }
    {   _Alignas(16) uint8_t da[0x400]; memset(da, 0xab, sizeof da);   // AV1: a host decoder (VideoToolbox or dav1d; the stand-ins here), freed with it
        assert(!h_cv_av1init2(da, 0x10000, 16, 16, 1, 1, 3) && cv_of(da) && cv_of(da)->codec == CV_AV1 && !h_cv_av1fin(da) && !cv_of(da) && !g_av1_live); }
    printf("t_codec_video: all passed\n");
    return 0;
}
