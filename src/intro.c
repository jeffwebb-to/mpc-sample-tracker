/*
 * Start-up screen, in the style of a VHS tape of a demoscene intro. The tape starts on the
 * VCR's blue screen, rolls into sync, and shows copper bars and a starfield behind a
 * big waving JEFF WEBB with a sine scroller underneath. A tracking glitch swaps the title
 * for the program name. Chroma bleed, scanlines, grain, tracking bands and head-switching
 * noise are applied over the whole picture. The pads show an equaliser and a short chiptune
 * plays on the replayer's spare voice. Any pad or button skips it.
 *
 * All timing is wall-clock, so a slow screen update drops frames instead of stretching the show.
 *
 * MIT licence, see LICENSE.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app.h"
#include "font5x7.h"
#include "hw.h"
#include "intro.h"
#include "player.h"

#define W SCR_W
#define H SCR_H
#define T_BLUE  0.35f             /* VCR blue screen */
#define T_LOCK  0.85f             /* picture has rolled into sync */
#define T_FLASH 2.8f              /* tracking glitch: title -> program name */
#define T_END   4.9f              /* fade out starts */
#define T_FADE  0.35f
#define FRAME_MS 40
#define RATE 44100.0f

#define SCROLL_TEXT "JEFF WEBB PRESENTS " APP_NAME " - A TRACKER FOR THE AKAI MPC SAMPLE - 8 TRACKS - WAV AND MOD FILES - PLAY IT ON THE PADS - PRESS ANY PAD - "

static volatile int running, abort_req;
int intro_active(void) { return running; }
void intro_abort(void) { abort_req = 1; }

static uint32_t rs = 0x2545F491u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

/* -------------------------------------------------------------- colours ---- */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int c8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static uint32_t rgb(int r, int g, int b) { return (uint32_t)c8(r) << 16 | (uint32_t)c8(g) << 8 | (uint32_t)c8(b); }
static int ch_r(uint32_t c) { return (int)(c >> 16 & 255); }
static int ch_g(uint32_t c) { return (int)(c >> 8 & 255); }
static int ch_b(uint32_t c) { return (int)(c & 255); }

static uint32_t mixc(uint32_t a, uint32_t b, float t)
{
    t = clampf(t, 0, 1);
    return rgb((int)(ch_r(a) * (1 - t) + ch_r(b) * t), (int)(ch_g(a) * (1 - t) + ch_g(b) * t), (int)(ch_b(a) * (1 - t) + ch_b(b) * t));
}

static const uint32_t G_FIRE[4] = { 0xFFFFFF, 0xFFF08C, 0xFFB000, 0xE05A00 };      /* title */
static const uint32_t G_ICE[4]  = { 0xFFFFFF, 0xA8EAFF, 0x4CB4FF, 0x2C6CF0 };      /* program name */
static const uint32_t G_SCRL[4] = { 0xFFFFFF, 0xC8F0FF, 0x7AB4FF, 0x2C4CA8 };      /* scroller */
static const uint32_t G_SUB[4]  = { 0xFFFFFF, 0xFFFFFF, 0xD8E4FF, 0xA8B8E8 };

static uint32_t grad(const uint32_t *st, float v)
{
    float f = clampf(v, 0, 1) * 3;
    int i = (int)f; if (i > 2) i = 2;
    return mixc(st[i], st[i + 1], f - (float)i);
}

static inline void plot(int x, int y, uint32_t c) { if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) scr_fb[y * W + x] = c; }

static void rect(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) { uint32_t *p = scr_fb + (y + j) * W + x; for (int i = 0; i < w; i++) p[i] = c; }
}

static float ease_out(float p) { p = clampf(p, 0, 1); return 1 - (1 - p) * (1 - p) * (1 - p); }

/* ------------------------------------------------------------ backdrop ---- */
static void draw_stars(float t)
{
    for (int i = 0; i < 80; i++) {
        float seed = (float)((i * 7919) % 1000) / 1000.0f, d = fmodf(seed + t * 0.28f, 1.0f);
        float sx = (float)((i * 104729) % 2001 - 1000) / 1000.0f, sy = (float)((i * 15485863u) % 2001 - 1000) / 1000.0f;
        float r = d * d * 2.4f;
        int x = W / 2 + (int)(sx * W * 0.5f * r), y = H / 2 + (int)(sy * H * 0.5f * r);
        int v = (int)(60 + 195 * d);
        uint32_t c = rgb(v, v, v);
        plot(x, y, c);
        if (d > 0.82f) { plot(x + 1, y, c); plot(x, y + 1, c); plot(x + 1, y + 1, c); }
    }
}

