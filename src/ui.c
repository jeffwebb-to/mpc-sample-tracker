/*
 * User interface of the MPC Sample tracker: screens, pad and button handling, pad LEDs.
 *
 * Threads: the control-surface reader thread calls ui_midi() for every message. It starts
 * pad sounds at once (so the latency of a pad hit does not depend on the screen) and queues
 * an event. The UI thread (ui_run) handles the events, draws and updates the LEDs.
 *
 * MIT licence, see LICENSE.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "font5x7.h"
#include "hw.h"
#include "intro.h"
#include "player.h"
#include "song.h"
#include "ui.h"

/* --------------------------------------------------------------- colours ---- */
#define C_BG      0x0D1117
#define C_ALT     0x161B22
#define C_PANEL   0x21262D
#define C_DIM     0x4D5560
#define C_TEXT    0xC9D1D9
#define C_BRIGHT  0xF0F6FC
#define C_INS     0x79C0FF
#define C_VOL     0x7EE787
#define C_FX      0xFFA657
#define C_CURROW  0x1F3A5F
#define C_PLAYROW 0x1B4D2B
#define C_RED     0xDA3633
#define C_GREEN   0x2EA043
#define C_YELLOW  0xE3B341
#define C_BLACK   0x000000

/* ------------------------------------------------------- surface numbers ---- */
enum { B_KNOBFX = 0, B_PADFX = 1, B_SAMPLE = 6, B_SEQ = 7, B_ERASE = 9, B_NOTEREP = 11, B_SCR1 = 12, B_SCR2 = 13, B_SCR3 = 14,
       B_BANK = 35, B_SAMPLESEL = 36, B_16LEV = 40, B_MUTE = 43, B_SHIFT = 49, B_TAP = 53, B_PLUS = 54, B_MINUS = 55,
       B_SAMPLEREC = 73, B_LOOP = 74, B_CHOP = 75, B_SEQREC = 80, B_STOP = 81, B_PLAY = 82, B_DATA = 111 };
enum { CC_K1 = 16, CC_K2 = 17, CC_K3 = 18, CC_VOLUME = 19, CC_DATA = 100, CC_BANK_A = 57 };
static const int pad_note[16] = { 37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53 };

/* ----------------------------------------------------------------- state ---- */
enum { SCR_PAT, SCR_INS, SCR_SONG, SCR_FILES };
enum { F_NOTE, F_INS, F_VOL, F_FX, F_PRM, NFIELDS };
enum { PM_KIT, PM_KEYS, PM_STEP };
enum { BR_SAMPLE, BR_SONG };

static Song song, tmpsong;
static char card[400];
static volatile int screen = SCR_PAT, pad_mode = PM_KIT, edit, cur_fld, bank, cur_ins = 1, octave = 4;
static volatile int mute_held;
static int prev_screen = SCR_PAT;
static int cur_pat, cur_ord, cur_row, cur_trk;
static int shift, tap_held, seq_held, value_lock, loop_mode = 1, edit_step = 1, last_note = REF_NOTE;
static int ins_fld, song_focus;
static volatile int quit_req;
static char toast[64]; static uint32_t toast_until;
static uint32_t new_confirm_until;
static float vu[NTRK];
static int rec_trk[16];
static uint32_t tap_times[4]; static int tap_n;

/* undo: whole-pattern snapshots */
#define NUNDO 20
static struct { int pat; Pattern p; } undo[NUNDO], redo[NUNDO];
static int n_undo, n_redo;
static uint32_t undo_stamp; static int undo_key = -1;

/* file browser */
#define BR_MAX 400
static struct { char name[64]; int dir; } br[BR_MAX];
static int br_n, br_sel, br_mode;
static char br_dir[400];
static uint32_t br_moved_at; static int br_previewed = -1;

/* ---------------------------------------------------------------- events ---- */
enum { EV_PAD_ON, EV_PAD_OFF, EV_BTN_ON, EV_BTN_OFF, EV_ENC, EV_KNOB };
typedef struct { int type, a, b, ins, note; PlayPos pos; } Event;
#define EVQ 256
static Event evq[EVQ];
static unsigned ev_w, ev_r;
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;

static void ev_push(Event e)
{
    pthread_mutex_lock(&ev_lock);
    if (ev_w - ev_r < EVQ) { evq[ev_w % EVQ] = e; ev_w++; }
    pthread_cond_signal(&ev_cond);
    pthread_mutex_unlock(&ev_lock);
}

static int ev_pop(Event *e, int wait_ms)
{
    pthread_mutex_lock(&ev_lock);
    if (ev_w == ev_r && wait_ms > 0) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)wait_ms * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&ev_cond, &ev_lock, &ts);
    }
    int got = ev_w != ev_r;
    if (got) { *e = evq[ev_r % EVQ]; ev_r++; }
    pthread_mutex_unlock(&ev_lock);
    return got;
}

/* What a pad plays right now, decided on the reader thread. Returns 0 when the pad is not a sound. */
static int pad_sound(int pad, int *ins, int *note)
{
    if (mute_held || screen == SCR_FILES) return 0;
    if (screen == SCR_PAT) {
        if (pad_mode == PM_STEP) return 0;
        if (edit && cur_fld == F_INS) { *ins = bank * 16 + pad + 1; *note = REF_NOTE; return 1; }
        if (edit && cur_fld != F_NOTE) return 0;
        if (pad_mode == PM_KEYS) { *ins = cur_ins; *note = 1 + 12 * octave + pad; return *note <= NOTE_MAX; }
    }
    *ins = bank * 16 + pad + 1; *note = REF_NOTE;
    return 1;
}

void ui_midi(const uint8_t *m, int len)
{
    if (len != 3) return;
    int st = m[0] & 0xF0, chn = m[0] & 0x0F;
    Event e; memset(&e, 0, sizeof e);
    if (chn == 0 && st == 0xB0 && m[1] == CC_VOLUME) {              /* MAIN VOLUME: set at once, even during the intro */
        float x = (float)m[2] / 127.0f;
        pl_set_master(1.2f * x * x);
        if (!intro_active()) { e.type = EV_KNOB; e.a = 3; e.b = m[2]; ev_push(e); }
        return;
    }
    if (intro_active()) { if (st == 0x90 && m[2] > 0 && (chn == 0 || chn == 9)) intro_abort(); return; }
    if (chn == 9 && (st == 0x90 || st == 0x80)) {
        int pad = -1;
        for (int i = 0; i < 16; i++) if (pad_note[i] == m[1]) pad = i;
        if (pad < 0) return;
        e.a = pad;
        if (st == 0x90 && m[2] > 0) {
            e.type = EV_PAD_ON; e.b = m[2]; e.pos = pl_pos();
            if (pad_sound(pad, &e.ins, &e.note)) pl_jam_on(pad, e.ins, e.note, (m[2] * 64 + 63) / 127);
            else e.ins = 0;
        } else { e.type = EV_PAD_OFF; e.pos = pl_pos(); pl_jam_off(pad); }
        ev_push(e);
    } else if (chn == 0 && (st == 0x90 || st == 0x80)) {
        e.type = (st == 0x90 && m[2] > 0) ? EV_BTN_ON : EV_BTN_OFF; e.a = m[1];
        ev_push(e);
    } else if (chn == 0 && st == 0xB0) {
        if (m[1] == CC_DATA) { e.type = EV_ENC; e.a = m[2] < 64 ? m[2] : m[2] - 128; ev_push(e); }
        else if (m[1] >= CC_K1 && m[1] <= CC_K3) { e.type = EV_KNOB; e.a = m[1] - CC_K1; e.b = m[2]; ev_push(e); }
    }
}

