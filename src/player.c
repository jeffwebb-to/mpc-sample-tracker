/*
 * Replayer and mixer.
 *
 * Timing follows the MOD convention: a row lasts `speed` ticks and a tick lasts 2.5 / bpm
 * seconds. Pitch is kept as an Amiga-style period (428 at C-4) so that the classic slide
 * effects behave as they do in a MOD; the playback rate is ins->rate * 428 / period.
 * Effects use the MOD numbering 0..F.
 *
 * MIT licence, see LICENSE.
 */
#include <math.h>
#include <pthread.h>
#include <string.h>
#include "player.h"

typedef struct {
    const Ins *ins;
    int    insno;
    int    active, fading;
    double pos, step;
    float  period, target;
    int    note;
    int    vol, pan;                 /* 0..64, 0..255 */
    float  lg, rg;                   /* current (ramped) gains */
    int    arp, vib_pos, trem_pos, trem_delta;
    float  vib_delta;
    uint8_t m_porta, m_tporta, m_vib, m_trem, m_slide, m_offset;
    Cell   delayed; int has_delayed;
    int    loop_row, loop_count;
    float  vu;
} Chan;

static Song *S;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Chan ch[NTRK], jam[NJAM], sting;
static Ins sting_ins;
static int mode, ord, pat, row, tick, speed, bpm, loop_pattern, pt;     /* pt: ProTracker behaviour, from the song */
static int jump_ord, break_row, pat_delay, loop_jump_row;
static double tick_left;                         /* frames until the next tick */
static double tick_len;
static int skip_pat = -1, skip_row, skip_trk;
static volatile float master = 0.5f;
static volatile PlayPos pos_out;
static volatile float vu_out[NTRK];

static const int8_t sine64[64] = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126,
    127, 126, 125, 122, 117, 112, 106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
    0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112, -117, -122, -125, -126,
    -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12
};

static uint32_t rnd_state = 0x9E3779B9u;
static uint32_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state >> 8; }

void pl_lock(void) { pthread_mutex_lock(&lock); }
void pl_unlock(void) { pthread_mutex_unlock(&lock); }
void pl_set_master(float g) { master = g; }

PlayPos pl_pos(void) { return pos_out; }

float pl_vu(int t)
{
    float v = vu_out[t];
    vu_out[t] = 0;
    return v;
}

static void set_tempo(int b)
{
    if (b < 32) b = 32;
    bpm = b;
    tick_len = MIX_RATE * 2.5 / bpm;
}

void pl_init(Song *s)
{
    S = s;
    memset(ch, 0, sizeof ch); memset(jam, 0, sizeof jam);
    mode = PL_STOPPED; speed = s->speed; set_tempo(s->bpm); pt = s->pt_mode;
}

static float clamp_period(float p) { return p < 14 ? 14 : p > 6848 ? 6848 : p; }

/* ProTracker's period table, C-1 to B-3 (notes 37..72 here) */
static const int pt_table[36] = { 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113 };
static float period_of(int note) { return (pt && note >= 37 && note <= 72) ? (float)pt_table[note - 37] : (float)note_period(note); }
/* slides stop at B-3 and C-1 in ProTracker */
static float slide_clamp(float p) { return pt ? (p < 113 ? 113 : p > 856 ? 856 : p) : clamp_period(p); }

static void chan_cut(Chan *c) { if (c->active) c->fading = 1; }

static void chan_trigger(Chan *c, int note, uint32_t offset)
{
    const Ins *in = &S->ins[c->insno];
    if (c->insno < 0 || c->insno > NINS || !in->data) { chan_cut(c); return; }   /* 0 is the browser's preview slot */
    c->ins = in; c->note = note;
    c->period = c->target = period_of(note);
    c->pos = offset < in->frames ? offset : in->frames - 1;
    c->active = 1; c->fading = 0;
    c->vib_pos = 0; c->trem_pos = 0; c->vib_delta = 0; c->trem_delta = 0; c->arp = 0;
    c->lg = c->rg = -1;                         /* set by the mixer on the first frame: no ramp from the old note */
}

static void chan_update_step(Chan *c)
{
    if (!c->active || !c->ins) return;
    float p = clamp_period(c->period + c->vib_delta);
    double f = (double)c->ins->rate * 428.0 / p;
    if (c->arp) f *= pow(2.0, c->arp / 12.0);
    c->step = f / MIX_RATE;
}

void pl_all_off(void)
{
    for (int t = 0; t < NTRK; t++) { ch[t].active = 0; ch[t].ins = NULL; }
    for (int j = 0; j < NJAM; j++) { jam[j].active = 0; jam[j].ins = NULL; }
}