static void draw_bars(float t)
{
    static const uint32_t col[7] = { 0xFF2814, 0xFF8C00, 0xFFE63C, 0x28DC3C, 0x1EC8E6, 0x2846FF, 0xF0F0FF };
    float ph[7]; int ord[7];
    for (int i = 0; i < 7; i++) { ph[i] = t * 1.6f + (float)i * 0.55f; ord[i] = i; }
    for (int a = 0; a < 7; a++)
        for (int b = a + 1; b < 7; b++)
            if (cosf(ph[ord[b]]) < cosf(ph[ord[a]])) { int x = ord[a]; ord[a] = ord[b]; ord[b] = x; }
    for (int k = 0; k < 7; k++) {
        int i = ord[k], yc = 98 + (int)(sinf(ph[i]) * 46);
        for (int j = -8; j <= 8; j++) {
            float a = 1.0f - fabsf((float)j) / 9.0f, v = a * 0.46f;
            uint32_t c = mixc(rgb((int)(ch_r(col[i]) * v), (int)(ch_g(col[i]) * v), (int)(ch_b(col[i]) * v)), 0xFFFFFF, a * a * a * 0.22f);
            rect(0, yc + j, W, 1, c);
        }
    }
}

/* ----------------------------------------------------------------- text ---- */
/* Text where every pixel column is shifted vertically by a sine of its screen x: the wave
 * travels along the text and the text can scroll underneath it. Pass 0 draws a black outline
 * and shadow, pass 1 the face with a vertical gradient. */
static void wave_text(const char *s, int count, int x0, int y0, int S, float amp, float wf, float ws, float t, const uint32_t *gr)
{
    int n = (int)strlen(s), ol = S >= 4 ? 2 : 1;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n && i < count; i++) {
            int ch = (unsigned char)s[i], cx = x0 + i * 6 * S;
            if (ch <= 32 || ch > 126 || cx < -6 * S - 2 || cx >= W + 2) continue;
            const unsigned char *g = font5x7[ch - 32];
            for (int col = 0; col < 5; col++)
                for (int sx = 0; sx < S; sx++) {
                    int x = cx + col * S + sx;
                    if (x < -2 || x >= W + 2) continue;
                    int dy = (int)(sinf((float)x * wf + t * ws) * amp + sinf((float)x * wf * 2.4f - t * ws * 0.5f) * amp * 0.3f);
                    for (int row = 0; row < 7; row++) {
                        if (!(g[col] >> row & 1)) continue;
                        for (int k = 0; k < S; k++) {
                            int y = y0 + dy + row * S + k;
                            if (pass == 0) {
                                plot(x + ol + 1, y + ol + 1, 0x1A0606);
                                for (int oy = -ol; oy <= ol; oy++) for (int ox = -ol; ox <= ol; ox++) plot(x + ox, y + oy, 0x000000);
                            } else plot(x, y, grad(gr, (float)(row * S + k) / (float)(7 * S - 1)));
                        }
                    }
                }
        }
}

static int text_w(const char *s, int S) { return (int)strlen(s) * 6 * S - S; }

static void flat_text(int x, int y, const char *s, int S, uint32_t c)
{
    for (int i = 0; s[i]; i++, x += 6 * S) {
        int ch = (unsigned char)s[i];
        if (ch < 32 || ch > 126) continue;
        const unsigned char *g = font5x7[ch - 32];
        for (int pass = 0; pass < 2; pass++)
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (g[col] >> row & 1) rect(x + col * S + (pass ? 0 : S / 2 + 1), y + row * S + (pass ? 0 : S / 2 + 1), S, S, pass ? c : 0x000000);
    }
}