/* -------------------------------------------------------------- drawing ---- */
static void rect(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    for (int j = 0; j < h; j++) { uint32_t *p = scr_fb + (size_t)(y + j) * SCR_W + x; for (int i = 0; i < w; i++) p[i] = c; }
}

static void glyph(int x, int y, int chr, uint32_t c)
{
    if (chr < 32 || chr > 126) chr = '?';
    const unsigned char *g = font5x7[chr - 32];
    for (int col = 0; col < 5; col++) {
        int px = x + col;
        if (px < 0 || px >= SCR_W) continue;
        for (int r = 0; r < 7; r++)
            if ((g[col] >> r & 1) && y + r >= 0 && y + r < SCR_H) scr_fb[(size_t)(y + r) * SCR_W + px] = c;
    }
}

static int text(int x, int y, const char *s, uint32_t c)
{
    for (; *s; s++, x += 6) glyph(x, y, (unsigned char)*s, c);
    return x;
}

static int textf(int x, int y, uint32_t c, const char *fmt, ...)
{
    char b[96]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    return text(x, y, b, c);
}

static void say(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(toast, sizeof toast, fmt, ap); va_end(ap);
    toast_until = hw_ms() + 2500;
    hw_log("%s", toast);
}

static void ins_label(int n, char out[4])
{
    if (n < 1 || n > NINS) { strcpy(out, "---"); return; }
    snprintf(out, 4, "%c%02d", 'A' + (n - 1) / 16, (n - 1) % 16 + 1);
}

/* ------------------------------------------------------------------ undo ---- */
static void undo_push(int key)
{
    uint32_t now = hw_ms();
    if (key >= 0 && key == undo_key && now - undo_stamp < 1500 && n_undo) { undo_stamp = now; return; }   /* one step for a run of encoder ticks */
    undo_key = key; undo_stamp = now;
    if (n_undo == NUNDO) { memmove(&undo[0], &undo[1], sizeof undo[0] * (NUNDO - 1)); n_undo--; }
    undo[n_undo].pat = cur_pat; undo[n_undo].p = song.pat[cur_pat]; n_undo++;
    n_redo = 0;
}

static void undo_do(int is_redo)
{
    int *n_from = is_redo ? &n_redo : &n_undo, *n_to = is_redo ? &n_undo : &n_redo;
    if (!*n_from) { say(is_redo ? "NOTHING TO REDO" : "NOTHING TO UNDO"); return; }
    typeof(undo[0]) *from = is_redo ? redo : undo, *to = is_redo ? undo : redo;
    (*n_from)--;
    int p = from[*n_from].pat;
    if (*n_to < NUNDO) { to[*n_to].pat = p; to[*n_to].p = song.pat[p]; (*n_to)++; }
    song.pat[p] = from[*n_from].p;
    cur_pat = p; undo_key = -1;
    say(is_redo ? "REDO" : "UNDO");
}

/* ------------------------------------------------------------- transport ---- */
static int playing(void) { return pl_pos().mode != PL_STOPPED; }

static void start_play(int from_row)
{
    int o = -1;
    if (cur_ord < song.nord && song.order[cur_ord] == cur_pat) o = cur_ord;
    else for (int i = 0; i < song.nord; i++) if (song.order[i] == cur_pat) { o = i; break; }
    pl_set_loop_pattern(loop_mode);
    if (o >= 0) { cur_ord = o; pl_play(PL_SONG, o, cur_pat, from_row); }
    else pl_play(PL_PATTERN, cur_ord, cur_pat, from_row);
}

/* ----------------------------------------------------------------- files ---- */
static void mkdirs(void)
{
    char p[500];
    snprintf(p, sizeof p, "%s/tracker", card); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/tracker/songs", card); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/tracker/samples", card); mkdir(p, 0755);
}

static void adopt_song(void)             /* tmpsong -> song, with everything stopped */
{
    pl_stop();
    pl_lock();
    pl_all_off();
    song_free(&song);
    memcpy(&song, &tmpsong, sizeof song);
    memset(&tmpsong, 0, sizeof tmpsong);
    pl_init(&song);
    pl_unlock();
    cur_pat = song.order[0]; cur_ord = 0; cur_row = 0; cur_trk = 0; n_undo = n_redo = 0; cur_ins = 1; bank = 0;
}

static int has_ext(const char *n, const char *ext)
{
    size_t a = strlen(n), b = strlen(ext);
    return a > b && !strcasecmp(n + a - b, ext);
}

static void load_song_file(const char *path)
{
    char err[64] = "";
    int r = has_ext(path, ".mod") ? mod_import(&tmpsong, path, err, sizeof err) : song_load(&tmpsong, path, err, sizeof err);
    if (r < 0) { say("%s", err); return; }
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    if (!has_ext(path, ".mod") && strcasecmp(b, "AUTOSAVE.mst")) {
        size_t i = 0;
        for (; b[i] && b[i] != '.' && i < sizeof tmpsong.name - 1; i++) tmpsong.name[i] = b[i];
        tmpsong.name[i] = 0;
    }
    adopt_song();
    if (has_ext(path, ".mod")) { loop_mode = 0; pl_set_loop_pattern(0); }      /* a module is a whole song: play it through */
    say("LOADED %s", b);
}

static int name_is_placeholder(void) { return !song.name[0] || !strcmp(song.name, "UNTITLED") || !strcmp(song.name, "DEMO"); }

static void save_song(int as_new)
{
    char path[600], err[64] = "";
    mkdirs();
    if (as_new || name_is_placeholder() || strpbrk(song.name, "/\\")) {
        int n = 1;
        for (; n < 1000; n++) {
            snprintf(path, sizeof path, "%s/tracker/songs/SONG%03d.mst", card, n);
            if (access(path, F_OK) != 0) break;
        }
        snprintf(song.name, sizeof song.name, "SONG%03d", n);
    }
    snprintf(path, sizeof path, "%s/tracker/songs/%s.mst", card, song.name);
    if (song_save(&song, path, err, sizeof err) < 0) say("%s", err);
    else say("SAVED %s.MST", song.name);
}

static void autosave(void)
{
    char path[600], err[64];
    mkdirs();
    snprintf(path, sizeof path, "%s/tracker/songs/AUTOSAVE.mst", card);
    if (song_save(&song, path, err, sizeof err) < 0) hw_log("autosave: %s", err); else hw_log("autosaved to %s", path);
}

