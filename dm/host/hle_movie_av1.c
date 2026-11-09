// AV1 for the nn::codec decoders (hle_codec_video.c): Fire Emblem Fortune's Weave's 82 movies are AV1 in WebM (2560x1440). Two backends behind one interface: Apple's VideoToolbox, whose AV1
// decoder is hardware only (Apple M3 and A17 Pro on), and dav1d (dm/third_party/dav1d) on older chips, for a stream VideoToolbox refuses, or with DM_AV1_DAV1D=1 on any chip. The movie module
// aborts on a decoder that does not initialize (movie::detail::InitializeDecoderByTransferMemory, subsdk2+0xf9b4), so without one a game with AV1 movies could not run there.
// VideoToolbox wants a format description holding the stream's av1C record (ISO-BMFF AV1 binding, 2.3.3): four bytes read from the sequence header, then that header's OBU. The decoder API takes no
// configuration for AV1, so the sequence header comes from the stream, where every key frame carries one (AV1 5.5); a changed one makes a new session. A temporal unit goes in whole, its OBUs in
// the low-overhead format WebM and the SDK's demuxer keep them in (each with its size). Decoding is synchronous and a temporal unit shows at most one frame, in presentation order, so no queue.
// On Android a third backend: the phone's hardware AV1 decoder through AMediaCodec ("video/av01"; Qualcomm's c2.qti.av1.decoder on the Snapdragon 8 Gen 2 on), configured as VideoToolbox is,
// from the stream's first sequence header (its av1C record as csd-0), output as NV12 in a ByteBuffer read through its stride, slice height and crop (android/av_android.h). dav1d on the CPU could
// not keep up with Ocarina of Time's 1920x1080 60 fps opening once the phone throttled (a Galaxy S25 at 2 GHz: 87-216 pictures in 5 s, the movie player waiting for them), the hardware
// decoder costs the CPU nothing. Only a hardware decoder is taken: Android's own software ones (c2.android.*, OMX.google.*) are not faster than dav1d. MediaCodec gives a picture a few
// packets after its packet, so a picture carries its packet's time (mav1_picture.has_pts); the end of a stream drains them all. DM_AV1_DAV1D=1 keeps dav1d
#include "hle.h"
#include <unistd.h>
#if PLAT_APPLE
#include <VideoToolbox/VideoToolbox.h>
#endif
#if PLAT_ANDROID
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdatomic.h>
#include <math.h>
#include "android/av_android.h"
#endif
#include "dav1d/dav1d.h"
#include "hle_movie_av1.h"

