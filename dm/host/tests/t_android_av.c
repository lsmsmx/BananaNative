// The Android host's pure audio / picture helpers (android/av_android.h), on the Mac: the AAudio fallback resampler, AAC channel mapping, the SPS size MediaCodec is configured
// with, and MediaCodec output layouts into NV12 / RGBA
#include "android/av_android.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static size_t unhex(const char *h, uint8_t *o) { size_t n = 0; while (h[0] && h[1]) { unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; h += 2; } return n; }

static void t_resample(void) {
    // equal rates: the output is the input two frames late, exactly
    nx_rs r; rs_init(&r, 48000, 48000, 2);
    int16_t src[2 * 300], dst[2 * 300]; for (int i = 0; i < 600; i++) src[i] = (int16_t)(i * 7 - 1000);
    uint32_t need = rs_need(&r, 100); CHECK(need == 100, "48→48 needs %u for 100", need);
    rs_run(&r, src, need, dst, 100);
    CHECK(dst[0] == 0 && dst[3] == 0, "primed with silence");
    int ok = 1; for (int j = 2; j < 100; j++) ok &= dst[2 * j] == src[2 * (j - 2)] && dst[2 * j + 1] == src[2 * (j - 2) + 1];
    CHECK(ok, "48→48 is a two-frame delay");
    need = rs_need(&r, 50); rs_run(&r, src + 200, need, dst, 50);
    CHECK(dst[0] == src[196] && dst[2] == src[198] && dst[4] == src[200], "the delay carries across calls (%d %d %d)", dst[0], dst[2], dst[4]);
    // 48 → 44.1 kHz over many calls of odd sizes: exactly as many source frames consumed as the ratio says, and a ramp stays a ramp
    rs_init(&r, 48000, 44100, 1);
    int16_t ramp[48000], out[44100]; for (int i = 0; i < 48000; i++) ramp[i] = (int16_t)(i / 2);
    uint32_t used = 0, made = 0, sizes[] = {1, 7, 96, 441, 1000, 3};
    for (int k = 0; made < 44000; k++) { uint32_t n = sizes[k % 6]; if (made + n > 44000) n = 44000 - made; uint32_t s = rs_need(&r, n); rs_run(&r, ramp + used, s, out + made, n); used += s; made += n; }
    double want = 44000.0 * 48000 / 44100;
    CHECK(used >= want - 2 && used <= want + 2, "48→44.1: %u source frames for 44000, want about %.1f", used, want);
    int mono = 1; for (uint32_t i = 3; i < made; i++) mono &= out[i] >= out[i - 1];
    CHECK(mono, "48→44.1 keeps a rising ramp rising");
    // 48 → 96 kHz: every other output is a midpoint
    rs_init(&r, 48000, 96000, 1);
    int16_t s2[8] = {0, 100, 200, 300, 400, 500, 600, 700}, o2[12];
    need = rs_need(&r, 12); CHECK(need == 6, "48→96 needs %u for 12", need);
    rs_run(&r, s2, need, o2, 12);
    CHECK(o2[4] == 0 && o2[5] == 50 && o2[6] == 100 && o2[7] == 150, "48→96 interpolates: %d %d %d %d", o2[4], o2[5], o2[6], o2[7]);
}
static void t_chmap(void) {
    int16_t m[4] = {1, 2, 3, 4}, s[8];
    ch_map(m, 1, s, 2, 4); CHECK(s[0] == 1 && s[1] == 1 && s[6] == 4 && s[7] == 4, "mono → stereo");
    int16_t st[6] = {10, 20, -10, -30, 7, 9}, mo[3];
    ch_map(st, 2, mo, 1, 3); CHECK(mo[0] == 15 && mo[1] == -20 && mo[2] == 8, "stereo → mono: %d %d %d", mo[0], mo[1], mo[2]);
    int16_t buf[12] = {1, 2, 3, 4, 5, 6};   // in place, growing: mono 6 frames would need 12; stereo 3 frames → 4 channels
    ch_map(buf, 2, buf, 4, 3);
    CHECK(buf[0] == 1 && buf[1] == 2 && buf[2] == 0 && buf[3] == 0 && buf[4] == 3 && buf[5] == 4 && buf[8] == 5 && buf[9] == 6 && buf[11] == 0, "2 → 4 in place");
    int16_t g[8] = {5, 6, 7, 0, 0, 0, 0, 0};
    ch_map(g, 1, g, 2, 3); CHECK(g[0] == 5 && g[1] == 5 && g[2] == 6 && g[3] == 6 && g[4] == 7 && g[5] == 7, "1 → 2 in place");
    int16_t six[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    ch_map(six, 6, six, 2, 2); CHECK(six[0] == 1 && six[1] == 2 && six[2] == 7 && six[3] == 8, "6 → 2 keeps the front pair");
}
static void t_sps(void) {
    // from libx264 (ffmpeg testsrc): High 1080p (cropped from 1088), Main 720p, Baseline with odd crop, High interlaced (frame_mbs_only 0)
    static const struct { const char *hex; int w, h; } k[] = {
        {"67640028acd940780227e5c044000003000400000300f03c60c658", 1920, 1080},
        {"674d401feca02802dd8088000003000800000301e078c18cb0", 1280, 720},
        {"6742c00dd90150878888c044000003000400000300f03c50a920", 322, 242},
        {"6764001eacd940b47b6022000003000200000300783e28532c", 720, 480},
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        uint8_t b[64]; size_t n = unhex(k[i].hex, b); int w = 0, h = 0;
        int ok = avc_sps_size(b, n, &w, &h);
        CHECK(ok && w == k[i].w && h == k[i].h, "SPS %zu: %d → %d×%d, want %d×%d", i, ok, w, h, k[i].w, k[i].h);
    }
    uint8_t pps[4] = {0x68, 0xee, 0x3c, 0x80}; int w, h;
    CHECK(!avc_sps_size(pps, 4, &w, &h), "a PPS is not an SPS");
    CHECK(!avc_sps_size((const uint8_t *)"\x67\x64", 2, &w, &h), "a truncated SPS");
}
static void t_yuv(void) {
    CHECK(nx_yuv_kind(21) == NXYUV_NV12 && nx_yuv_kind(19) == NXYUV_I420 && nx_yuv_kind(0x7fa30c04) == NXYUV_NV12 && nx_yuv_kind(0x7f000789) == NXYUV_NONE, "colour formats");
    // a 6×4 picture cropped at (2, 2) from an 8-stride, 6-row buffer: Y = row * 10 + col, U = 100 + ..., V = 200 + ...
    enum { S = 8, SL = 6 };
    uint8_t nv12[S * SL + S * SL / 2], i420[S * SL + 2 * (S / 2) * (SL / 2)], out[6 * 4 * 3 / 2];
    for (int r = 0; r < SL; r++) for (int c = 0; c < S; c++) nv12[r * S + c] = i420[r * S + c] = (uint8_t)(r * 10 + c);
    for (int r = 0; r < SL / 2; r++) for (int c = 0; c < S / 2; c++) {
        nv12[S * SL + r * S + 2 * c] = (uint8_t)(100 + r * 10 + c); nv12[S * SL + r * S + 2 * c + 1] = (uint8_t)(200 + r * 10 + c);
        i420[S * SL + r * (S / 2) + c] = (uint8_t)(100 + r * 10 + c); i420[S * SL + (S / 2) * (SL / 2) + r * (S / 2) + c] = (uint8_t)(200 + r * 10 + c);
    }
    nx_yuv_layout L = {NXYUV_NV12, S, SL, 2, 2, 6, 4};
    CHECK(!nx_yuv_to_nv12(nv12, sizeof nv12, &L, out), "NV12 copy");
    CHECK(out[0] == 22 && out[5] == 27 && out[6] == 32 && out[23] == 57, "NV12 luma crop: %d %d %d %d", out[0], out[5], out[6], out[23]);
    CHECK(out[24] == 111 && out[25] == 211 && out[28] == 113 && out[30] == 121 && out[31] == 221, "NV12 chroma crop: %d %d %d %d %d", out[24], out[25], out[28], out[30], out[31]);
    uint8_t out2[sizeof out]; L.kind = NXYUV_I420;
    CHECK(!nx_yuv_to_nv12(i420, sizeof i420, &L, out2) && !memcmp(out, out2, sizeof out), "I420 gives the same NV12");
    L.kind = NXYUV_NV12;
    CHECK(nx_yuv_to_nv12(nv12, sizeof nv12 - 5, &L, out) == -1, "a short buffer is refused");
    // odd crop width 5: padded to 6 by repeating the last column
    nx_yuv_layout O = {NXYUV_NV12, S, SL, 0, 0, 5, 4}; uint8_t o3[6 * 4 * 3 / 2];
    CHECK(!nx_yuv_to_nv12(nv12, sizeof nv12, &O, o3) && o3[4] == 4 && o3[5] == 4, "odd width padded: %d %d", o3[4], o3[5]);
    // Qualcomm's AV1 first picture: a 4x4 picture whose luma is padded to a slice height of 6 (two zero rows), then only the 2 chroma rows the picture has (4 * 6 + 4 * 2 bytes,
    // not 4 * 6 + 4 * 3): taken at slice height 6, its chroma read after the padding (at 4 rows the chroma would come from the zero padding: green)
    {   uint8_t q[4 * 6 + 4 * 2], qo[4 * 4 * 3 / 2]; memset(q, 0, sizeof q);
        for (int i = 0; i < 16; i++) q[i] = 80;
        for (int i = 24; i < 32; i++) q[i] = (uint8_t)(120 + i - 24);
        nx_yuv_layout Q = {NXYUV_NV12, 4, 6, 0, 0, 4, 4};
        CHECK(nx_yuv_bytes(&Q) == sizeof q, "the bytes the crop reads: %zu", nx_yuv_bytes(&Q));
        CHECK(!nx_yuv_to_nv12(q, sizeof q, &Q, qo) && qo[0] == 80 && qo[16] == 120 && qo[23] == 127, "chroma after the padding: %d %d", qo[16], qo[23]);
        CHECK(nx_yuv_to_nv12(q, sizeof q - 1, &Q, qo) == -1, "one byte short is refused");
    }
}
static void t_rgba(void) {
    // limited-range white, black, and BT.601 / BT.709 red
    uint8_t y[4] = {235, 16, 81, 63}, uv[4] = {128, 128, 90, 240};   // pixels 0-1 share grey chroma, 2-3 share red-ish chroma
    uint8_t o[16];
    nx_nv12_to_rgba(y, uv, 4, 1, 4, o, 16, 0, 0);
    CHECK(o[0] == 255 && o[1] == 255 && o[2] == 255 && o[3] == 255, "white: %d %d %d", o[0], o[1], o[2]);
    CHECK(o[4] == 0 && o[5] == 0 && o[6] == 0, "black: %d %d %d", o[4], o[5], o[6]);
    CHECK(o[8] >= 250 && o[9] <= 4 && o[10] <= 4, "BT.601 red (81, 90, 240): %d %d %d", o[8], o[9], o[10]);
    uint8_t y7[2] = {63, 63}, uv7[2] = {102, 240};   // BT.709 red
    nx_nv12_to_rgba(y7, uv7, 2, 1, 2, o, 8, 1, 0);
    CHECK(o[0] >= 250 && o[1] <= 4 && o[2] <= 4, "BT.709 red: %d %d %d", o[0], o[1], o[2]);
    uint8_t fy[2] = {255, 0}, fuv[2] = {128, 128};
    nx_nv12_to_rgba(fy, fuv, 2, 1, 2, o, 8, 1, 1);
    CHECK(o[0] == 255 && o[4] == 0, "full range: %d %d", o[0], o[4]);
}
int main(void) {
    t_resample(); t_chmap(); t_sps(); t_yuv(); t_rgba();
    if (fails) { printf("t_android_av: %d failures\n", fails); return 1; }
    printf("t_android_av: ok\n");
    return 0;
}