static int br_cmp(const void *a, const void *b)
{
    const typeof(br[0]) *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void br_scan(void)
{
    br_n = 0; br_sel = 0; br_previewed = -1;
    DIR *d = opendir(br_dir);
    if (!d) { say("CANNOT OPEN FOLDER"); return; }
    struct dirent *de;
    while ((de = readdir(d)) && br_n < BR_MAX) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof br[0].name) continue;
        char p[700]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", br_dir, de->d_name);
        if (stat(p, &st) != 0) continue;
        int dir = S_ISDIR(st.st_mode);
        if (!dir && !(br_mode == BR_SAMPLE ? has_ext(de->d_name, ".wav") : (has_ext(de->d_name, ".mst") || has_ext(de->d_name, ".mod")))) continue;
        if (dir && (!strcasecmp(de->d_name, "autorun") || !strcasecmp(de->d_name, "System Volume Information"))) continue;
        snprintf(br[br_n].name, sizeof br[br_n].name, "%s", de->d_name); br[br_n].dir = dir; br_n++;
    }
    closedir(d);
    qsort(br, (size_t)br_n, sizeof br[0], br_cmp);
}

static void br_open(int mode)
{
    struct stat st;
    br_mode = mode;
    snprintf(br_dir, sizeof br_dir, "%s/tracker/%s", card, mode == BR_SAMPLE ? "samples" : "songs");
    if (stat(br_dir, &st) != 0) snprintf(br_dir, sizeof br_dir, "%s", card);
    if (screen != SCR_FILES) prev_screen = screen;
    screen = SCR_FILES;
    br_scan();
    if (br_n == 0 && strcmp(br_dir, card)) { snprintf(br_dir, sizeof br_dir, "%s", card); br_scan(); }   /* nothing there yet: start at the card root */
}

static void br_up(void)
{
    if (!strcmp(br_dir, card)) return;
    char *s = strrchr(br_dir, '/');
    if (s && s > br_dir) *s = 0;
    if (strlen(br_dir) < strlen(card)) snprintf(br_dir, sizeof br_dir, "%s", card);
    br_scan();
}

static void preview_stop(void)
{
    pl_lock(); pl_ins_changed(0); int16_t *old = song.ins[0].data; memset(&song.ins[0], 0, sizeof song.ins[0]); pl_unlock();
    free(old);
}

static void br_preview(void)
{
    if (br_mode != BR_SAMPLE || br_sel >= br_n || br[br_sel].dir) return;
    char p[700], err[64]; Ins in;
    snprintf(p, sizeof p, "%s/%s", br_dir, br[br_sel].name);
    if (wav_load_max(p, &in, 441000, err, sizeof err) < 0) { say("%s", err); return; }     /* ten seconds is plenty to audition */
    pl_lock(); pl_ins_changed(0); int16_t *old = song.ins[0].data; song.ins[0] = in; pl_unlock();
    free(old);
    pl_jam_on(0, 0, REF_NOTE, 64);
}

static void br_choose(void)
{
    if (br_sel >= br_n) return;
    char p[700];
    snprintf(p, sizeof p, "%s/%s", br_dir, br[br_sel].name);
    if (br[br_sel].dir) { snprintf(br_dir, sizeof br_dir, "%s", p); br_scan(); return; }
    if (br_mode == BR_SONG) { load_song_file(p); screen = SCR_PAT; return; }
    char err[64]; Ins in;
    if (wav_load(p, &in, err, sizeof err) < 0) { say("%s", err); return; }
    size_t old_bytes = song.ins[cur_ins].data ? (size_t)song.ins[cur_ins].frames * song.ins[cur_ins].ch * sizeof(int16_t) : 0;
    if (song_sample_bytes(&song) - old_bytes + (size_t)in.frames * in.ch * sizeof(int16_t) > MAX_SONG_BYTES) { free(in.data); say("OUT OF SAMPLE MEMORY"); return; }
    preview_stop();
    pl_lock(); pl_ins_changed(cur_ins); int16_t *old = song.ins[cur_ins].data; song.ins[cur_ins] = in; pl_unlock();
    free(old);
    char lab[4]; ins_label(cur_ins, lab);
    say("%s = %s", lab, in.name);
    screen = prev_screen == SCR_FILES ? SCR_INS : prev_screen;
    pl_jam_on(0, cur_ins, REF_NOTE, 64);
}

/* --------------------------------------------------------- pattern edits ---- */
static Cell *cell_at(int row, int trk) { return &song.pat[cur_pat].c[row][trk]; }

static void advance(void)
{
    int rows = song.pat[cur_pat].rows;
    if (edit_step > 0) cur_row = (cur_row + edit_step) % rows;
}

static int vel_to_vol(int vel) { return vel >= 112 ? 0 : 1 + vel * 64 / 112; }     /* a hard hit leaves the volume column empty */

static void write_note(const Event *e)
{
    int rows = song.pat[cur_pat].rows;
    int row = cur_row, trk = cur_trk, live = e->pos.mode != PL_STOPPED;
    if (live) {
        cur_pat = e->pos.pat; rows = song.pat[cur_pat].rows;
        row = e->pos.row;
        int ahead = e->pos.frac > 0.5f;
        if (ahead) row = row + 1 < rows ? row + 1 : (loop_mode || e->pos.mode == PL_PATTERN ? 0 : row);
        for (int k = 0; k < NTRK; k++) { int t = (cur_trk + k) % NTRK; if (!song.pat[cur_pat].c[row][t].note) { trk = t; break; } }
        if (row != e->pos.row) pl_skip_once(cur_pat, row, trk);     /* it already sounded under the finger */
    }
    undo_push(-1);
    Cell *c = cell_at(row, trk);
    c->note = (uint8_t)e->note; c->ins = (uint8_t)e->ins; c->vol = (uint8_t)vel_to_vol(e->b);
    rec_trk[e->a] = trk;
    if (!live) advance();
}

static void pad_on(const Event *e)
{
    int pad = e->a;
    if (mute_held) { if (pad < NTRK) { song.mute[pad] ^= 1; say("TRACK %d %s", pad + 1, song.mute[pad] ? "MUTED" : "ON"); } return; }
    if (screen == SCR_FILES) return;
    if (screen != SCR_PAT) { if (e->ins) { cur_ins = e->ins; } return; }
    if (pad_mode == PM_STEP) {
        int row = cur_row / 16 * 16 + pad;
        if (row >= song.pat[cur_pat].rows) return;
        undo_push(-1);
        Cell *c = cell_at(row, cur_trk);
        if (c->note) { c->note = c->ins = c->vol = 0; }
        else { c->note = (uint8_t)last_note; c->ins = (uint8_t)cur_ins; pl_jam_on(pad, cur_ins, last_note, 64); }
        if (!playing()) cur_row = row;
        return;
    }
    if (edit && cur_fld != F_NOTE) {                 /* the pads pick a value for the column under the cursor */
        undo_push(-1);
        Cell *c = cell_at(cur_row, cur_trk);
        switch (cur_fld) {
        case F_INS: c->ins = (uint8_t)e->ins; cur_ins = e->ins; break;
        case F_VOL: c->vol = (uint8_t)(1 + (pad * 64 + 7) / 15); break;
        case F_FX:  c->fx = (uint8_t)pad; break;
        case F_PRM: c->prm = (uint8_t)(pad * 17); break;
        }
        if (!playing()) advance();
        return;
    }
    if (!e->ins) return;
    if (pad_mode == PM_KIT) cur_ins = e->ins;
    last_note = e->note;
    if (edit) write_note(e);
}