static void draw_scene(float t)
{
    if (t < T_BLUE) {                                       /* the VCR's blue screen */
        rect(0, 0, W, H, rgb(8, 16, 178));
        return;
    }
    rect(0, 0, W, H, rgb(0, 0, 8));
    draw_stars(t);
    draw_bars(t);
    if (t < T_FLASH + 0.2f) {
        const char *s = APP_AUTHOR;
        int S = 5, x0 = (W - text_w(s, S)) / 2 + (int)((1 - ease_out((t - 0.7f) / 0.9f)) * (float)W * 1.1f);
        wave_text(s, 99, x0, 62, S, 8.0f * clampf((t - 0.7f) / 0.6f, 0, 1), 0.04f, 4.0f, t, G_FIRE);
        int shown = (int)((t - 2.0f) / 0.05f);
        if (shown > 0) wave_text("PRESENTS", shown, (W - text_w("PRESENTS", 2)) / 2, 124, 2, 2.0f, 0.05f, 4.0f, t, G_SUB);
    } else {
        const char *s = APP_NAME;
        int n = (int)strlen(s), S = n * 30 - 5 <= 300 ? 5 : 4;
        int x0 = (W - text_w(s, S)) / 2 - (int)((1 - ease_out((t - T_FLASH - 0.2f) / 0.6f)) * (float)W * 1.1f);
        wave_text(s, 99, x0, 62, S, 8.0f, 0.04f, 4.0f, t, G_ICE);
        int shown = (int)((t - T_FLASH - 0.8f) / 0.03f);
        const char *sub = "FOR THE AKAI MPC SAMPLE";
        if (shown > 0) wave_text(sub, shown, (W - text_w(sub, 2)) / 2, 124, 2, 2.0f, 0.05f, 4.0f, t, G_SUB);
        if (t > T_FLASH + 1.1f) {
            if (((int)(t * 2.5f)) & 1) { const char *p = "PRESS ANY PAD"; flat_text((W - text_w(p, 2)) / 2, 206, p, 2, 0xFFFFFF); }
            flat_text(8, 226, "(C) 2026 " APP_AUTHOR, 1, rgb(190, 200, 230));
            flat_text(W - 8 - text_w("V" APP_VERSION, 1), 226, "V" APP_VERSION, 1, rgb(190, 200, 230));
        }
    }
    if (t > T_LOCK) {
        float total = (float)strlen(SCROLL_TEXT) * 12.0f + W;
        int x0 = W - (int)fmodf((t - T_LOCK) * 130.0f, total);
        wave_text(SCROLL_TEXT, 999, x0, 168, 2, 9.0f, 0.035f, 5.0f, t, G_SCRL);
    }
}

/* ------------------------------------------------------------------ VHS ---- */
static uint32_t src[W * H];
static inline uint32_t sp(const uint32_t *row, int x) { return row[x < 0 ? 0 : x >= W ? W - 1 : x]; }

static void vhs(float t, int frame)
{
    memcpy(src, scr_fb, sizeof src);
    int roll = 0;
    if (t >= T_BLUE && t < T_LOCK) { float p = 1 - (t - T_BLUE) / (T_LOCK - T_BLUE); roll = (int)(p * p * (float)H * 1.7f); }
    int bandy = -100, bandh = 0, bandx = 0;
    if (t > T_LOCK && fmodf(t, 1.3f) < 0.14f) { bandh = 16; bandy = (int)fmodf(t * 170.0f, 190.0f) + 20; bandx = 7 + (int)(rnd() % 9); }
    int glitch = t >= T_FLASH && t < T_FLASH + 0.35f;
    for (int y = 0; y < H; y++) {
        const uint32_t *row = src + ((y + roll) % H) * W;
        uint32_t *out = scr_fb + y * W;
        int dx = (int)(sinf((float)y * 0.07f + t * 9.0f) * 1.2f);
        if ((rnd() & 63) == 0) dx += (int)(rnd() & 3) - 1;
        if (y >= bandy && y < bandy + bandh) dx += bandx + (int)(rnd() % 5) - 2;
        if (glitch && (rnd() & 7) < 3) dx += (int)(rnd() % 49) - 24;
        if (y >= H - 8) dx += (int)(rnd() % 17) - 8;
        int bleed = glitch ? 5 : 1, inv = glitch && ((y / 5 + frame) % 4) == 0, head = y >= H - 6;
        for (int x = 0; x < W; x++) {
            int sx = x - dx;
            int r = (int)(sp(row, sx + bleed) >> 16 & 255), g = (int)((sp(row, sx) >> 8 & 255) + (sp(row, sx - 1) >> 8 & 255)) >> 1, b = (int)(sp(row, sx - bleed) & 255);
            int n = (int)(rnd() & 15) - 8;
            r += n; g += n; b += n;
            if (inv) { r = 255 - r; g = 255 - g; b = 255 - b; }
            if (head) { int v = (int)(rnd() & 255); r = (r + v * 3) >> 2; g = (g + v * 3) >> 2; b = (b + v * 3) >> 2; }
            out[x] = rgb(r, g, b);
        }
    }
}

