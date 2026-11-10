// Pure audio and picture helpers of the Android host (no Android headers, no host state), so tests/t_android_av.c checks them on the Mac:
// a linear resampler for an AAudio stream that would not open at 48 kHz, channel mapping for an AAC decoder whose output layout differs from the track's,
// the SPS picture size for MediaCodec's format, MediaCodec output buffer layouts copied into tight NV12 or RGBA, and what a stream keeps for a hardware decoder that fails inside it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ---------- Resampler ----------
// Pull model, as a device callback needs: rs_need says how many source frames n output frames take, the caller pulls exactly that many, rs_run makes the n frames.
// Linear interpolation between S(i) and S(i + 1); the state keeps two pulled frames (a = S(0), b = S(1)), so the source runs two frames behind and nothing pulled is lost
// between calls. step is source frames per output frame in 16.16 fixed point
#define RS_MAXCH 8
typedef struct { uint32_t step, frac; int ch; int16_t a[RS_MAXCH], b[RS_MAXCH]; } nx_rs;
static inline void rs_init(nx_rs *r, int src_rate, int dst_rate, int ch) {
    memset(r, 0, sizeof *r);
    r->ch = ch < 1 ? 1 : ch > RS_MAXCH ? RS_MAXCH : ch;
    r->step = (uint32_t)(((uint64_t)src_rate << 16) / (uint64_t)(dst_rate > 0 ? dst_rate : 1));
}
static inline uint32_t rs_need(const nx_rs *r, uint32_t n) { return (uint32_t)(((uint64_t)r->frac + (uint64_t)n * r->step) >> 16); }
static inline void rs_run(nx_rs *r, const int16_t *src, uint32_t nsrc, int16_t *dst, uint32_t n) {
    int ch = r->ch;
    for (uint32_t j = 0; j < n; j++) {
        uint64_t pos = (uint64_t)r->frac + (uint64_t)j * r->step; uint32_t i = (uint32_t)(pos >> 16); int32_t f = (int32_t)(pos & 0xffff);
        for (int c = 0; c < ch; c++) {
            // S(0) = a, S(1) = b, S(k) = src[k - 2]
            int32_t s0 = i == 0 ? r->a[c] : i == 1 ? r->b[c] : (i - 2 < nsrc ? src[(i - 2) * ch + c] : 0);
            int32_t s1 = i + 1 == 1 ? r->b[c] : (i - 1 < nsrc ? src[(i - 1) * ch + c] : s0);
            dst[j * ch + c] = (int16_t)(s0 + (((s1 - s0) * f) >> 16));
        }
    }
    uint64_t end = (uint64_t)r->frac + (uint64_t)n * r->step; uint32_t adv = (uint32_t)(end >> 16);
    r->frac = (uint32_t)(end & 0xffff);
    if (adv > nsrc) adv = nsrc;   // the caller pulled fewer than rs_need asked: keep what there is
    // new a = S(adv), new b = S(adv + 1)
    for (int c = 0; c < ch; c++) {
        int16_t na = adv == 0 ? r->a[c] : adv == 1 ? r->b[c] : src[(adv - 2) * ch + c];
        int16_t nb = adv == 0 ? r->b[c] : src[(adv - 1) * ch + c];
        r->a[c] = na; r->b[c] = nb;
    }
}

// ---------- Channel mapping ----------
// Interleaved PCM16 from sch to dch channels: one source channel is copied to every output channel, one output channel takes the mean of all inputs,
// otherwise the first min(sch, dch) channels are copied and the rest are silent (an AAC decoder that turns a mono track with parametric stereo into stereo, say)
// In place (src == dst) works in either direction: a growing layout is walked from the end
static inline void ch_map(const int16_t *src, int sch, int16_t *dst, int dch, size_t frames) {
    for (size_t k = 0; k < frames; k++) {
        size_t i = dch > sch ? frames - 1 - k : k;
        const int16_t *s = src + i * (size_t)sch; int16_t *d = dst + i * (size_t)dch;
        if (sch == dch) { memmove(d, s, (size_t)sch * sizeof *s); continue; }
        if (sch == 1) { for (int c = 0; c < dch; c++) d[c] = s[0]; continue; }
        if (dch == 1) { int32_t m = 0; for (int c = 0; c < sch; c++) m += s[c]; d[0] = (int16_t)(m / sch); continue; }
        int16_t t[RS_MAXCH]; int n = sch < RS_MAXCH ? sch : RS_MAXCH; memcpy(t, s, (size_t)n * sizeof *s);   // src and dst may overlap
        for (int c = 0; c < dch; c++) d[c] = c < n ? t[c] : 0;
    }
}