static void pad_off(const Event *e)
{
    if (screen != SCR_PAT || !edit || pad_mode != PM_KEYS || e->pos.mode == PL_STOPPED) return;
    if (!song.ins[cur_ins].data || !song.ins[cur_ins].loop_len) return;
    Cell *c = &song.pat[e->pos.pat].c[e->pos.row][rec_trk[e->a]];
    if (!c->note) c->note = NOTE_CUT;                /* looped sounds need an end */
}

static void edit_value(int d)
{
    Cell *c = cell_at(cur_row, cur_trk);
    undo_push(cur_row * 64 + cur_trk * 8 + cur_fld);
    int v;
    switch (cur_fld) {
    case F_NOTE:
        v = (c->note >= NOTE_MIN && c->note <= NOTE_MAX) ? c->note + d : last_note;
        v = v < NOTE_MIN ? NOTE_MIN : v > NOTE_MAX ? NOTE_MAX : v;
        c->note = (uint8_t)v; last_note = v;
        if (!c->ins) c->ins = (uint8_t)cur_ins;
        break;
    case F_INS: v = c->ins + d; c->ins = (uint8_t)(v < 0 ? 0 : v > NINS ? NINS : v); if (c->ins) cur_ins = c->ins; break;
    case F_VOL: v = c->vol + d; c->vol = (uint8_t)(v < 0 ? 0 : v > 65 ? 65 : v); break;
    case F_FX:  c->fx = (uint8_t)((c->fx + d) & 15); break;
    case F_PRM: c->prm = (uint8_t)(c->prm + d); break;
    }
}

static void erase(void)
{
    undo_push(-1);
    if (shift) { for (int r = 0; r < MAXROWS; r++) memset(cell_at(r, cur_trk), 0, sizeof(Cell)); say("TRACK %d CLEARED", cur_trk + 1); return; }
    Cell *c = cell_at(cur_row, cur_trk);
    switch (cur_fld) {
    case F_NOTE: c->note = c->ins = c->vol = 0; break;
    case F_INS: c->ins = 0; break;
    case F_VOL: c->vol = 0; break;
    default: c->fx = c->prm = 0; break;
    }
    if (!playing()) advance();
}

/* --------------------------------------------------------------- song ops ---- */
enum { SF_ORDER, SF_BPM, SF_SPEED, SF_ROWS, SF_INSERT, SF_CLONE, SF_DELETE, SF_N };
static const char *sf_name[SF_N] = { "POSITION", "TEMPO BPM", "SPEED", "PATTERN ROWS", "ADD EMPTY PATTERN", "ADD COPY OF PATTERN", "REMOVE POSITION" };

static int free_pattern(void)
{
    for (int p = 0; p < MAXPAT; p++) {
        int used = 0;
        for (int i = 0; i < song.nord; i++) if (song.order[i] == p) used = 1;
        if (!used && pattern_empty(&song.pat[p])) return p;
    }
    return -1;
}