#define MC_QMAX 16
struct mav1 {
    uint8_t seq[512]; size_t nseq;
#if PLAT_APPLE
    CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs;
    CVPixelBufferRef got, locked;
#endif
#if PLAT_ANDROID
    AMediaCodec *mc; char mcname[64]; nx_yuv_layout L; int bt709, full, eos_sent, eos_done, pending, ndiag; int64_t next_pts;   // pending: packets in without a picture out; ndiag: pictures probed (mc_keep); next_pts: mav1_set_time
    struct { uint8_t *nv12; int w, h; int64_t pts; } q[MC_QMAX]; int nq;   // pictures taken from the decoder, in order
    uint8_t *cur;   // the picture the last call gave (freed at the next)
#endif
    int soft;                     // dav1d decodes: no hardware decoder, DM_AV1_DAV1D=1, or a stream the hardware decoder refused (until mav1_reset)
    Dav1dContext *dav1d; Dav1dPicture pic; int has_pic;
    uint8_t *y8, *uv; size_t ny8, nuv;   // dav1d's picture as NV12: the chroma interleaved, and the luma of a 10- or 12-bit stream shifted to 8 bits
    int red;
};
#if PLAT_APPLE
static int hw_av1(void) { const char *e = getenv("DM_AV1_DAV1D"); return !(e && *e && *e != '0') && VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1); }
#elif PLAT_ANDROID
static _Atomic int g_mc_av1 = -1;   // the phone has a hardware AV1 decoder: -1 not known yet (the first stream finds out), 0 no, 1 yes
static int hw_av1(void) { const char *e = getenv("DM_AV1_DAV1D"); return !(e && *e && *e != '0') && atomic_load(&g_mc_av1) != 0; }
#else
static int hw_av1(void) { return 0; }
#endif
mav1 *mav1_new(void) { mav1 *a = calloc(1, sizeof *a); if (!a) die("movie: AV1 decoder allocation failed"); a->soft = !hw_av1(); return a; }
#if PLAT_ANDROID
const char *mav1_backend(const mav1 *a) { return a->soft ? "dav1d" : a->mcname[0] ? a->mcname : "MediaCodec"; }
#else
const char *mav1_backend(const mav1 *a) { return a->soft ? "dav1d" : "VideoToolbox"; }
#endif
static void unlock_picture(mav1 *a) {
#if PLAT_APPLE
    if (a->locked) { CVPixelBufferUnlockBaseAddress(a->locked, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(a->locked); a->locked = NULL; }
#endif
#if PLAT_ANDROID
    free(a->cur); a->cur = NULL;
#endif
    if (a->has_pic) { dav1d_picture_unref(&a->pic); a->has_pic = 0; }
}
static void av1_close(mav1 *a) {
#if PLAT_APPLE
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
    if (a->got) { CVPixelBufferRelease(a->got); a->got = NULL; }
#elif PLAT_ANDROID
    if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; }
    for (int i = 0; i < a->nq; i++) free(a->q[i].nv12);
    a->nq = 0; a->eos_sent = a->eos_done = a->pending = 0;
#else
    (void)a;
#endif
}
void mav1_reset(mav1 *a) { unlock_picture(a); av1_close(a); a->nseq = 0; if (a->dav1d) dav1d_flush(a->dav1d); a->soft = !hw_av1(); }
void mav1_free(mav1 *a) { if (a) { mav1_reset(a); if (a->dav1d) dav1d_close(&a->dav1d); free(a->y8); free(a->uv); free(a); } }