void pl_ins_changed(int n)
{
    for (int t = 0; t < NTRK; t++) if (ch[t].insno == n) { ch[t].active = 0; ch[t].ins = NULL; }
    for (int j = 0; j < NJAM; j++) if (jam[j].insno == n) { jam[j].active = 0; jam[j].ins = NULL; }
}

/* ------------------------------------------------------------ sequencer ---- */
static void do_note(Chan *c, const Cell *cl, int t)
{
    int porta = (cl->fx == 3 || cl->fx == 5) && c->active && !c->fading;
    if (cl->ins >= 1 && cl->ins <= NINS) {
        c->insno = cl->ins;
        if (S->ins[cl->ins].data) c->vol = S->ins[cl->ins].vol;
    }
    if (cl->note >= NOTE_MIN && cl->note <= NOTE_MAX) {
        if (porta) c->target = period_of(cl->note);
        else {
            uint32_t off = 0;
            if (cl->fx == 9) { if (cl->prm) c->m_offset = cl->prm; off = (uint32_t)c->m_offset << 8; }
            chan_trigger(c, cl->note, off);
        }
    } else if (cl->note == NOTE_CUT) chan_cut(c);
    if (cl->vol) c->vol = cl->vol - 1;
    (void)t;
}

static void row_effects(Chan *c, const Cell *cl)
{
    int x = cl->prm >> 4, y = cl->prm & 15;
    switch (cl->fx) {
    case 0x8: c->pan = cl->prm; break;
    case 0xB: jump_ord = cl->prm; break;
    case 0xC: c->vol = cl->prm > 64 ? 64 : cl->prm; break;
    case 0xD: break_row = x * 10 + y; break;
    case 0xF: if (cl->prm) { if (cl->prm < 32) speed = cl->prm; else set_tempo(cl->prm); } break;
    case 0xE:
        switch (x) {
        case 0x1: c->period = slide_clamp(c->period - (float)y); break;
        case 0x2: c->period = slide_clamp(c->period + (float)y); break;
        case 0x6:
            if (y == 0) c->loop_row = row;
            else if (c->loop_count == 0) { c->loop_count = y; loop_jump_row = c->loop_row; }
            else if (--c->loop_count > 0) loop_jump_row = c->loop_row;
            break;
        case 0xA: c->vol += y; if (c->vol > 64) c->vol = 64; break;
        case 0xB: c->vol -= y; if (c->vol < 0) c->vol = 0; break;
        case 0xE: pat_delay = y; break;
        }
        break;
    }
}

static void vol_slide(Chan *c, int prm)
{
    if (!pt) { if (prm) c->m_slide = (uint8_t)prm; prm = c->m_slide; }      /* ProTracker has no memory here: Axy with xy = 0 does nothing */
    int x = prm >> 4, y = prm & 15;
    if (x) c->vol += x; else c->vol -= y;
    if (c->vol > 64) c->vol = 64; else if (c->vol < 0) c->vol = 0;
}

static void tone_porta(Chan *c, int prm)
{
    if (prm) c->m_tporta = (uint8_t)prm;
    if (c->period < c->target) { c->period += c->m_tporta; if (c->period > c->target) c->period = c->target; }
    else if (c->period > c->target) { c->period -= c->m_tporta; if (c->period < c->target) c->period = c->target; }
}

static void vibrato(Chan *c, int prm)
{
    if (prm & 0xF0) c->m_vib = (uint8_t)((c->m_vib & 0x0F) | (prm & 0xF0));
    if (prm & 0x0F) c->m_vib = (uint8_t)((c->m_vib & 0xF0) | (prm & 0x0F));
    c->vib_delta = (float)(sine64[c->vib_pos & 63] * (c->m_vib & 15)) / 64.0f;
    c->vib_pos += c->m_vib >> 4;
}

static void tick_effects(Chan *c, const Cell *cl, int t)
{
    int x = cl->prm >> 4, y = cl->prm & 15;
    c->arp = 0;
    switch (cl->fx) {
    case 0x0: if (cl->prm) { int k = t % 3; c->arp = k == 1 ? x : k == 2 ? y : 0; } break;
    case 0x1: if (!pt && cl->prm) c->m_porta = cl->prm; c->period = slide_clamp(c->period - (pt ? cl->prm : c->m_porta)); break;
    case 0x2: if (!pt && cl->prm) c->m_porta = cl->prm; c->period = slide_clamp(c->period + (pt ? cl->prm : c->m_porta)); break;
    case 0x3: tone_porta(c, cl->prm); break;
    case 0x4: vibrato(c, cl->prm); break;
    case 0x5: tone_porta(c, 0); vol_slide(c, cl->prm); break;
    case 0x6: vibrato(c, 0); vol_slide(c, cl->prm); break;
    case 0x7:
        if (cl->prm & 0xF0) c->m_trem = (uint8_t)((c->m_trem & 0x0F) | (cl->prm & 0xF0));
        if (cl->prm & 0x0F) c->m_trem = (uint8_t)((c->m_trem & 0xF0) | (cl->prm & 0x0F));
        c->trem_delta = sine64[c->trem_pos & 63] * (c->m_trem & 15) / 32;
        c->trem_pos += c->m_trem >> 4;
        break;
    case 0xA: vol_slide(c, cl->prm); break;
    case 0xE:
        if (x == 0x9 && y && t % y == 0 && c->note) chan_trigger(c, c->note, 0);
        else if (x == 0xC && t == y) c->vol = 0;
        else if (x == 0xD && t == y && c->has_delayed) { do_note(c, &c->delayed, 0); c->has_delayed = 0; }
        break;
    }
}