// ---------- H.264 SPS picture size ----------
// MediaCodec's format wants a width and height before the first packet; the SPS has them (H.264 7.3.2.1.1). Returns 1 with the cropped size, 0 for an SPS it cannot read
typedef struct { const uint8_t *p; size_t n, bit; int bad; } nx_bits;
static inline unsigned nxb_u(nx_bits *b, unsigned n) {
    unsigned v = 0;
    while (n--) { if (b->bit >= b->n * 8) { b->bad = 1; return 0; } v = (v << 1) | ((b->p[b->bit / 8] >> (7 - b->bit % 8)) & 1); b->bit++; }
    return v;
}
static inline unsigned nxb_ue(nx_bits *b) { unsigned z = 0; while (!b->bad && !nxb_u(b, 1)) if (++z > 31) { b->bad = 1; return 0; } return b->bad ? 0 : ((1u << z) - 1) + nxb_u(b, z); }
static inline int nxb_se(nx_bits *b) { unsigned k = nxb_ue(b); return k & 1 ? (int)((k + 1) / 2) : -(int)(k / 2); }
static inline int avc_sps_size(const uint8_t *nal, size_t n, int *w, int *h) {
    if (n < 4 || (nal[0] & 31) != 7 || n > 1024) return 0;
    uint8_t rbsp[1024]; size_t m = 0;
    for (size_t i = 1; i < n; i++) { if (i >= 3 && nal[i] == 3 && !nal[i - 1] && !nal[i - 2]) continue; rbsp[m++] = nal[i]; }
    nx_bits b = {rbsp, m, 0, 0};
    unsigned profile = nxb_u(&b, 8); nxb_u(&b, 16); nxb_ue(&b);
    unsigned chroma = 1;
    switch (profile) {
    case 44: case 83: case 86: case 100: case 110: case 118: case 122: case 128: case 134: case 135: case 138: case 139: case 144: case 244:
        chroma = nxb_ue(&b); if (chroma > 3) return 0;
        if (chroma == 3) nxb_u(&b, 1);
        nxb_ue(&b); nxb_ue(&b); nxb_u(&b, 1);
        if (nxb_u(&b, 1)) for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u) && !b.bad; i++) {
            if (!nxb_u(&b, 1)) continue;
            int last = 8, next = 8;
            for (unsigned j = 0; j < (i < 6 ? 16u : 64u) && !b.bad; j++) { if (next) next = (last + nxb_se(&b)) & 255; if (next) last = next; }
        }
        break;
    }
    nxb_ue(&b);   // log2_max_frame_num_minus4
    unsigned poc = nxb_ue(&b);
    if (poc == 0) nxb_ue(&b);
    else if (poc == 1) { nxb_u(&b, 1); nxb_se(&b); nxb_se(&b); unsigned k = nxb_ue(&b); if (k > 255) return 0; for (unsigned i = 0; i < k; i++) nxb_se(&b); }
    nxb_ue(&b); nxb_u(&b, 1);   // max_num_ref_frames, gaps_in_frame_num_value_allowed_flag
    unsigned wmb = nxb_ue(&b) + 1, hmu = nxb_ue(&b) + 1, frame_mbs_only = nxb_u(&b, 1);
    if (!frame_mbs_only) nxb_u(&b, 1);
    nxb_u(&b, 1);
    int cw = (int)wmb * 16, chh = (int)(hmu * 16 * (2 - frame_mbs_only));
    if (nxb_u(&b, 1)) {
        unsigned l = nxb_ue(&b), r = nxb_ue(&b), t = nxb_ue(&b), bo = nxb_ue(&b);
        int ux = chroma == 1 || chroma == 2 ? 2 : 1, uy = (chroma == 1 ? 2 : 1) * (int)(2 - frame_mbs_only);
        if (chroma == 0) { ux = 1; uy = (int)(2 - frame_mbs_only); }
        cw -= (int)(l + r) * ux; chh -= (int)(t + bo) * uy;
    }
    if (b.bad || cw <= 0 || chh <= 0 || cw > 8192 || chh > 8192) return 0;
    *w = cw; *h = chh; return 1;
}