static void song_action(void)
{
    if (song_focus == SF_INSERT || song_focus == SF_CLONE) {
        int p = free_pattern();
        if (p < 0 || song.nord >= MAXORD) { say("SONG IS FULL"); return; }
        pl_lock();
        if (song_focus == SF_CLONE) song.pat[p] = song.pat[cur_pat]; else song.pat[p].rows = song.pat[cur_pat].rows;
        memmove(&song.order[cur_ord + 2], &song.order[cur_ord + 1], (size_t)(song.nord - cur_ord - 1));
        song.order[cur_ord + 1] = (uint8_t)p; song.nord++;
        pl_unlock();
        cur_ord++; cur_pat = p; cur_row = 0;
        say("PATTERN %02d ADDED AT POSITION %02d", p, cur_ord);
    } else if (song_focus == SF_DELETE) {
        if (song.nord <= 1) { say("A SONG NEEDS ONE POSITION"); return; }
        pl_lock();
        memmove(&song.order[cur_ord], &song.order[cur_ord + 1], (size_t)(song.nord - cur_ord - 1));
        song.nord--;
        pl_unlock();
        if (cur_ord >= song.nord) cur_ord = song.nord - 1;
        cur_pat = song.order[cur_ord];
        say("POSITION REMOVED");
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------------------------------------------------------------- input ---- */
static void encoder(int d)
{
    if (tap_held) { song.bpm = (uint8_t)clampi(song.bpm + d, 32, 255); return; }
    switch (screen) {
    case SCR_PAT:
        if (seq_held) { if (!playing()) { cur_pat = clampi(cur_pat + d, 0, MAXPAT - 1); cur_row = clampi(cur_row, 0, song.pat[cur_pat].rows - 1); } }
        else if (shift || value_lock) edit_value(shift && value_lock ? d * (cur_fld == F_NOTE ? 12 : 16) : d);
        else if (!playing()) { int rows = song.pat[cur_pat].rows; cur_row = ((cur_row + d) % rows + rows) % rows; }
        break;
    case SCR_INS: {
        Ins *in = &song.ins[cur_ins];
        if (!(shift || value_lock)) { cur_ins = clampi(cur_ins + d, 1, NINS); bank = (cur_ins - 1) / 16; break; }
        if (!in->data) break;
        if (ins_fld == 0) in->vol = (uint8_t)clampi(in->vol + d, 0, 64);
        else if (ins_fld == 1) { in->tune = (int8_t)clampi(in->tune + d, -36, 36); ins_update_rate(in); }
        else if (ins_fld == 2) { in->fine = (int8_t)clampi(in->fine + d, -50, 50); ins_update_rate(in); }
        else { pl_lock(); if (in->loop_len) in->loop_len = 0; else { in->loop_start = 0; in->loop_len = in->frames; } pl_ins_changed(cur_ins); pl_unlock(); }
        break; }
    case SCR_SONG:
        if (song_focus == SF_ORDER) {
            if (shift || value_lock) { if (!playing()) { song.order[cur_ord] = (uint8_t)clampi(song.order[cur_ord] + d, 0, MAXPAT - 1); cur_pat = song.order[cur_ord]; } }
            else if (!playing()) { cur_ord = clampi(cur_ord + d, 0, song.nord - 1); cur_pat = song.order[cur_ord]; cur_row = 0; }
        } else if (song_focus == SF_BPM) song.bpm = (uint8_t)clampi(song.bpm + d, 32, 255);
        else if (song_focus == SF_SPEED) song.speed = (uint8_t)clampi(song.speed + d, 1, 31);
        else if (song_focus == SF_ROWS && !playing()) { pl_lock(); song.pat[cur_pat].rows = (uint8_t)clampi(song.pat[cur_pat].rows + d * (shift ? 16 : 1), 1, MAXROWS); pl_unlock(); cur_row = clampi(cur_row, 0, song.pat[cur_pat].rows - 1); }
        break;
    case SCR_FILES:
        if (br_n) { br_sel = clampi(br_sel + d, 0, br_n - 1); br_moved_at = hw_ms(); }
        break;
    }
}

static void go_screen(int s)
{
    if (screen == SCR_FILES) preview_stop();
    screen = s; value_lock = 0;
}

static void tap_tempo(void)
{
    uint32_t now = hw_ms();
    if (tap_n && now - tap_times[tap_n - 1] > 2000) tap_n = 0;
    if (tap_n == 4) { memmove(tap_times, tap_times + 1, sizeof tap_times[0] * 3); tap_n = 3; }
    tap_times[tap_n++] = now;
    if (tap_n >= 2) {
        float avg = (float)(tap_times[tap_n - 1] - tap_times[0]) / (float)(tap_n - 1);
        if (avg > 0) { song.bpm = (uint8_t)clampi((int)(60000.0f / avg + 0.5f), 32, 255); say("TEMPO %d BPM", song.bpm); }
    }
}

static void button(int b, int on)
{
    switch (b) {
    case B_SHIFT: shift = on; return;
    case B_MUTE: mute_held = on; return;
    case B_TAP: tap_held = on; if (on) tap_tempo(); return;
    case B_SEQ: seq_held = on; if (on) go_screen(SCR_PAT); return;
    }
    if (!on) return;
    switch (b) {
    case B_PLAY:
        if (screen == SCR_FILES && br_mode == BR_SAMPLE) { br_preview(); break; }
        start_play(shift ? cur_row : 0);
        break;
    case B_STOP:
        if (shift) { quit_req = 1; break; }
        if (screen == SCR_FILES) { go_screen(prev_screen); break; }
        if (playing()) pl_stop(); else { cur_row = 0; pl_lock(); pl_all_off(); pl_unlock(); }
        break;
    case B_SEQREC: edit ^= 1; break;
    case B_LOOP: loop_mode ^= 1; pl_set_loop_pattern(loop_mode); say(loop_mode ? "LOOP PATTERN" : "PLAY SONG"); break;
    case B_16LEV: pad_mode = pad_mode == PM_KEYS ? PM_KIT : PM_KEYS; cur_fld = F_NOTE; break;
    case B_NOTEREP: pad_mode = pad_mode == PM_STEP ? PM_KIT : PM_STEP; cur_fld = F_NOTE; break;
    case B_BANK:
        if (pad_mode == PM_KEYS && screen == SCR_PAT) octave = clampi(octave + (shift ? -1 : 1), 0, 8);
        else { bank = (bank + (shift ? 3 : 1)) & 3; cur_ins = bank * 16 + (cur_ins - 1) % 16 + 1; }
        break;
    case B_SAMPLE: go_screen(SCR_INS); break;
    case B_SAMPLESEL: br_open(BR_SAMPLE); break;
    case B_SCR1: if (shift) save_song(0); else go_screen(SCR_PAT); break;
    case B_SCR2: if (shift) br_open(BR_SONG); else go_screen(SCR_INS); break;
    case B_SCR3:
        if (!shift) { go_screen(SCR_SONG); break; }
        if (hw_ms() < new_confirm_until) { song_init(&tmpsong); adopt_song(); new_confirm_until = 0; say("NEW SONG"); }
        else { new_confirm_until = hw_ms() + 3000; say("PRESS AGAIN TO ERASE THE SONG"); }
        break;
    case B_CHOP: undo_do(shift); break;
    case B_ERASE:
        if (screen == SCR_PAT) erase();
        else if (screen == SCR_INS && shift && song.ins[cur_ins].data) {
            pl_lock(); pl_ins_changed(cur_ins); int16_t *old = song.ins[cur_ins].data; memset(&song.ins[cur_ins], 0, sizeof(Ins)); pl_unlock();
            free(old); say("SAMPLE REMOVED");
        } else if (screen == SCR_INS) say("HOLD SHIFT TO REMOVE THE SAMPLE");
        break;
    case B_PLUS: case B_MINUS: {
        int d = b == B_PLUS ? 1 : -1;
        if (screen == SCR_PAT) {
            if (shift) cur_trk = (cur_trk + d + NTRK) % NTRK;
            else {
                int f = cur_fld + d;
                if (f >= NFIELDS) { f = 0; cur_trk = (cur_trk + 1) % NTRK; }
                else if (f < 0) { f = NFIELDS - 1; cur_trk = (cur_trk + NTRK - 1) % NTRK; }
                cur_fld = f;
            }
        } else if (screen == SCR_INS) ins_fld = (ins_fld + d + 4) % 4;
        else if (screen == SCR_SONG) song_focus = (song_focus + d + SF_N) % SF_N;
        else if (screen == SCR_FILES && d < 0) br_up();
        break; }
    case B_DATA:
        if (screen == SCR_FILES) br_choose();
        else if (screen == SCR_INS) br_open(BR_SAMPLE);
        else if (screen == SCR_SONG && song_focus >= SF_INSERT) song_action();
        else value_lock ^= 1;
        break;
    }
}

static void knob(int k, int v)
{
    if (k == 3) { static int last = -1; if (v != last) { last = v; say("VOLUME %d%%", v * 100 / 127); } }
    else if (k == 1) { int s = v * 9 / 128; if (s != edit_step) { edit_step = s; say("EDIT STEP %d", s); } }
}

/* -------------------------------------------------------------- screens ---- */
static const char *fx_name[16] = { "ARPEGGIO", "SLIDE UP", "SLIDE DOWN", "GLIDE TO NOTE", "VIBRATO", "GLIDE + VOL SLIDE", "VIBRATO + VOL SLIDE", "TREMOLO",
    "PAN", "SAMPLE OFFSET", "VOLUME SLIDE", "JUMP TO POSITION", "SET VOLUME", "PATTERN BREAK", "EXTENDED", "SPEED / TEMPO" };
static const char *efx_name[16] = { "", "FINE SLIDE UP", "FINE SLIDE DOWN", "", "", "", "LOOP ROWS", "", "CHANCE x/16", "RETRIGGER", "FINE VOL UP", "FINE VOL DOWN", "CUT NOTE", "DELAY NOTE", "PATTERN DELAY", "" };
static const char hexd[] = "0123456789ABCDEF";

static void badge(int x, int y, const char *s, uint32_t bg, uint32_t fg)
{
    int w = (int)strlen(s) * 6 + 5;
    rect(x, y - 1, w, 10, bg);
    text(x + 3, y, s, fg);
}

static void draw_header(void)
{
    PlayPos pp = pl_pos();
    rect(0, 0, SCR_W, 21, C_PANEL);
    char nm[17]; snprintf(nm, sizeof nm, "%s", song.name);
    text(3, 2, nm, C_BRIGHT);
    textf(104, 2, C_TEXT, "PAT");  textf(124, 2, C_BRIGHT, "%02d", cur_pat);
    textf(142, 2, C_TEXT, "POS");  textf(162, 2, C_BRIGHT, "%02d/%02d", cur_ord, song.nord - 1);
    int live = pp.mode != PL_STOPPED && pp.bpm > 0;                    /* the song may change speed and tempo as it plays */
    textf(198, 2, C_TEXT, "BPM");  textf(218, 2, tap_held ? C_YELLOW : C_BRIGHT, "%3d", live ? pp.bpm : song.bpm);
    textf(242, 2, C_TEXT, "SPD");  textf(262, 2, C_BRIGHT, "%d", live ? pp.speed : song.speed);
    if (pp.mode != PL_STOPPED) badge(284, 2, loop_mode ? "LOOP" : "SONG", C_GREEN, C_BLACK);
    else badge(284, 2, "STOP", C_BG, C_DIM);
    char lab[4]; ins_label(cur_ins, lab);
    text(3, 12, lab, C_INS);
    char in[15]; snprintf(in, sizeof in, "%s", song.ins[cur_ins].data ? song.ins[cur_ins].name : "(EMPTY)");
    text(24, 12, in, song.ins[cur_ins].data ? C_TEXT : C_DIM);
    textf(114, 12, C_DIM, "STEP"); textf(140, 12, C_TEXT, "%d", edit_step);
    if (pad_mode == PM_KEYS) textf(154, 12, C_TEXT, "OCT%d", octave);
    badge(188, 12, pad_mode == PM_KIT ? "KIT" : pad_mode == PM_KEYS ? "KEYS" : "STEP", C_BG, C_INS);
    if (value_lock) badge(224, 12, "VAL", C_YELLOW, C_BLACK);
    badge(250, 12, edit ? "WRITE" : "PLAY ", edit ? C_RED : C_BG, edit ? C_BRIGHT : C_VOL);
    if (aud_underruns()) textf(292, 12, C_DIM, "X%u", aud_underruns() % 1000);
}

static void draw_softkeys(void)
{
    static const char *norm[3] = { "PATTERN", "SAMPLES", "SONG" }, *alt[3] = { "SAVE", "LOAD", "NEW" };
    for (int i = 0; i < 3; i++) {
        int x = 1 + i * 106, act = !shift && ((i == 0 && screen == SCR_PAT) || (i == 1 && screen == SCR_INS) || (i == 2 && screen == SCR_SONG));
        rect(x, 226, 104, 13, act ? C_INS : C_PANEL);
        const char *s = shift ? alt[i] : norm[i];
        text(x + 52 - (int)strlen(s) * 3, 229, s, act ? C_BLACK : shift ? C_YELLOW : C_TEXT);
    }
}

static void draw_info(const char *s, uint32_t c)
{
    if (toast[0] && hw_ms() < toast_until) { s = toast; c = C_YELLOW; }
    text(3, 214, s, c);
}

#define GRID_Y 44
#define ROW_H 10
#define TRK_X0 20
#define TRK_W 75
static const int fld_x[NFIELDS] = { 0, 21, 36, 51, 57 }, fld_w[NFIELDS] = { 18, 12, 12, 6, 12 };

static void draw_pattern(void)
{
    PlayPos pp = pl_pos();
    const Pattern *pt = &song.pat[cur_pat];
    int base = cur_trk / 4 * 4;
    int show_play = pp.mode != PL_STOPPED && pp.pat == cur_pat;
    int page = cur_row / 16;
    /* track headers with level meters; a small strip shows the four tracks that are off screen */
    for (int k = 0; k < 4; k++) {
        int t = base + k, x = TRK_X0 + k * TRK_W;
        uint32_t c = song.mute[t] ? C_RED : t == cur_trk ? C_BRIGHT : C_TEXT;
        textf(x, 24, c, song.mute[t] ? "TRK %d MUTE" : "TRACK %d", t + 1);
        rect(x, 34, 69, 4, C_ALT);
        int w = (int)(vu[t] * 69.0f * 1.4f); if (w > 69) w = 69;
        rect(x, 34, w, 4, w > 60 ? C_YELLOW : C_GREEN);
    }
    for (int k = 0; k < 4; k++) {
        int t = (base + 4) % NTRK + k;
        int h = (int)(vu[t] * 14.0f * 1.4f); if (h > 14) h = 14;
        rect(2 + k * 4, 24, 3, 14, C_ALT);
        rect(2 + k * 4, 38 - h, 3, h, song.mute[t] ? C_RED : C_DIM);
    }
    for (int i = 0; i < 16; i++) {
        int r = page * 16 + i, y = GRID_Y + i * ROW_H;
        if (r >= pt->rows) { rect(0, y, SCR_W, ROW_H, C_BG); continue; }
        uint32_t bg = r % 4 == 0 ? C_ALT : C_BG;
        if (show_play && r == pp.row) bg = C_PLAYROW;
        if (r == cur_row && pp.mode == PL_STOPPED) bg = C_CURROW;
        rect(0, y, SCR_W, ROW_H, bg);
        textf(3, y + 2, r % 4 == 0 ? C_TEXT : C_DIM, "%02d", r);
        for (int k = 0; k < 4; k++) {
            int t = base + k, x = TRK_X0 + k * TRK_W;
            const Cell *c = &pt->c[r][t];
            char s[4];
            if (r == cur_row && t == cur_trk) {
                uint32_t cb = edit ? C_RED : C_DIM;
                if (value_lock) cb = C_YELLOW;
                rect(x + fld_x[cur_fld] - 1, y, fld_w[cur_fld] + 1, ROW_H, cb);
            }
            int at = r == cur_row && t == cur_trk;
            #define FC(f, col) (at && cur_fld == (f) ? (value_lock ? C_BLACK : C_BRIGHT) : (col))
            if (c->note) { note_name(c->note, s); text(x + fld_x[F_NOTE], y + 2, s, FC(F_NOTE, song.mute[t] ? C_DIM : C_BRIGHT)); }
            else text(x + fld_x[F_NOTE], y + 2, "---", FC(F_NOTE, C_DIM));
            if (c->ins) textf(x + fld_x[F_INS], y + 2, FC(F_INS, C_INS), "%02X", c->ins); else text(x + fld_x[F_INS], y + 2, "..", FC(F_INS, C_DIM));
            if (c->vol) textf(x + fld_x[F_VOL], y + 2, FC(F_VOL, C_VOL), "%02d", c->vol - 1); else text(x + fld_x[F_VOL], y + 2, "..", FC(F_VOL, C_DIM));
            if (c->fx || c->prm) {
                s[0] = hexd[c->fx]; s[1] = 0; text(x + fld_x[F_FX], y + 2, s, FC(F_FX, C_FX));
                textf(x + fld_x[F_PRM], y + 2, FC(F_PRM, C_FX), "%02X", c->prm);
            } else { text(x + fld_x[F_FX], y + 2, ".", FC(F_FX, C_DIM)); text(x + fld_x[F_PRM], y + 2, "..", FC(F_PRM, C_DIM)); }
            #undef FC
        }
    }
    for (int k = 1; k < 4; k++) rect(TRK_X0 + k * TRK_W - 4, GRID_Y, 1, 16 * ROW_H, C_PANEL);
    /* what is under the cursor */
    const Cell *c = &pt->c[cur_row][cur_trk];
    char info[64];
    static const char *fn[NFIELDS] = { "NOTE", "SAMPLE", "VOLUME", "EFFECT", "EFFECT VALUE" };
    if (seq_held) snprintf(info, sizeof info, "TURN DATA TO CHANGE PATTERN");
    else if (cur_fld >= F_FX && (c->fx || c->prm)) snprintf(info, sizeof info, "%c%02X %s", hexd[c->fx], c->prm, c->fx == 14 ? efx_name[c->prm >> 4] : fx_name[c->fx]);
    else if (cur_fld == F_INS && c->ins && song.ins[c->ins].data) { char lab[4]; ins_label(c->ins, lab); snprintf(info, sizeof info, "%s %s", lab, song.ins[c->ins].name); }
    else snprintf(info, sizeof info, "T%d ROW %02d  %s%s", cur_trk + 1, cur_row, fn[cur_fld], edit && cur_fld != F_NOTE ? "  (PADS SET IT)" : "");
    draw_info(info, C_TEXT);
}

static void draw_ins(void)
{
    for (int i = 0; i < 16; i++) {
        int n = bank * 16 + i + 1, y = 24 + i * 9;
        const Ins *in = &song.ins[n];
        if (n == cur_ins) rect(0, y - 1, 150, 9, C_CURROW);
        char lab[4]; ins_label(n, lab);
        text(3, y, lab, C_INS);
        char nm[20]; snprintf(nm, sizeof nm, "%s", in->data ? in->name : "-");
        text(27, y, nm, in->data ? C_BRIGHT : C_DIM);
    }
    const Ins *in = &song.ins[cur_ins];
    int x = 158;
    rect(154, 23, 1, 146, C_PANEL);
    if (!in->data) {
        text(x, 30, "EMPTY SLOT", C_DIM);
        text(x, 46, "PUSH DATA TO LOAD", C_TEXT); text(x, 56, "A WAV FROM THE CARD", C_TEXT);
    } else {
        static const char *pn[4] = { "VOLUME", "TUNE", "FINE", "LOOP" };
        char val[4][16];
        snprintf(val[0], 16, "%d", in->vol); snprintf(val[1], 16, "%+d ST", in->tune); snprintf(val[2], 16, "%+d CT", in->fine);
        snprintf(val[3], 16, "%s", in->loop_len ? "ON" : "OFF");
        for (int i = 0; i < 4; i++) {
            int y = 28 + i * 12;
            if (i == ins_fld) rect(x - 2, y - 2, 160, 11, value_lock ? C_YELLOW : C_PANEL);
            text(x, y, pn[i], i == ins_fld && value_lock ? C_BLACK : C_TEXT);
            text(x + 60, y, val[i], i == ins_fld && value_lock ? C_BLACK : C_BRIGHT);
        }
        textf(x, 80, C_DIM, "%s %d HZ", in->ch == 2 ? "STEREO" : "MONO", (int)in->base_rate);
        textf(x, 90, C_DIM, "%.2f SEC", (double)in->frames / in->base_rate);
        /* waveform */
        int wx = 158, wy = 104, ww = 158, wh = 60;
        rect(wx, wy, ww, wh, C_ALT);
        for (int i = 0; i < ww; i++) {
            uint32_t a = (uint32_t)((uint64_t)i * in->frames / (uint32_t)ww), b = (uint32_t)((uint64_t)(i + 1) * in->frames / (uint32_t)ww);
            int lo = 0, hi = 0; uint32_t stp = (b - a) / 64 + 1;
            for (uint32_t k = a; k < b; k += stp) { int v = in->data[(size_t)k * in->ch]; if (v < lo) lo = v; if (v > hi) hi = v; }
            int y1 = wy + wh / 2 - hi * (wh / 2) / 32768, y2 = wy + wh / 2 - lo * (wh / 2) / 32768;
            rect(wx + i, y1, 1, y2 - y1 + 1, in->loop_len ? C_VOL : C_INS);
        }
    }
    textf(3, 172, C_DIM, "BANK %c", 'A' + bank);
    text(3, 184, "DATA: SELECT   PUSH: LOAD WAV   PADS: PLAY", C_DIM);
    text(3, 194, "+/-: PARAMETER   SHIFT+DATA: CHANGE IT", C_DIM);
    draw_info("SHIFT+ERASE REMOVES THE SAMPLE", C_DIM);
}

static void draw_song(void)
{
    PlayPos pp = pl_pos();
    text(3, 24, "POS PATTERN", C_DIM);
    int top = clampi(cur_ord - 7, 0, song.nord > 16 ? song.nord - 16 : 0);
    for (int i = 0; i < 16 && top + i < song.nord; i++) {
        int o = top + i, y = 35 + i * 10;
        if (o == cur_ord) rect(0, y - 1, 96, 10, song_focus == SF_ORDER ? (value_lock ? C_YELLOW : C_CURROW) : C_PANEL);
        if (pp.mode != PL_STOPPED && o == pp.ord) rect(0, y - 1, 3, 10, C_GREEN);
        textf(6, y, o == cur_ord && value_lock && song_focus == SF_ORDER ? C_BLACK : C_TEXT, "%02d", o);
        textf(30, y, o == cur_ord && value_lock && song_focus == SF_ORDER ? C_BLACK : C_BRIGHT, "%02d", song.order[o]);
        textf(54, y, C_DIM, "%2dR", song.pat[song.order[o]].rows);
    }
    rect(100, 23, 1, 186, C_PANEL);
    for (int f = 1; f < SF_N; f++) {
        int y = 30 + (f - 1) * 14 + (f >= SF_INSERT ? 10 : 0);
        if (f == song_focus) rect(106, y - 3, 210, 13, C_CURROW);
        text(110, y, sf_name[f], f == song_focus ? C_BRIGHT : C_TEXT);
        if (f == SF_BPM) textf(250, y, C_BRIGHT, "%d", song.bpm);
        if (f == SF_SPEED) textf(250, y, C_BRIGHT, "%d", song.speed);
        if (f == SF_ROWS) textf(250, y, C_BRIGHT, "%d", song.pat[cur_pat].rows);
        if (f >= SF_INSERT && f == song_focus) text(274, y, "PUSH", C_YELLOW);
    }
    textf(110, 140, C_DIM, "PATTERNS IN USE  %d", song_used_patterns(&song));
    text(110, 160, "+/-: MOVE BETWEEN ITEMS", C_DIM);
    text(110, 170, "DATA: CHANGE THE ITEM", C_DIM);
    text(110, 180, "SHIFT+DATA ON A POSITION:", C_DIM);
    text(110, 190, "  CHOOSE ITS PATTERN", C_DIM);
    draw_info(song_focus == SF_ORDER ? "POSITIONS PLAY TOP TO BOTTOM" : sf_name[song_focus], C_TEXT);
}

static void draw_files(void)
{
    const char *rel = br_dir + strlen(card);
    textf(3, 24, C_YELLOW, "%s", br_mode == BR_SAMPLE ? "LOAD SAMPLE" : "LOAD SONG");
    char p[40]; size_t l = strlen(rel);
    snprintf(p, sizeof p, "%s", l > 36 ? rel + l - 36 : (*rel ? rel : "/"));
    text(96, 24, p, C_DIM);
    int top = clampi(br_sel - 8, 0, br_n > 17 ? br_n - 17 : 0);
    for (int i = 0; i < 17 && top + i < br_n; i++) {
        int k = top + i, y = 36 + i * 10;
        if (k == br_sel) rect(0, y - 1, SCR_W, 10, C_CURROW);
        char nm[50]; snprintf(nm, sizeof nm, "%s%s", br[k].dir ? "[" : "", br[k].name);
        if (br[k].dir && strlen(nm) < sizeof nm - 1) strcat(nm, "]");
        text(6, y, nm, br[k].dir ? C_INS : C_BRIGHT);
    }
    if (!br_n) text(6, 40, br_mode == BR_SAMPLE ? "NO WAV FILES HERE" : "NO SONGS HERE (.MST OR .MOD)", C_DIM);
    draw_info("PUSH: OPEN  MINUS: FOLDER UP  STOP: CLOSE", C_DIM);
}

static void draw(void)
{
    rect(0, 0, SCR_W, SCR_H, C_BG);
    draw_header();
    switch (screen) {
    case SCR_PAT: draw_pattern(); break;
    case SCR_INS: draw_ins(); break;
    case SCR_SONG: draw_song(); break;
    case SCR_FILES: draw_files(); break;
    }
    draw_softkeys();
}

/* ------------------------------------------------------------------ LEDs ---- */
static void leds(void)
{
    PlayPos pp = pl_pos();
    int play = pp.mode != PL_STOPPED;
    surf_led(B_PLAY, play ? 2 : 0);
    surf_led(B_SEQREC, edit ? 2 : 0);
    surf_led(B_LOOP, loop_mode ? 2 : 0);
    surf_led(B_16LEV, pad_mode == PM_KEYS ? 2 : 0);
    surf_led(B_NOTEREP, pad_mode == PM_STEP ? 2 : 0);
    surf_led(B_MUTE, mute_held ? 2 : 0);
    surf_led(B_SEQ, screen == SCR_PAT ? 2 : 0);
    surf_led(B_SAMPLE, screen == SCR_INS ? 2 : 0);
    surf_led(B_SHIFT, shift ? 2 : 0);
    int lit = pad_mode == PM_KEYS && screen == SCR_PAT ? octave : bank;
    for (int i = 0; i < 8; i++) surf_led(CC_BANK_A + i, i == lit ? 2 : 0);
    for (int i = 0; i < 16; i++) {
        int r = 0, g = 0, b = 0;
        if (mute_held) { if (i < NTRK) { if (song.mute[i]) r = 100; else g = 90; } }
        else if (screen == SCR_FILES) { }
        else if (screen == SCR_PAT && pad_mode == PM_STEP) {
            int row = cur_row / 16 * 16 + i;
            if (row < song.pat[cur_pat].rows) {
                const Cell *c = &song.pat[cur_pat].c[row][cur_trk];
                if (c->note) { r = 127; g = 60; } else if (i % 4 == 0) { r = g = b = 6; } else { b = 3; }
                if (play && pp.pat == cur_pat && pp.row == row) { r = g = b = 127; }
            }
        } else if (screen == SCR_PAT && edit && cur_fld >= F_VOL) {
            const Cell *c = &song.pat[cur_pat].c[cur_row][cur_trk];
            int v = cur_fld == F_VOL ? (c->vol ? (c->vol - 1) * 15 / 64 : -1) : cur_fld == F_FX ? c->fx : c->prm / 17;
            if (cur_fld == F_VOL) { g = 10 + i * 4; } else { r = 30; g = 12; }
            if (i == v) { r = g = b = 127; }
        } else if (screen == SCR_PAT && pad_mode == PM_KEYS && !(edit && cur_fld == F_INS)) {
            int n = (12 * octave + i) % 12;
            int black = n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
            if (n == 0) { g = 90; b = 127; } else if (black) { b = 10; } else { r = 20; g = 20; b = 50; }
        } else {
            int n = bank * 16 + i + 1;
            if (song.ins[n].data) { r = 70; g = 35; }
            if (n == cur_ins) { r = 127; g = 127; b = 110; }
        }
        surf_pad_rgb(i, r, g, b);
    }
}

/* ------------------------------------------------------------------ main ---- */
void ui_render_audio(float *out, int frames)
{
    pl_lock();
    pl_render(out, frames);
    pl_unlock();
}

void ui_request_quit(void) { quit_req = 1; }

static int first_mod(char *out, size_t n)       /* the alphabetically first .mod in tracker/songs */
{
    char dir[600], best[256] = "";
    snprintf(dir, sizeof dir, "%s/tracker/songs", card);
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)))
        if (de->d_name[0] != '.' && strlen(de->d_name) < sizeof best && has_ext(de->d_name, ".mod") && (!best[0] || strcasecmp(de->d_name, best) < 0))
            snprintf(best, sizeof best, "%s", de->d_name);
    closedir(d);
    if (!best[0]) return -1;
    snprintf(out, n, "%s/%s", dir, best);
    return 0;
}