// ---- The sequence header (AV1 5.5), read as far as the colour config ----
typedef struct { const uint8_t *p; size_t n, bit; int bad; } av1_bits;
static uint32_t rb(av1_bits *b, unsigned n) {
    uint32_t v = 0;
    if (b->bit + n > b->n * 8) { b->bad = 1; return 0; }
    while (n--) { v = v << 1 | ((b->p[b->bit / 8] >> (7 - b->bit % 8)) & 1); b->bit++; }
    return v;
}
static void uvlc(av1_bits *b) { unsigned z = 0; while (!b->bad && !rb(b, 1)) if (++z >= 32) return; if (z < 32) rb(b, z); }
static size_t leb128(const uint8_t *p, size_t n, uint64_t *v) {
    *v = 0;
    for (size_t i = 0; i < n && i < 8; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (i * 7); if (!(p[i] & 0x80)) return i + 1; }
    return 0;
}
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h) {
    if (n < 2 || (obu[0] >> 3 & 15) != 1 || !(obu[0] & 2)) return 0;   // a sequence header with a size field
    size_t at = obu[0] & 4 ? 2 : 1; uint64_t size;
    size_t l = at < n ? leb128(obu + at, n - at, &size) : 0;
    if (!l || size > n - at - l) return 0;
    av1_bits b = {obu + at + l, (size_t)size, 0, 0};
    unsigned profile = rb(&b, 3); rb(&b, 1); unsigned reduced = rb(&b, 1), level = 0, tier = 0;
    unsigned decoder_model = 0, delay_len = 0;
    if (reduced) level = rb(&b, 5);
    else {
        if (rb(&b, 1)) {   // timing_info
            rb(&b, 32); rb(&b, 32); if (rb(&b, 1)) uvlc(&b);
            if ((decoder_model = rb(&b, 1))) { delay_len = rb(&b, 5) + 1; rb(&b, 32); rb(&b, 5); rb(&b, 5); }
        }
        unsigned display_delay = rb(&b, 1), points = rb(&b, 5) + 1;
        for (unsigned i = 0; i < points && !b.bad; i++) {
            rb(&b, 12); unsigned lv = rb(&b, 5), t = lv > 7 ? rb(&b, 1) : 0;
            if (!i) { level = lv; tier = t; }
            if (decoder_model && rb(&b, 1)) { rb(&b, delay_len); rb(&b, delay_len); rb(&b, 1); }
            if (display_delay && rb(&b, 1)) rb(&b, 4);
        }
    }
    unsigned wb = rb(&b, 4) + 1, hb = rb(&b, 4) + 1;
    *w = (int)rb(&b, wb) + 1; *h = (int)rb(&b, hb) + 1;
    if (!reduced && rb(&b, 1)) { rb(&b, 4); rb(&b, 3); }   // frame ids
    rb(&b, 3);   // 128x128 superblocks, filter intra, intra edge filter
    if (!reduced) {
        rb(&b, 4);   // inter-intra, masked compound, warped motion, dual filter
        unsigned order_hint = rb(&b, 1);
        if (order_hint) rb(&b, 2);   // jnt_comp, ref_frame_mvs
        unsigned tools = rb(&b, 1) ? 2 : rb(&b, 1);
        if (tools && !rb(&b, 1)) rb(&b, 1);   // force_integer_mv
        if (order_hint) rb(&b, 3);
    }
    rb(&b, 3);   // superres, cdef, restoration
    unsigned hbd = rb(&b, 1), twelve = profile == 2 && hbd ? rb(&b, 1) : 0, mono = profile == 1 ? 0 : rb(&b, 1), cp = 2, tc = 2, mc = 2, ssx = 1, ssy = 1, pos = 0;
    if (rb(&b, 1)) { cp = rb(&b, 8); tc = rb(&b, 8); mc = rb(&b, 8); }
    if (!mono) {
        if (cp == 1 && tc == 13 && mc == 0) ssx = ssy = 0;   // sRGB
        else {
            rb(&b, 1);   // colour range
            if (profile == 1) ssx = ssy = 0;
            else if (profile == 2) { if (twelve) { ssx = rb(&b, 1); ssy = ssx ? rb(&b, 1) : 0; } else { ssx = 1; ssy = 0; } }
            if (ssx && ssy) pos = rb(&b, 2);
        }
    }
    if (b.bad) return 0;
    c[0] = 0x81;   // marker, version 1
    c[1] = (uint8_t)(profile << 5 | (level & 31));
    c[2] = (uint8_t)(tier << 7 | hbd << 6 | twelve << 5 | mono << 4 | ssx << 3 | ssy << 2 | pos);
    c[3] = 0;
    return 1;
}
// The sequence header OBU of a temporal unit, or none
static const uint8_t *find_seq(const uint8_t *p, size_t n, size_t *len) {
    for (size_t i = 0; i < n;) {
        if (!(p[i] & 2)) return NULL;   // no size field: not the low-overhead format
        size_t hdr = p[i] & 4 ? 2 : 1; uint64_t size;
        size_t l = i + hdr < n ? leb128(p + i + hdr, n - i - hdr, &size) : 0;
        if (!l || size > n - i - hdr - l) return NULL;
        if ((p[i] >> 3 & 15) == 1) { *len = hdr + l + (size_t)size; return p + i; }
        i += hdr + l + (size_t)size;
    }
    return NULL;
}
#if PLAT_APPLE
static void av1_on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)src; (void)fl; (void)pts; (void)dur; mav1 *a = ref;
    if (st || !img) { if (st && !a->red++) LOG("[RED] movie: AV1 decode error (VideoToolbox %d, first case)", (int)st); return; }
    if (a->got) CVPixelBufferRelease(a->got);
    a->got = CVPixelBufferRetain(img);
}
static int av1_open(mav1 *a, const uint8_t *seq, size_t n) {
    av1_close(a);
    uint8_t rec[4 + sizeof a->seq]; int w = 0, h = 0;
    if (n > sizeof a->seq || !mav1_av1c(seq, n, rec, &w, &h)) { if (!a->red++) LOG("[RED] movie: an AV1 sequence header the host cannot read (%zu bytes)", n); return 0; }
    memcpy(rec + 4, seq, n);
    CFDataRef data = CFDataCreate(NULL, rec, (CFIndex)(4 + n));
    const void *ak[] = {CFSTR("av1C")}, *av[] = {data};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(data);
    const void *ek[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *ev[] = {atoms};
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(atoms);
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_AV1, w, h, ext, &a->fd); CFRelease(ext);
    if (st) { if (!a->red++) LOG("[RED] movie: no format description for AV1 (%d)", (int)st); return 0; }
    int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {num};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(num);
    VTDecompressionOutputCallbackRecord cb = {av1_on_frame, a};
    st = VTDecompressionSessionCreate(NULL, a->fd, NULL, attrs, &cb, &a->vs); CFRelease(attrs);
    if (st) { if (!a->red++) LOG("[RED] movie: VideoToolbox cannot create an AV1 session (%d)", (int)st); a->vs = NULL; return 0; }
    memcpy(a->seq, seq, n); a->nseq = n;
    static int once; if (!once++) LOG("[movie] AV1: VideoToolbox decoding %dx%d, profile %d level %d", w, h, rec[1] >> 5, rec[1] & 31);
    return 1;
}
#endif
#if PLAT_ANDROID
// ---- MediaCodec (the phone's hardware AV1 decoder) ----
#define MC_PENDING_MAX 8   // packets in the decoder without a picture out before a decode waits for one
static int mc_soft_name(const char *n) { return !strncmp(n, "c2.android.", 11) || !strncmp(n, "OMX.google.", 11) || strstr(n, "dav1d") || strstr(n, "gav1") || strstr(n, ".sw."); }
static int mc_open(mav1 *a, const uint8_t *seq, size_t n) {
    av1_close(a);
    uint8_t rec[4 + sizeof a->seq]; int w = 0, h = 0;
    if (n > sizeof a->seq || !mav1_av1c(seq, n, rec, &w, &h)) { if (!a->red++) LOG("[RED] movie: an AV1 sequence header the host cannot read (%zu bytes)", n); return 0; }
    memcpy(rec + 4, seq, n);
    a->mc = AMediaCodec_createDecoderByType("video/av01");
    // NULL is also a decoder the phone has but could not give now (its instances taken): this stream stays on dav1d and the next one asks again. A software decoder is the
    // phone's own answer and is kept for the session (below)
    if (!a->mc) { static int k; if (!k++) LOG("[movie] AV1: MediaCodec gave no AV1 decoder: dav1d decodes this stream"); return 0; }
    char *name = NULL; AMediaCodec_getName(a->mc, &name); snprintf(a->mcname, sizeof a->mcname, "%s", name ? name : "MediaCodec"); if (name) AMediaCodec_releaseName(a->mc, name);
    if (mc_soft_name(a->mcname)) {
        LOG("[movie] AV1: the phone's AV1 decoder is %s, a software one: dav1d decodes", a->mcname);
        AMediaCodec_delete(a->mc); a->mc = NULL; a->mcname[0] = 0; atomic_store(&g_mc_av1, 0); return 0;
    }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/av01");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);   // COLOR_FormatYUV420SemiPlanar: NV12 in the ByteBuffer
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, w * h * 3 / 2 > (1 << 20) ? w * h * 3 / 2 : 1 << 20);
    AMediaFormat_setBuffer(f, "csd-0", rec, 4 + n);   // the av1C record with the sequence header OBU (AV1 in ISO-BMFF 2.3.3), as MediaCodec takes it
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    if (st != AMEDIA_OK) {
        LOG("[RED] movie: %s refuses the AV1 stream (%d, %dx%d, profile %d): dav1d decodes it", a->mcname, (int)st, w, h, rec[1] >> 5);
        AMediaCodec_delete(a->mc); a->mc = NULL; a->mcname[0] = 0; return 0;
    }
    atomic_store(&g_mc_av1, 1);
    a->L = (nx_yuv_layout){NXYUV_NV12, w, h, 0, 0, w, h}; a->bt709 = h >= 720; a->full = 0; a->eos_sent = a->eos_done = 0; a->nq = 0; a->ndiag = 0;
    memcpy(a->seq, seq, n); a->nseq = n;
    LOG("[movie] AV1: %s (hardware) decoding %dx%d, profile %d level %d", a->mcname, w, h, rec[1] >> 5, rec[1] & 31);
    return 1;
}
static void mc_out_format(mav1 *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); int32_t v, w = a->L.cw, h = a->L.ch, stride, slice, cf = 21;
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w); AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &cf);
    if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &stride) || stride < w) stride = w;
    if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice) || slice < h) slice = h;
    int32_t l = 0, t = 0, r = w - 1, b = h - 1;
    if (!AMediaFormat_getRect(f, AMEDIAFORMAT_KEY_DISPLAY_CROP, &l, &t, &r, &b)) {
        AMediaFormat_getInt32(f, "crop-left", &l); AMediaFormat_getInt32(f, "crop-top", &t); AMediaFormat_getInt32(f, "crop-right", &r); AMediaFormat_getInt32(f, "crop-bottom", &b);
    }
    if (l < 0 || t < 0 || r < l || b < t || r >= stride || b >= slice) { l = t = 0; r = w - 1; b = h - 1; }
    a->L = (nx_yuv_layout){nx_yuv_kind(cf), stride, slice, l, t, r - l + 1, b - t + 1};
    a->bt709 = a->L.ch >= 720; if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_STANDARD, &v)) a->bt709 = v == 1 ? 1 : v == 6 ? 2 : 0;   // BT709, BT2020 (2), else BT.601
    a->full = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_RANGE, &v) && v == 1;
    AMediaFormat_delete(f);
    if (a->L.kind == NXYUV_NONE && !a->red++) LOG("[RED] movie: %s outputs colour format %#x, which the host cannot read: no pictures", a->mcname, cf);
    LOG("[movie] AV1 output: %d×%d (crop %d,%d %d×%d), stride %d, slice height %d, colour format %#x, %s %s range", w, h, l, t, a->L.cw, a->L.ch, stride, slice, cf,
        a->bt709 == 2 ? "BT.2020" : a->bt709 ? "BT.709" : "BT.601", a->full ? "full" : "limited");
}
// Mean and spread of the 8 rows from byte off (the probe below); -1 when the buffer does not reach them
static void mc_rows(const uint8_t *p, size_t n, size_t off, size_t stride, double *mean, double *sd) {
    size_t len = stride * 8; *mean = *sd = -1; if (!stride || off + len > n) return;
    double s = 0, s2 = 0; for (size_t i = 0; i < len; i++) { double v = p[off + i]; s += v; s2 += v * v; }
    *mean = s / len; double var = s2 / len - *mean * *mean; *sd = var > 0 ? sqrt(var) : 0;
}
static void mc_keep(mav1 *a, const uint8_t *p, size_t n, size_t cap, int64_t pts) {
    nx_yuv_layout L = a->L;
    if (L.kind == NXYUV_NONE) return;
    // The first pictures of each stream, where the decoder's first output format (slice height 1088) and the next one (1080) disagree, and green stripes showed before
    // Ocarina of Time's movies (Galaxy S25, c2.qti.av1.decoder): the buffer's size, and the 8 rows where the chroma plane starts under each slice height. Chroma in a
    // movie sits near 128 with a small spread; luma, or padding, does not
    if (a->ndiag < 3) {
        a->ndiag++;
        double m1, s1, m2, s2; size_t r1 = (size_t)L.stride * (size_t)(L.ct + L.ch), r2 = (size_t)L.stride * (size_t)L.slice;
        mc_rows(p, n, r1, (size_t)L.stride, &m1, &s1); mc_rows(p, n, r2, (size_t)L.stride, &m2, &s2);
        LOG("[movie] AV1 picture %d of the stream: %zu bytes (buffer %zu), layout stride %d slice %d crop %d,%d %dx%d needs %zu; after row %d: mean %.1f spread %.1f; after row %d: mean %.1f spread %.1f",
            a->ndiag, n, cap, L.stride, L.slice, L.cl, L.ct, L.cw, L.ch, nx_yuv_bytes(&L), L.ct + L.ch, m1, s1, L.slice, m2, s2);
    }
    if (n < nx_yuv_bytes(&L) && L.slice > L.ct + L.ch) L.slice = L.ct + L.ch;   // a decoder whose slice height overstates its buffer: the picture's own height
    int W = (L.cw + 1) & ~1, H = (L.ch + 1) & ~1;
    uint8_t *f = malloc((size_t)W * H * 3 / 2); if (!f) return;
    if (nx_yuv_to_nv12(p, n, &L, f)) { free(f); if (!a->red++) LOG("[RED] movie: %s gave a %zu-byte picture, smaller than its format (stride %d, slice height %d)", a->mcname, n, L.stride, L.slice); return; }
    if (a->nq == MC_QMAX) { if (!a->red++) LOG("[RED] movie: AV1 picture queue full, the oldest picture dropped"); free(a->q[0].nv12); memmove(a->q, a->q + 1, (size_t)--a->nq * sizeof a->q[0]); }
    a->q[a->nq].nv12 = f; a->q[a->nq].w = W; a->q[a->nq].h = H; a->q[a->nq].pts = pts; a->nq++;
}
static void mc_drain(mav1 *a, int64_t wait_us) {
    while (a->mc) {
        AMediaCodecBufferInfo info; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &info, wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { mc_out_format(a); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return;
        wait_us = 0;
        size_t sz = 0; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &sz);
        if (b && info.size > 0 && (size_t)info.offset + (size_t)info.size <= sz) { mc_keep(a, b + info.offset, (size_t)info.size, sz - (size_t)info.offset, (int64_t)info.presentationTimeUs); if (a->pending) a->pending--; }
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) a->eos_done = 1;
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        if (a->eos_done) return;
    }
}
static ssize_t mc_input_slot(mav1 *a) {   // an input buffer, taking pictures out meanwhile (a full decoder frees inputs only when its outputs are taken); about a second at most
    for (int k = 0; k < 50; k++) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 10000);
        if (i >= 0) return i;
        mc_drain(a, 10000);
    }
    return -1;
}
static int decode_mc(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out) {
    if (data && n) {
        if (a->eos_sent) { AMediaCodec_flush(a->mc); for (int i = 0; i < a->nq; i++) free(a->q[i].nv12); a->nq = 0; a->eos_sent = a->eos_done = 0; a->pending = 0; }   // packets after an end of stream: a new stream
        ssize_t i = mc_input_slot(a);
        if (i < 0) { if (!a->red++) LOG("[RED] movie: %s takes no input (first case): a temporal unit dropped", a->mcname); return -1; }
        size_t cap = 0; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
        if (!b || n > cap) { AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, 0, 0); if (!a->red++) LOG("[RED] movie: an AV1 temporal unit of %zu bytes, the input buffer has %zu", n, cap); return -1; }
        memcpy(b, data, n);
        if (AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, n, (uint64_t)a->next_pts, 0) != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: an AV1 temporal unit not queued (first case)"); return -1; }
        a->pending++;
        mc_drain(a, a->nq ? 0 : 8000);
        for (int k = 0; k < 10 && a->pending > MC_PENDING_MAX && !a->nq; k++) mc_drain(a, 20000);
    } else if (!a->eos_done) {   // the end of the stream: every picture still inside comes out, one a call
        if (!a->eos_sent) { ssize_t i = mc_input_slot(a); if (i >= 0) { AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); a->eos_sent = 1; } }
        for (int k = 0; k < 50 && !a->eos_done && !a->nq; k++) mc_drain(a, 20000);
    }
    if (!a->nq) return 0;
    a->cur = a->q[0].nv12; int W = a->q[0].w, H = a->q[0].h; int64_t pts = a->q[0].pts;
    memmove(a->q, a->q + 1, (size_t)--a->nq * sizeof a->q[0]);
    *out = (mav1_picture){a->cur, a->cur + (size_t)W * H, (size_t)W, (size_t)W, W, H, a->bt709 == 2 ? 5 : a->bt709 ? 2 : 1, a->full, 1, pts};
    return 1;
}
#endif
// ---- dav1d ----
static void d_log(void *cookie, const char *fmt, va_list ap) {
    (void)cookie; char b[256]; vsnprintf(b, sizeof b, fmt, ap);
    size_t l = strlen(b); if (l && b[l - 1] == '\n') b[l - 1] = 0;
    LOG("[movie] dav1d: %s", b);
}
static int grow(uint8_t **p, size_t *have, size_t want) {
    if (*have >= want) return 1;
    uint8_t *q = realloc(*p, want); if (!q) return 0;
    *p = q; *have = want; return 1;
}
// dav1d's planar picture as the NV12 the interface gives: the luma in place for 8 bits, else shifted down into y8; the chroma interleaved into uv (subsampled from 4:2:2 and 4:4:4, grey for 4:0:0)
static int dav1d_nv12(mav1 *a, const Dav1dPicture *p, mav1_picture *out) {
    int w = p->p.w, h = p->p.h, sh = p->p.bpc - 8, l = p->p.layout, cw = (w + 1) / 2, ch = (h + 1) / 2;
    int sx = l != DAV1D_PIXEL_LAYOUT_I444, sy = l == DAV1D_PIXEL_LAYOUT_I420;
    if (!grow(&a->uv, &a->nuv, (size_t)cw * 2 * ch) || (sh && !grow(&a->y8, &a->ny8, (size_t)w * h))) return 0;
    const uint8_t *y = p->data[0]; size_t ys = (size_t)p->stride[0];
    if (sh) {
        for (int r = 0; r < h; r++) { const uint16_t *s = (const uint16_t *)((const uint8_t *)p->data[0] + (ptrdiff_t)r * p->stride[0]); uint8_t *o = a->y8 + (size_t)r * w; for (int x = 0; x < w; x++) o[x] = (uint8_t)(s[x] >> sh); }
        y = a->y8; ys = (size_t)w;
    }
    for (int r = 0; r < ch; r++) {
        uint8_t *o = a->uv + (size_t)r * cw * 2;
        if (l == DAV1D_PIXEL_LAYOUT_I400) { memset(o, 128, (size_t)cw * 2); continue; }
        const uint8_t *u = (const uint8_t *)p->data[1] + (ptrdiff_t)(sy ? r : 2 * r) * p->stride[1], *v = (const uint8_t *)p->data[2] + (ptrdiff_t)(sy ? r : 2 * r) * p->stride[1];
        for (int x = 0; x < cw; x++) {
            int c = sx ? x : 2 * x;
            o[2 * x] = sh ? (uint8_t)(((const uint16_t *)u)[c] >> sh) : u[c]; o[2 * x + 1] = sh ? (uint8_t)(((const uint16_t *)v)[c] >> sh) : v[c];
        }
    }
    unsigned mc = p->seq_hdr->mtrx;   // matrix coefficients (AV1 6.4.2); unspecified, BT.601 below 720 lines and BT.709 from there, as VideoToolbox attaches to such a stream's pictures
    if (mc == DAV1D_MC_UNKNOWN) mc = h < 720 ? DAV1D_MC_BT601 : DAV1D_MC_BT709;
    *out = (mav1_picture){y, a->uv, ys, (size_t)cw * 2, w, h, mc == DAV1D_MC_BT709 ? 2 : mc == DAV1D_MC_BT2020_NCL || mc == DAV1D_MC_BT2020_CL ? 5 : 1, p->seq_hdr->color_range};
    return 1;
}
static int decode_dav1d(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out) {
    if (!a->dav1d) {
        Dav1dSettings st; dav1d_default_settings(&st);
        int nth = 4;
        const char *env_th = getenv("DM_DAV1D_THREADS");
        if (env_th && *env_th) nth = atoi(env_th);
        else { long ncpu = sysconf(_SC_NPROCESSORS_ONLN); if (ncpu >= 8) nth = 8; else if (ncpu >= 4) nth = 4; }
        st.n_threads = nth > 0 ? nth : 4;
        const char *env_del = getenv("DM_DAV1D_FRAME_DELAY");
        if (env_del && *env_del) st.max_frame_delay = atoi(env_del);
        else st.max_frame_delay = 0;   // 0 = automatic frame multithreading across CPU cores
        st.logger.cookie = a; st.logger.callback = d_log;
        if (dav1d_open(&a->dav1d, &st)) { a->dav1d = NULL; if (!a->red++) LOG("[RED] movie: dav1d could not be opened"); return -1; }
        static int once; if (!once++) LOG("[movie] AV1: dav1d %s decoding (%s, %d threads, max_frame_delay %d)", dav1d_version(), hw_av1() ? "a stream VideoToolbox refused" : getenv("DM_AV1_DAV1D") ? "DM_AV1_DAV1D" : "no hardware AV1 decoder", st.n_threads, st.max_frame_delay);
    }
    if (!data || !n) {
        Dav1dPicture p = {0};
        if (!dav1d_get_picture(a->dav1d, &p)) {
            if (a->has_pic) dav1d_picture_unref(&a->pic);
            a->pic = p; a->has_pic = 1;
            if (!dav1d_nv12(a, &a->pic, out)) return -1;
            return 1;
        }
        return 0;
    }
    Dav1dData d = {0}; uint8_t *buf = dav1d_data_create(&d, n);
    if (!buf) return -1;
    memcpy(buf, data, n);
    Dav1dPicture p = {0}; int got = 0;
    for (int tries = 0; d.sz && tries < 64; tries++) {
        int r = dav1d_send_data(a->dav1d, &d);
        if (r && r != DAV1D_ERR(EAGAIN)) { dav1d_data_unref(&d); if (!a->red++) LOG("[RED] movie: dav1d refused a temporal unit (%d, first case)", r); return -1; }
        while (!dav1d_get_picture(a->dav1d, &p)) { if (a->has_pic) dav1d_picture_unref(&a->pic); a->pic = p; a->has_pic = 1; got = 1; break; }
    }
    if (d.sz) { dav1d_data_unref(&d); if (!a->red++) LOG("[RED] movie: dav1d took no more data, a temporal unit dropped (first case)"); return -1; }
    if (!got) {
        if (!dav1d_get_picture(a->dav1d, &p)) { if (a->has_pic) dav1d_picture_unref(&a->pic); a->pic = p; a->has_pic = 1; got = 1; }
    }
    if (!got) return 0;
    if (!dav1d_nv12(a, &a->pic, out)) { if (!a->red++) LOG("[RED] movie: no memory for a %dx%d AV1 picture", a->pic.p.w, a->pic.p.h); return -1; }
    return 1;
}