// ---------- MediaCodec output layouts ----------
// A ByteBuffer output picture: planar I420 (COLOR_FormatYUV420Planar 19 and PackedPlanar 20), semi-planar NV12 (21, PackedSemiPlanar 39, Flexible 0x7f420888 when 21 was asked for,
// Qualcomm's 32m 0x7fa30c04) or NV21 (Qualcomm 0x7fa30c00). stride / slice are the format's "stride" / "slice-height"; the crop rectangle is inclusive of its left / top edge
enum { NXYUV_NONE, NXYUV_I420, NXYUV_NV12, NXYUV_NV21 };
static inline int nx_yuv_kind(int color_format) {
    switch (color_format) {
    case 19: case 20: return NXYUV_I420;
    case 21: case 39: case 0x7f420888: case 0x7fa30c04: return NXYUV_NV12;
    case 0x7fa30c00: return NXYUV_NV21;
    default: return NXYUV_NONE;
    }
}
typedef struct { int kind, stride, slice, cl, ct, cw, ch; } nx_yuv_layout;
// Bytes a picture of this layout must have: the luma at its slice height, then the chroma rows the crop reads (nx_yuv_to_nv12), not the slice height's. Qualcomm's AV1 decoder
// (c2.qti.av1.decoder) gives the first picture of a stream with its luma padded to a slice height of 1088 and 540 chroma rows after it (3,125,760 bytes for 1920x1080):
// counted at 544 chroma rows the buffer looked short, the copy fell back to a slice height of 1080 and read its chroma from the luma's zero padding, which showed as green
// stripes at the top of the first picture of every movie (Ocarina of Time, Galaxy S25)
static inline size_t nx_yuv_bytes(const nx_yuv_layout *L) {
    size_t y = (size_t)L->stride * L->slice, plane = (size_t)(L->slice + 1) / 2, rows = (size_t)(L->ct / 2) + (size_t)((L->ch + 1) / 2);
    if (rows > plane) rows = plane;
    return L->kind == NXYUV_I420 ? y + (plane + rows) * (size_t)((L->stride + 1) / 2) : y + rows * (size_t)L->stride;   // I420: all of U, then the V rows read
}
// The cropped picture as tight NV12 of an even size (W = cw rounded up to even, H likewise): Y stride W, then UV at W * H, stride W.
// Returns 0, or -1 when the buffer is smaller than the layout says
static inline int nx_yuv_to_nv12(const uint8_t *src, size_t n, const nx_yuv_layout *L, uint8_t *dst) {
    if (L->kind == NXYUV_NONE || L->stride < L->cl + L->cw || L->slice < L->ct + L->ch || n < nx_yuv_bytes(L)) return -1;
    int W = (L->cw + 1) & ~1, H = (L->ch + 1) & ~1;
    for (int r = 0; r < H; r++) {
        int sr = L->ct + (r < L->ch ? r : L->ch - 1);
        memcpy(dst + (size_t)r * W, src + (size_t)sr * L->stride + L->cl, (size_t)L->cw);
        if (W > L->cw) dst[(size_t)r * W + W - 1] = dst[(size_t)r * W + L->cw - 1];
    }
    uint8_t *uv = dst + (size_t)W * H; size_t yb = (size_t)L->stride * L->slice;
    int c0 = L->cl / 2, r0 = L->ct / 2, cols = W / 2, rows = H / 2, maxr = (L->slice + 1) / 2 - 1;
    for (int r = 0; r < rows; r++) {
        int sr = r0 + r; if (sr > maxr) sr = maxr;
        uint8_t *o = uv + (size_t)r * W;
        if (L->kind == NXYUV_I420) {
            int cs = (L->stride + 1) / 2; const uint8_t *u = src + yb + (size_t)sr * cs + c0, *v = src + yb + (size_t)((L->slice + 1) / 2) * cs + (size_t)sr * cs + c0;
            for (int c = 0; c < cols; c++) { o[2 * c] = u[c]; o[2 * c + 1] = v[c]; }
        } else {
            const uint8_t *s = src + yb + (size_t)sr * L->stride + 2 * c0;
            if (L->kind == NXYUV_NV12) memcpy(o, s, (size_t)cols * 2);
            else for (int c = 0; c < cols; c++) { o[2 * c] = s[2 * c + 1]; o[2 * c + 1] = s[2 * c]; }
        }
    }
    return 0;
}
// ---------- A hardware decoder that fails inside a stream ----------
// AMediaCodec_dequeueOutputBuffer / dequeueInputBuffer: an index, or -1 (try again later), -2 (output format changed), -3 (output buffers changed); anything below is an error
// (AMEDIA_ERROR_*, AMEDIACODEC_ERROR_*: a decoder in its error state, or one whose resources were reclaimed), after which it gives nothing more. Read as "no picture yet",
// such an error left the movie player without a picture for every packet that followed
static inline int nx_mc_failed(long r) { return r < -3; }
// The packets of a stream since its last key frame (the last temporal unit with a sequence header), copied, so that when the hardware decoder fails the CPU decoder can start
// again from that key frame: given counts the pictures the hardware decoder gave since it, which the CPU decoder's first pictures repeat (they are dropped). lost: more than the
// cap came since the key frame (the CPU decoder then starts at the next one)
#define NX_MCK_PACKETS 4096
#define NX_MCK_BYTES (64u << 20)
typedef struct { struct { uint8_t *d; size_t n; int64_t pts; } *p; int n, cap, lost; size_t bytes; uint64_t given; } nx_mck;
static inline void nx_mck_drop(nx_mck *k) { for (int i = 0; i < k->n; i++) free(k->p[i].d); k->n = 0; k->bytes = 0; }
static inline void nx_mck_clear(nx_mck *k) { nx_mck_drop(k); k->lost = 0; k->given = 0; }
static inline void nx_mck_free(nx_mck *k) { nx_mck_clear(k); free(k->p); k->p = NULL; k->cap = 0; }
static inline void nx_mck_packet(nx_mck *k, const uint8_t *d, size_t n, int64_t pts, int key) {
    if (key) nx_mck_clear(k);
    if (k->lost) return;
    if (k->n >= NX_MCK_PACKETS || k->bytes + n > NX_MCK_BYTES) { k->lost = 1; nx_mck_drop(k); return; }
    if (k->n == k->cap) {
        int c = k->cap ? k->cap * 2 : 64; void *q = realloc(k->p, (size_t)c * sizeof *k->p);
        if (!q) { k->lost = 1; nx_mck_drop(k); return; }
        k->p = q; k->cap = c;
    }
    uint8_t *c = malloc(n ? n : 1); if (!c) { k->lost = 1; nx_mck_drop(k); return; }
    if (n) memcpy(c, d, n);
    k->p[k->n].d = c; k->p[k->n].n = n; k->p[k->n].pts = pts; k->n++; k->bytes += n;
}
static inline void nx_mck_picture(nx_mck *k) { k->given++; }
#ifdef __ANDROID__   // the Vulkan replay's (vka_present.c), when it is linked in: 1 when the swapchain is BGRA
__attribute__((weak)) int vka_swap_rb(void);
#define NX_SWAP_RB() (&vka_swap_rb && vka_swap_rb())
#else   // Mach-O has no undefined weak reference without -U at link time: the Mac host and the unit tests never swap
#define NX_SWAP_RB() 0
#endif