static void overlay(uint32_t col, float a)
{
    int k = (int)(clampf(a, 0, 1) * 256), r = ch_r(col) * k, g = ch_g(col) * k, b = ch_b(col) * k;
    if (k == 0) return;
    for (int i = 0; i < W * H; i++) {
        uint32_t p = scr_fb[i];
        scr_fb[i] = rgb((ch_r(p) * (256 - k) + r) >> 8, (ch_g(p) * (256 - k) + g) >> 8, (ch_b(p) * (256 - k) + b) >> 8);
    }
}

static void scanlines(void)
{
    for (int y = 1; y < H; y += 2) {
        uint32_t *row = scr_fb + y * W;
        for (int x = 0; x < W; x++) row[x] -= (row[x] >> 2) & 0x3F3F3F;
    }
}

static void pads(float t)
{
    for (int i = 0; i < 16; i++) {
        int col = i % 4, row = i / 4, r = 0, g = 0, b = 0;
        if (t >= T_FLASH && t < T_FLASH + 0.2f) r = g = b = 127;
        else if (t > 0.5f) {
            float lvl = 2.2f + 1.8f * sinf(t * 6.0f + (float)col * 1.9f) * sinf(t * 2.3f + (float)col);
            if ((float)row + 0.5f < lvl) { if (row < 2) g = 110; else if (row == 2) { r = 120; g = 90; } else r = 127; }
            else b = 2;
        }
        surf_pad_rgb(i, r, g, b);
    }
}