static void next_row(void)
{
    const Pattern *pt = &S->pat[pat];
    if (loop_jump_row >= 0) { row = loop_jump_row; loop_jump_row = -1; return; }
    int song = mode == PL_SONG && !loop_pattern;
    if (jump_ord >= 0 || break_row >= 0) {
        int r = break_row >= 0 ? break_row : 0;
        if (song) {
            ord = jump_ord >= 0 ? jump_ord : ord + 1;
            if (ord >= S->nord) ord = 0;
            pat = S->order[ord];
        }
        row = r < S->pat[pat].rows ? r : 0;
        jump_ord = break_row = -1;
        for (int t = 0; t < NTRK; t++) { ch[t].loop_row = 0; ch[t].loop_count = 0; }
        return;
    }
    if (++row >= pt->rows) {
        row = 0;
        if (song) { if (++ord >= S->nord) ord = 0; pat = S->order[ord]; }
        for (int t = 0; t < NTRK; t++) { ch[t].loop_row = 0; ch[t].loop_count = 0; }
    }
}

static void do_tick(void)
{
    if (tick == 0) {
        for (int t = 0; t < NTRK; t++) {
            const Cell *cl = &S->pat[pat].c[row][t];
            Chan *c = &ch[t];
            c->vib_delta = 0; c->trem_delta = 0; c->arp = 0; c->has_delayed = 0;
            if (skip_pat == pat && skip_row == row && skip_trk == t) { skip_pat = -1; continue; }
            if (cl->fx == 0xE && (cl->prm >> 4) == 0x8 && (int)(rnd() & 15) >= (cl->prm & 15)) continue;      /* chance x/16 */
            if (cl->fx == 0xE && (cl->prm >> 4) == 0xD && (cl->prm & 15) > 0) { c->delayed = *cl; c->has_delayed = 1; }
            else do_note(c, cl, t);
            row_effects(c, cl);
        }
    } else {
        for (int t = 0; t < NTRK; t++) tick_effects(&ch[t], &S->pat[pat].c[row][t], tick);
    }
    for (int t = 0; t < NTRK; t++) chan_update_step(&ch[t]);
    if (++tick >= speed * (1 + pat_delay)) {
        tick = 0; pat_delay = 0;
        next_row();
    } else if (pat_delay && tick % speed == 0) {
        /* pattern delay: the row's tick effects simply keep running */
    }
}

/* ---------------------------------------------------------------- mixer ---- */
static void mix_chan(Chan *c, float *out, int frames, int muted, float *vu)
{
    if (!c->active || !c->ins || !c->ins->data) return;
    const Ins *in = c->ins;
    const int16_t *d = in->data;
    int v = c->vol + c->trem_delta; if (v < 0) v = 0; else if (v > 64) v = 64;
    float g = (muted || c->fading) ? 0 : (float)v / 64.0f / 32768.0f;
    float p = (float)c->pan / 255.0f;
    float tl = g * (p < 0.5f ? 1.0f : 2.0f * (1.0f - p)), tr = g * (p > 0.5f ? 1.0f : 2.0f * p);
    if (c->lg < 0) { c->lg = tl; c->rg = tr; }
    double pos = c->pos, step = c->step;
    double end = in->loop_len ? (double)(in->loop_start + in->loop_len) : (double)in->frames;
    float peak = 0;
    for (int i = 0; i < frames; i++) {
        if (pos >= end) {
            if (in->loop_len) { pos -= in->loop_len; if (pos >= end) pos = in->loop_start; }
            else { c->active = 0; break; }
        }
        uint32_t i0 = (uint32_t)pos, i1 = i0 + 1;
        if (in->loop_len && i1 >= in->loop_start + in->loop_len) i1 = in->loop_start;
        float fr = (float)(pos - i0);
        float l, r;
        if (in->ch == 2) {
            l = d[2 * i0] + (d[2 * i1] - d[2 * i0]) * fr;
            r = d[2 * i0 + 1] + (d[2 * i1 + 1] - d[2 * i0 + 1]) * fr;
        } else l = r = d[i0] + (d[i1] - d[i0]) * fr;
        c->lg += (tl - c->lg) * 0.02f; c->rg += (tr - c->rg) * 0.02f;
        l *= c->lg; r *= c->rg;
        out[2 * i] += l; out[2 * i + 1] += r;
        float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if (a > peak) peak = a;
        pos += step;
    }
    c->pos = pos;
    if (c->fading && c->lg < 1e-7f && c->rg < 1e-7f) { c->active = 0; c->fading = 0; }
    if (vu && peak > *vu) *vu = peak;
}