// NV12 (stride w for both planes) into RGBA rows (memory order R G B A), BT.601 or BT.709, limited or full range
static inline uint8_t nx_clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline void nx_nv12_to_rgba(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, uint8_t *dst, int dstride, int bt709, int full) {
    // 16.16 coefficients: R = Y' + rv V', G = Y' - gu U' - gv V', B = Y' + bu U'
    int ys = full ? 65536 : 76309, cs = full ? 65536 : 74606;   // 255/219, 255/224
    int rv = bt709 ? 103206 : 91881, gu = bt709 ? 12276 : 22553, gv = bt709 ? 30679 : 46802, bu = bt709 ? 121609 : 116130;
    rv = (int)((int64_t)rv * cs >> 16); gu = (int)((int64_t)gu * cs >> 16); gv = (int)((int64_t)gv * cs >> 16); bu = (int)((int64_t)bu * cs >> 16);
    int swap_rb = NX_SWAP_RB();
    for (int r = 0; r < h; r++) {
        const uint8_t *yr = y + (size_t)r * stride, *cr = uv + (size_t)(r / 2) * stride; uint8_t *o = dst + (size_t)r * dstride;
        for (int x = 0; x < w; x++) {
            int Y = ((yr[x] - (full ? 0 : 16)) * ys), U = cr[x & ~1] - 128, V = cr[(x & ~1) + 1] - 128;
            uint8_t red = nx_clamp8((Y + rv * V + 32768) >> 16);
            uint8_t green = nx_clamp8((Y - gu * U - gv * V + 32768) >> 16);
            uint8_t blue = nx_clamp8((Y + bu * U + 32768) >> 16);
            o[4 * x] = swap_rb ? blue : red;
            o[4 * x + 1] = green;
            o[4 * x + 2] = swap_rb ? red : blue;
            o[4 * x + 3] = 255;
        }
    }
}