int mav1_decode(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out) {
    unlock_picture(a);
    if (a->soft) return decode_dav1d(a, data, n, out);
#if PLAT_APPLE
    size_t seqn = 0; const uint8_t *seq = find_seq(data, n, &seqn);
    if (seq && (seqn != a->nseq || memcmp(seq, a->seq, seqn) || !a->vs) && !av1_open(a, seq, seqn)) {   // a stream VideoToolbox does not take: dav1d from this temporal unit on (it holds the sequence header)
        LOG("[movie] AV1: dav1d decodes the stream instead"); av1_close(a); a->soft = 1;
        return decode_dav1d(a, data, n, out);
    }
    if (!a->vs) return 0;   // the stream's first sequence header has not come
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL; size_t ssz = n;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, n, NULL, NULL, 0, n, 0, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(data, bb, 0, n);
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 0, NULL, 1, &ssz, &sb);
    if (!st) st = VTDecompressionSessionDecodeFrame(a->vs, sb, 0, NULL, NULL);   // flags 0: synchronous, the callback has run when this returns
    if (sb) CFRelease(sb);
    if (bb) CFRelease(bb);
    if (st) { if (!a->red++) LOG("[RED] movie: AV1 decode submission failed (%d, first case)", (int)st); return -1; }
    if (!a->got) return 0;
    CVPixelBufferRef pb = a->got; a->got = NULL; a->locked = pb;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CFStringRef m = CVBufferCopyAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
    *out = (mav1_picture){CVPixelBufferGetBaseAddressOfPlane(pb, 0), CVPixelBufferGetBaseAddressOfPlane(pb, 1), CVPixelBufferGetBytesPerRowOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                          (int)CVPixelBufferGetWidthOfPlane(pb, 0), (int)CVPixelBufferGetHeightOfPlane(pb, 0),
                          !m || CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_709_2) ? 2 : CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_2020) ? 5 : 1, 0};
    if (m) CFRelease(m);
    return 1;
#elif PLAT_ANDROID
    size_t seqn = 0; const uint8_t *seq = data && n ? find_seq(data, n, &seqn) : NULL;
    if (seq && (seqn != a->nseq || memcmp(seq, a->seq, seqn) || !a->mc) && !mc_open(a, seq, seqn)) {   // no hardware decoder, or one that does not take the stream: dav1d from this temporal unit on
        LOG("[movie] AV1: dav1d decodes the stream"); av1_close(a); a->soft = 1;
        return decode_dav1d(a, data, n, out);
    }
    if (!a->mc) return 0;   // the stream's first sequence header has not come
    return decode_mc(a, data, n, out);
#else
    (void)find_seq;
    return decode_dav1d(a, data, n, out);
#endif
}
void mav1_set_time(mav1 *a, int64_t ts) {   // the next temporal unit's time, which a picture coming out later carries (MediaCodec); the other backends give a picture in its own call
#if PLAT_ANDROID
    a->next_pts = ts;
#else
    (void)a; (void)ts;
#endif
}