void ui_init(const char *card_dir)
{
    snprintf(card, sizeof card, "%s", card_dir);
    size_t l = strlen(card);
    while (l > 1 && card[l - 1] == '/') card[--l] = 0;
    char path[600], err[64];
    snprintf(path, sizeof path, "%s/tracker/songs/AUTOSAVE.mst", card);
    if (song_load(&song, path, err, sizeof err) == 0) hw_log("continuing from %s", path);
    else {
        /* first run: the first .mod in the card's songs folder if there is one, otherwise the built-in kit */
        if (first_mod(path, sizeof path) == 0 && mod_import(&song, path, err, sizeof err) == 0) { hw_log("demo song %s", path); loop_mode = 0; }
        else { song_init(&song); song_demo(&song); }
    }
    pl_init(&song);
    pl_set_loop_pattern(loop_mode);
    cur_pat = song.order[0];
}

void ui_run(void)
{
    uint32_t last_vu = hw_ms();
    while (!quit_req) {
        Event e;
        int wait = 30;
        while (ev_pop(&e, wait)) {
            wait = 0;
            switch (e.type) {
            case EV_PAD_ON: pad_on(&e); break;
            case EV_PAD_OFF: pad_off(&e); break;
            case EV_BTN_ON: button(e.a, 1); break;
            case EV_BTN_OFF: button(e.a, 0); break;
            case EV_ENC: encoder(e.a); break;
            case EV_KNOB: knob(e.a, e.b); break;
            }
        }
        PlayPos pp = pl_pos();
        if (pp.mode != PL_STOPPED) {
            cur_pat = pp.pat;
            if (pp.mode == PL_SONG) cur_ord = pp.ord;
            cur_row = pp.row;                      /* the view follows the playhead; notes played in WRITE mode land on it */
        }
        cur_row = clampi(cur_row, 0, song.pat[cur_pat].rows - 1);
        uint32_t now = hw_ms();
        if (now - last_vu >= 30) {
            last_vu = now;
            for (int t = 0; t < NTRK; t++) { float v = pl_vu(t), d = vu[t] * 0.85f; vu[t] = v > d ? v : d; if (vu[t] < 0.004f) vu[t] = 0; }
        }
        if (screen == SCR_FILES && br_mode == BR_SAMPLE && br_previewed != br_sel && now - br_moved_at > 300) { br_previewed = br_sel; br_preview(); }
        draw();
        scr_present();
        leds();
    }
    pl_stop();
    autosave();
}