void pl_render(float *out, int frames)
{
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (mode != PL_STOPPED) {
            if (tick_left <= 0) { do_tick(); tick_left += tick_len; }
            if (n > (int)ceil(tick_left)) n = (int)ceil(tick_left);
            if (n < 1) n = 1;
        }
        for (int t = 0; t < NTRK; t++) {
            float vu = 0;
            mix_chan(&ch[t], out + 2 * done, n, S->mute[t], &vu);
            if (vu > vu_out[t]) vu_out[t] = vu;
        }
        for (int j = 0; j < NJAM; j++) mix_chan(&jam[j], out + 2 * done, n, 0, NULL);
        mix_chan(&sting, out + 2 * done, n, 0, NULL);
        if (mode != PL_STOPPED) tick_left -= n;
        done += n;
    }
    float m = master;
    for (int i = 0; i < 2 * frames; i++) out[i] = tanhf(out[i] * m);
    PlayPos pp = { mode, ord, pat, row, 0, speed, bpm };
    if (mode != PL_STOPPED && speed > 0) {
        double f = (tick + 1.0 - tick_left / tick_len) / speed;     /* tick was already advanced past the one sounding */
        f -= 1.0 / speed;
        pp.frac = (float)(f < 0 ? 0 : f > 1 ? 1 : f);
    }
    pos_out = pp;
}

/* ------------------------------------------------------------- transport ---- */
void pl_play(int m, int o, int p, int r)
{
    pl_lock();
    for (int t = 0; t < NTRK; t++) { chan_cut(&ch[t]); ch[t].loop_row = ch[t].loop_count = 0; ch[t].pan = S->pan[t]; ch[t].has_delayed = 0; }
    mode = m;
    if (m == PL_SONG) { ord = o < S->nord ? o : 0; pat = S->order[ord]; }
    else { pat = p; ord = o; }
    row = r < S->pat[pat].rows ? r : 0;
    tick = 0; tick_left = 0; speed = S->speed ? S->speed : 6; set_tempo(S->bpm); pt = S->pt_mode;
    jump_ord = break_row = loop_jump_row = -1; pat_delay = 0; skip_pat = -1;
    PlayPos pp = { mode, ord, pat, row, 0, speed, bpm }; pos_out = pp;
    pl_unlock();
}

void pl_stop(void)
{
    pl_lock();
    mode = PL_STOPPED;
    for (int t = 0; t < NTRK; t++) chan_cut(&ch[t]);
    PlayPos pp = pos_out; pp.mode = PL_STOPPED; pos_out = pp;
    pl_unlock();
}

void pl_set_loop_pattern(int on) { loop_pattern = on; }

void pl_skip_once(int p, int r, int t)
{
    pl_lock(); skip_pat = p; skip_row = r; skip_trk = t; pl_unlock();
}

void pl_jam_on(int key, int ins, int note, int vol)
{
    if (key < 0 || key >= NJAM || ins < 0 || ins > NINS) return;
    pl_lock();
    Chan *c = &jam[key];
    c->insno = ins; c->vol = vol * (S->ins[ins].data ? S->ins[ins].vol : 64) / 64; c->pan = 128;
    chan_trigger(c, note, 0);
    chan_update_step(c);
    pl_unlock();
}

void pl_jam_off(int key)
{
    if (key < 0 || key >= NJAM) return;
    pl_lock();
    if (jam[key].active && jam[key].ins && jam[key].ins->loop_len) chan_cut(&jam[key]);
    pl_unlock();
}

void pl_stinger(const int16_t *data, uint32_t frames, float rate)
{
    pl_lock();
    memset(&sting_ins, 0, sizeof sting_ins);
    sting_ins.data = (int16_t *)data; sting_ins.frames = frames; sting_ins.ch = 1; sting_ins.vol = 64; sting_ins.base_rate = rate;
    ins_update_rate(&sting_ins);
    memset(&sting, 0, sizeof sting);
    sting.ins = &sting_ins; sting.period = sting.target = 428; sting.vol = 64; sting.pan = 128;
    sting.active = 1; sting.lg = sting.rg = -1;
    chan_update_step(&sting);
    pl_unlock();
}

void pl_stinger_stop(void)
{
    pl_lock();
    if (sting.active) sting.fading = 1;
    pl_unlock();
}