/* -------------------------------------------------------------- stinger ---- */
static void make_stinger(int16_t **out, uint32_t *frames)
{
    const float TAU = 6.2831853f;
    uint32_t n = (uint32_t)((T_END + T_FADE) * RATE);
    float *b = calloc(n, sizeof *b);
    uint32_t r2 = 0xC0FFEEu;
#define NOISE() (r2 = r2 * 1664525u + 1013904223u, (float)(int32_t)r2 / 2147483648.0f)
    /* tape hiss and the clunk of the tape starting */
    float lp = 0;
    for (uint32_t i = 0; i < (uint32_t)(T_END * RATE); i++) { float nz = NOISE(); lp += (nz - lp) * 0.2f; b[i] += 0.02f * (nz - lp); }
    for (uint32_t i = 0; i < (uint32_t)(0.2f * RATE); i++) { float t = (float)i / RATE; b[i] += 0.5f * sinf(TAU * 70.0f * t) * expf(-t * 25.0f) + (i < 300 ? 0.3f * NOISE() * (1 - (float)i / 300.0f) : 0); }
    /* a chiptune: pulse arpeggio, kick, bass and hats over four chords, 150 bpm */
    static const int chord[4][4] = { { 0, 3, 7, 12 }, { -4, 0, 3, 8 }, { 3, 7, 10, 15 }, { -2, 2, 5, 10 } }, root[4] = { 0, -4, 3, -2 }, ping[6] = { 0, 1, 2, 3, 2, 1 };
    float prev = 0;
    for (int st = 0; st < 22; st++) {
        uint32_t s0 = (uint32_t)((0.6f + (float)st * 0.1f) * RATE);
        int ci = (st / 4) % 4;
        float f = 440.0f * powf(2.0f, (float)chord[ci][ping[st % 6]] / 12.0f);
        for (uint32_t i = 0; i < (uint32_t)(0.1f * RATE) && s0 + i < n; i++) {
            float t = (float)i / RATE, ph = f * t;
            b[s0 + i] += 0.16f * ((ph - floorf(ph)) < 0.25f ? 1.0f : -1.0f) * expf(-t * 12.0f);
        }
        if (st % 4 == 0) {
            for (uint32_t i = 0; i < (uint32_t)(0.3f * RATE) && s0 + i < n; i++) {
                float t = (float)i / RATE, ph = 45.0f * t + 105.0f * (1 - expf(-9.0f * t)) / 9.0f;
                b[s0 + i] += 0.55f * sinf(TAU * ph) * expf(-t * 8.0f);
            }
            float f2 = 55.0f * powf(2.0f, (float)root[ci] / 12.0f);
            for (uint32_t i = 0; i < (uint32_t)(0.4f * RATE) && s0 + i < n; i++) {
                float t = (float)i / RATE, ph = f2 * t;
                b[s0 + i] += 0.14f * ((ph - floorf(ph)) < 0.5f ? 1.0f : -1.0f) * (1 - t / 0.4f);
            }
        }
        if (st % 2 == 1)
            for (uint32_t i = 0; i < (uint32_t)(0.04f * RATE) && s0 + i < n; i++) { float t = (float)i / RATE, nz = NOISE(); b[s0 + i] += 0.12f * (nz - prev) * expf(-t * 70.0f); prev = nz; }
    }
    /* the tracking glitch: sample-and-hold noise and a falling square, then a chord */
    uint32_t g0 = (uint32_t)(T_FLASH * RATE);
    float held = 0, ph = 0;
    for (uint32_t i = 0; i < (uint32_t)(0.4f * RATE) && g0 + i < n; i++) {
        float t = (float)i / RATE;
        if (i % 10 == 0) held = NOISE();
        ph += (900.0f * expf(-t * 7.0f) + 50.0f) / RATE;
        b[g0 + i] += 0.3f * held * expf(-t * 5.0f) + 0.2f * ((ph - floorf(ph)) < 0.5f ? 1.0f : -1.0f) * expf(-t * 4.0f);
    }
    static const int notes[5] = { 0, 3, 7, 12, 14 };
    float nf[5];
    for (int k = 0; k < 5; k++) nf[k] = 220.0f * powf(2.0f, (float)notes[k] / 12.0f);
    for (uint32_t i = 0; g0 + (uint32_t)(0.3f * RATE) + i < n; i++) {
        float t = (float)i / RATE, env = (t < 0.004f ? t / 0.004f : 1.0f) * expf(-t * 0.9f), v = 0;
        for (int k = 0; k < 5; k++) { float p = nf[k] * t; v += (p - floorf(p)) < (k & 1 ? 0.25f : 0.5f) ? 1.0f : -1.0f; }
        b[g0 + (uint32_t)(0.3f * RATE) + i] += 0.07f * v * env + 0.3f * sinf(TAU * 110.0f * t) * expf(-t * 1.4f);
    }
    float peak = 0;
    for (uint32_t i = 0; i < n; i++) if (fabsf(b[i]) > peak) peak = fabsf(b[i]);
    float gain = peak > 0 ? 0.85f / peak : 1;
    int16_t *o = malloc(sizeof(int16_t) * ((size_t)n + 1));
    uint32_t tail = (uint32_t)(T_FADE * RATE);
    for (uint32_t i = 0; i < n; i++) o[i] = (int16_t)(b[i] * gain * (i + tail > n ? (float)(n - i) / (float)tail : 1.0f) * 32000.0f);
    o[n] = 0;
    free(b);
    *out = o; *frames = n;
#undef NOISE
}

/* ------------------------------------------------------------------ run ---- */
void intro_run(void)
{
    running = 1; abort_req = 0;
    int16_t *snd = NULL; uint32_t n = 0;
    make_stinger(&snd, &n);
    pl_stinger(snd, n, RATE);
    uint32_t start = hw_ms(), led_ms = 0;
    float fade_start = -1;
    int stopped = 0, frame = 0;
    for (;;) {
        uint32_t now = hw_ms();
        float t = (float)(now - start) / 1000.0f;
        if (abort_req && fade_start < 0) fade_start = t;
        if (t >= T_END && fade_start < 0) fade_start = t;
        if (abort_req && !stopped) { pl_stinger_stop(); stopped = 1; }
        float fo = 0;
        if (fade_start >= 0) { fo = (t - fade_start) / T_FADE; if (fo >= 1) break; }
        draw_scene(t);
        vhs(t, frame++);
        scanlines();
        if (t >= T_FLASH && t < T_FLASH + 0.18f) overlay(0xFFFFFF, 0.6f * (1 - (t - T_FLASH) / 0.18f));
        overlay(0, fo);
        scr_present();
        if (now - led_ms >= 90) { led_ms = now; pads(t); }
        uint32_t spent = hw_ms() - now;
        if (spent < FRAME_MS) usleep((FRAME_MS - spent) * 1000);
    }
    for (int i = 0; i < 16; i++) surf_pad_rgb(i, 0, 0, 0);
    if (!stopped) pl_stinger_stop();
    usleep(60000);
    free(snd);
    running = 0;
}
