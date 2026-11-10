/* Song model, file formats and the built-in demo kit. MIT licence, see LICENSE. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "song.h"

static int fail(char *err, int errlen, const char *msg)
{
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "%s", msg);
    return -1;
}

double note_period(int note) { return 428.0 * pow(2.0, (REF_NOTE - note) / 12.0); }

void note_name(int note, char out[4])
{
    static const char *nm = "C-C#D-D#E-F-F#G-G#A-A#B-";
    if (note == NOTE_CUT) { memcpy(out, "^^^", 4); return; }
    if (note < NOTE_MIN || note > NOTE_MAX) { memcpy(out, "---", 4); return; }
    int n = note - 1;
    out[0] = nm[(n % 12) * 2]; out[1] = nm[(n % 12) * 2 + 1]; out[2] = (char)('0' + n / 12); out[3] = 0;
}

void ins_update_rate(Ins *in)
{
    in->rate = in->base_rate * (float)pow(2.0, (in->tune + in->fine / 100.0) / 12.0);
}

void ins_free(Ins *in)
{
    free(in->data);
    memset(in, 0, sizeof *in);
}

void song_init(Song *s)
{
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "UNTITLED");
    s->bpm = 125; s->speed = 6; s->nord = 1;
    for (int p = 0; p < MAXPAT; p++) s->pat[p].rows = MAXROWS;
    for (int t = 0; t < NTRK; t++) s->pan[t] = 128;
}

void song_free(Song *s)
{
    for (int i = 1; i <= NINS; i++) free(s->ins[i].data);
    memset(s->ins, 0, sizeof s->ins);
}

int pattern_empty(const Pattern *p)
{
    for (int r = 0; r < MAXROWS; r++)
        for (int t = 0; t < NTRK; t++) {
            const Cell *c = &p->c[r][t];
            if (c->note || c->ins || c->vol || c->fx || c->prm) return 0;
        }
    return 1;
}

int song_used_patterns(const Song *s)
{
    int hi = 0;
    for (int i = 0; i < s->nord; i++) if (s->order[i] > hi) hi = s->order[i];
    for (int p = MAXPAT - 1; p > hi; p--) if (!pattern_empty(&s->pat[p])) { hi = p; break; }
    return hi + 1;
}

/* ------------------------------------------------------------------ WAV ---- */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void name_from_path(char *dst, size_t n, const char *path)
{
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    size_t i = 0;
    for (; b[i] && b[i] != '.' && i < n - 1; i++) dst[i] = (char)toupper((unsigned char)b[i]);
    if (b[i] && b[i] != '.' && i == n - 1) { /* truncated */ }
    dst[i] = 0;
}

int wav_load_max(const char *path, Ins *out, uint32_t max_frames, char *err, int errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return fail(err, errlen, "CANNOT OPEN FILE");
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return fail(err, errlen, "NOT A WAV FILE"); }
    int fmt = 0, ch = 0, bits = 0, have_fmt = 0;
    uint32_t rate = 0, loop_start = 0, loop_end = 0; int have_loop = 0;
    int16_t *data = NULL; uint32_t frames = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t len = rd32(ck + 4);
        long next = ftell(f) + (long)len + (long)(len & 1);
        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t b[40] = {0};
            size_t want = len < sizeof b ? len : sizeof b;
            if (fread(b, 1, want, f) != want || len < 16) break;
            fmt = rd16(b); ch = rd16(b + 2); rate = rd32(b + 4); bits = rd16(b + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(b + 24);
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4) && have_fmt && !data) {
            int bytes = bits / 8;
            if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32)) || ch < 1 || ch > 8 || rate < 1000) {
                fclose(f); return fail(err, errlen, "UNSUPPORTED WAV FORMAT");
            }
            uint32_t total = len / (uint32_t)(bytes * ch);
            if (total > max_frames) total = max_frames;
            if (total < 2) { fclose(f); return fail(err, errlen, "EMPTY WAV FILE"); }
            int och = ch >= 2 ? 2 : 1;
            data = malloc(sizeof(int16_t) * ((size_t)total + 1) * (size_t)och);
            uint8_t *raw = malloc((size_t)total * (size_t)(bytes * ch));
            if (!data || !raw) { free(data); free(raw); fclose(f); return fail(err, errlen, "OUT OF MEMORY"); }
            size_t got = fread(raw, (size_t)(bytes * ch), total, f);
            total = (uint32_t)got;
            for (uint32_t i = 0; i < total; i++)
                for (int c = 0; c < och; c++) {
                    const uint8_t *p = raw + ((size_t)i * (size_t)ch + (size_t)c) * (size_t)bytes;
                    int v;
                    if (fmt == 3) { float x; memcpy(&x, p, 4); if (x > 1) x = 1; else if (x < -1) x = -1; v = (int)(x * 32767.0f); }
                    else if (bits == 8) v = (p[0] - 128) << 8;
                    else if (bits == 16) v = (int16_t)rd16(p);
                    else if (bits == 24) v = (int16_t)rd16(p + 1);
                    else v = (int16_t)rd16(p + 2);
                    data[(size_t)i * (size_t)och + (size_t)c] = (int16_t)v;
                }
            for (int c = 0; c < och; c++) data[(size_t)total * (size_t)och + (size_t)c] = 0;
            free(raw);
            frames = total; ch = och;
        } else if (!memcmp(ck, "smpl", 4) && len >= 60) {
            uint8_t b[60];
            if (fread(b, 1, 60, f) == 60 && rd32(b + 28) >= 1) { loop_start = rd32(b + 44); loop_end = rd32(b + 48); have_loop = 1; }
        }
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    fclose(f);
    if (!data || frames < 2) { free(data); return fail(err, errlen, "NO AUDIO IN WAV FILE"); }
    memset(out, 0, sizeof *out);
    name_from_path(out->name, sizeof out->name, path);
    out->data = data; out->frames = frames; out->ch = (uint8_t)ch; out->vol = 64;
    out->base_rate = (float)rate;
    if (have_loop && loop_end > loop_start && loop_end < frames) { out->loop_start = loop_start; out->loop_len = loop_end - loop_start + 1; }
    ins_update_rate(out);
    return 0;
}

int wav_load(const char *path, Ins *out, char *err, int errlen)
{
    return wav_load_max(path, out, MAX_SAMPLE_FRAMES, err, errlen);
}

size_t song_sample_bytes(const Song *s)
{
    size_t n = 0;
    for (int i = 1; i <= NINS; i++) n += (size_t)s->ins[i].frames * s->ins[i].ch * sizeof(int16_t);
    return n;
}

/* --------------------------------------------------------- native format ---- */
static const char MAGIC[8] = { 'M', 'P', 'C', 'T', 'R', 'K', '1', 0 }, MAGIC2[8] = { 'M', 'P', 'C', 'T', 'R', 'K', '2', 0 };

static void w8(FILE *f, unsigned v) { fputc((int)(v & 0xFF), f); }
static void w16(FILE *f, unsigned v) { w8(f, v); w8(f, v >> 8); }
static void w32(FILE *f, uint32_t v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }
static unsigned r8(FILE *f) { int c = fgetc(f); return c < 0 ? 0 : (unsigned)c; }
static unsigned r16(FILE *f) { unsigned a = r8(f); return a | r8(f) << 8; }
static uint32_t r32(FILE *f) { uint32_t a = r16(f); return a | (uint32_t)r16(f) << 16; }

int song_save(const Song *s, const char *path, char *err, int errlen)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return fail(err, errlen, "CANNOT WRITE TO CARD");
    fwrite(MAGIC2, 1, 8, f);
    fwrite(s->name, 1, 24, f);
    w8(f, s->bpm); w8(f, s->speed); w16(f, s->nord); w8(f, s->pt_mode);
    fwrite(s->order, 1, s->nord, f);
    fwrite(s->pan, 1, NTRK, f); fwrite(s->mute, 1, NTRK, f);
    int np = song_used_patterns(s);
    w8(f, (unsigned)np);
    for (int p = 0; p < np; p++) {
        const Pattern *pt = &s->pat[p];
        w8(f, pt->rows);
        fwrite(pt->c, sizeof(Cell), (size_t)pt->rows * NTRK, f);
    }
    for (int i = 1; i <= NINS; i++) {
        const Ins *in = &s->ins[i];
        w8(f, in->data ? 1 : 0);
        if (!in->data) continue;
        fwrite(in->name, 1, 24, f);
        w8(f, in->ch); w8(f, in->vol); w8(f, (uint8_t)in->tune); w8(f, (uint8_t)in->fine);
        uint32_t rate; memcpy(&rate, &in->base_rate, 4); w32(f, rate);
        w32(f, in->frames); w32(f, in->loop_start); w32(f, in->loop_len);
        fwrite(in->data, sizeof(int16_t), (size_t)in->frames * in->ch, f);   /* the device and the Mac are little-endian */
    }
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (bad) { remove(tmp); return fail(err, errlen, "WRITE FAILED (CARD FULL?)"); }
    if (rename(tmp, path) != 0) { remove(tmp); return fail(err, errlen, "CANNOT REPLACE SONG FILE"); }
    return 0;
}

int song_load(Song *s, const char *path, char *err, int errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return fail(err, errlen, "CANNOT OPEN FILE");
    char magic[8];
    int v2 = fread(magic, 1, 8, f) == 8 && !memcmp(magic, MAGIC2, 8);
    if (!v2 && memcmp(magic, MAGIC, 8)) { fclose(f); return fail(err, errlen, "NOT A TRACKER SONG"); }
    song_init(s);
    const char *why = "SONG FILE IS DAMAGED";
    size_t budget = 0;
    if (fread(s->name, 1, 24, f) != 24) goto bad;
    s->name[23] = 0;
    s->bpm = (uint8_t)r8(f); s->speed = (uint8_t)r8(f); s->nord = (uint16_t)r16(f);
    if (v2) s->pt_mode = (uint8_t)r8(f);
    if (s->nord < 1 || s->nord > MAXORD || s->bpm < 32 || s->speed < 1) goto bad;
    if (fread(s->order, 1, s->nord, f) != s->nord) goto bad;
    for (int i = 0; i < s->nord; i++) if (s->order[i] >= MAXPAT) s->order[i] = 0;
    if (fread(s->pan, 1, NTRK, f) != NTRK || fread(s->mute, 1, NTRK, f) != NTRK) goto bad;
    int np = (int)r8(f);
    if (np > MAXPAT) goto bad;
    for (int p = 0; p < np; p++) {
        Pattern *pt = &s->pat[p];
        pt->rows = (uint8_t)r8(f);
        if (pt->rows < 1 || pt->rows > MAXROWS) goto bad;
        if (fread(pt->c, sizeof(Cell), (size_t)pt->rows * NTRK, f) != (size_t)pt->rows * NTRK) goto bad;
    }
    for (int i = 1; i <= NINS; i++) {
        Ins *in = &s->ins[i];
        if (!r8(f)) continue;
        if (fread(in->name, 1, 24, f) != 24) goto bad;
        in->name[23] = 0;
        in->ch = (uint8_t)r8(f); in->vol = (uint8_t)r8(f); in->tune = (int8_t)r8(f); in->fine = (int8_t)r8(f);
        uint32_t rate = r32(f); memcpy(&in->base_rate, &rate, 4);
        in->frames = r32(f); in->loop_start = r32(f); in->loop_len = r32(f);
        if (in->ch < 1 || in->ch > 2 || in->frames < 2 || in->frames > MAX_SAMPLE_FRAMES || in->vol > 64 || !(in->base_rate >= 1000.0f && in->base_rate <= 400000.0f)) goto bad;
        budget += (size_t)in->frames * in->ch * sizeof(int16_t);
        if (budget > MAX_SONG_BYTES) { why = "SONG TOO BIG FOR MEMORY"; goto bad; }
        if (in->loop_len && (uint64_t)in->loop_start + in->loop_len > in->frames) in->loop_len = 0;
        in->data = malloc(sizeof(int16_t) * ((size_t)in->frames + 1) * in->ch);
        if (!in->data) goto bad;
        if (fread(in->data, sizeof(int16_t), (size_t)in->frames * in->ch, f) != (size_t)in->frames * in->ch) goto bad;
        for (int c = 0; c < in->ch; c++) in->data[(size_t)in->frames * in->ch + (size_t)c] = 0;
        ins_update_rate(in);
    }
    fclose(f);
    return 0;
bad:
    fclose(f);
    song_free(s);
    song_init(s);
    return fail(err, errlen, why);
}

/* ------------------------------------------------------------ MOD import ---- */
int mod_import(Song *s, const char *path, char *err, int errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return fail(err, errlen, "CANNOT OPEN FILE");
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size < 1084 || size > 64L * 1024 * 1024) { fclose(f); return fail(err, errlen, "NOT A MOD FILE"); }
    uint8_t *d = malloc((size_t)size);
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) { free(d); fclose(f); return fail(err, errlen, "CANNOT READ FILE"); }
    fclose(f);
    const char *sig = (const char *)d + 1080;
    int nch = 0;
    if (!memcmp(sig, "M.K.", 4) || !memcmp(sig, "M!K!", 4) || !memcmp(sig, "FLT4", 4) || !memcmp(sig, "M&K!", 4)) nch = 4;
    else if (isdigit((unsigned char)sig[0]) && !memcmp(sig + 1, "CHN", 3)) nch = sig[0] - '0';
    else if (isdigit((unsigned char)sig[0]) && isdigit((unsigned char)sig[1]) && !memcmp(sig + 2, "CH", 2)) nch = (sig[0] - '0') * 10 + (sig[1] - '0');
    if (nch < 1) { free(d); return fail(err, errlen, "NOT A 31-SAMPLE MOD FILE"); }
    if (nch > NTRK) { free(d); return fail(err, errlen, "MOD HAS MORE THAN 8 CHANNELS"); }
    song_init(s);
    s->pt_mode = 1;
    for (int i = 0; i < 20; i++) { int c = d[i]; s->name[i] = (char)(c >= 32 && c < 127 ? toupper(c) : (c ? ' ' : 0)); }
    s->name[20] = 0;
    if (!s->name[0]) name_from_path(s->name, sizeof s->name, path);
    int nord = d[950]; if (nord < 1) nord = 1; if (nord > 128) nord = 128;
    int npat = 0;
    for (int i = 0; i < 128; i++) if (d[952 + i] > npat) npat = d[952 + i];
    npat++;
    if (npat > MAXPAT) { free(d); song_init(s); return fail(err, errlen, "MOD HAS TOO MANY PATTERNS"); }
    s->nord = (uint16_t)nord;
    for (int i = 0; i < nord; i++) s->order[i] = d[952 + i];
    static const uint8_t amiga_pan[4] = { 64, 192, 192, 64 };
    for (int t = 0; t < NTRK; t++) s->pan[t] = amiga_pan[t & 3];
    size_t off = 1084;
    for (int p = 0; p < npat; p++) {
        Pattern *pt = &s->pat[p];
        for (int r = 0; r < 64; r++)
            for (int t = 0; t < nch; t++, off += 4) {
                if (off + 4 > (size_t)size) continue;
                const uint8_t *b = d + off;
                Cell *c = &pt->c[r][t];
                int period = (b[0] & 0x0F) << 8 | b[1];
                c->ins = (uint8_t)((b[0] & 0xF0) | b[2] >> 4);
                if (c->ins > 31) c->ins = 0;
                c->fx = b[2] & 0x0F; c->prm = b[3];
                if (period > 0) {
                    int n = (int)lround(REF_NOTE + 12.0 * log2(428.0 / period));
                    c->note = (uint8_t)(n < NOTE_MIN ? NOTE_MIN : n > NOTE_MAX ? NOTE_MAX : n);
                }
            }
    }
    for (int i = 1; i <= 31; i++) {
        const uint8_t *h = d + 20 + (i - 1) * 30;
        uint32_t len = (uint32_t)(h[22] << 8 | h[23]) * 2;
        int ft = h[24] & 0x0F; if (ft > 7) ft -= 16;
        int vol = h[25] > 64 ? 64 : h[25];
        uint32_t ls = (uint32_t)(h[26] << 8 | h[27]) * 2, ll = (uint32_t)(h[28] << 8 | h[29]) * 2;
        if (off >= (size_t)size) len = 0;
        if (off + len > (size_t)size) len = (uint32_t)((size_t)size - off);
        if (len >= 4) {
            Ins *in = &s->ins[i];
            for (int k = 0; k < 22; k++) { int c = h[k]; in->name[k] = (char)(c >= 32 && c < 127 ? toupper(c) : (c ? ' ' : 0)); }
            in->name[22] = 0;
            in->data = malloc(sizeof(int16_t) * ((size_t)len + 1));
            if (!in->data) { free(d); song_free(s); song_init(s); return fail(err, errlen, "OUT OF MEMORY"); }
            for (uint32_t k = 0; k < len; k++) in->data[k] = (int16_t)((int8_t)d[off + k] * 256);
            in->data[0] = in->data[1] = 0;       /* the first word of a MOD sample is the idle loop, not audio */
            in->data[len] = 0;
            in->frames = len; in->ch = 1; in->vol = (uint8_t)vol;
            in->base_rate = (float)(8287.14 * pow(2.0, ft / 96.0));
            if (ll > 2 && ls + ll <= len) { in->loop_start = ls; in->loop_len = ll; }
            else if (ll > 2 && ls < len) { in->loop_start = ls; in->loop_len = len - ls; }
            ins_update_rate(in);
        }
        off += len;
    }
    free(d);
    return 0;
}

/* -------------------------------------------------------------- demo kit ---- */
#define SR 44100
static uint32_t rng_state = 0x1234567u;
static float frand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)(int32_t)rng_state / 2147483648.0f;
}

static Ins *mk(Song *s, int n, const char *name, float secs)
{
    Ins *in = &s->ins[n];
    ins_free(in);
    snprintf(in->name, sizeof in->name, "%s", name);
    in->frames = (uint32_t)(secs * SR); in->ch = 1; in->vol = 64; in->base_rate = SR;
    in->data = calloc((size_t)in->frames + 1, sizeof(int16_t));
    ins_update_rate(in);
    return in;
}

static void put(Ins *in, uint32_t i, float x)
{
    if (x > 1) x = 1; else if (x < -1) x = -1;
    in->data[i] = (int16_t)(x * 32000.0f);
}

static void mk_cycle(Song *s, int n, const char *name, int shape)
{
    const int N = 256;
    Ins *in = mk(s, n, name, (float)N / SR);
    in->frames = N; in->loop_start = 0; in->loop_len = N; in->vol = 40;
    in->base_rate = 261.6256f * N;               /* one cycle per 256 frames = C-4 at the reference note */
    for (int i = 0; i < N; i++) {
        float ph = (float)i / N, x = 0;
        if (shape == 0) for (int h = 1; h <= 24; h++) x += sinf(6.2831853f * ph * (float)h) / (float)h * 0.55f;                          /* saw */
        else if (shape == 1) for (int h = 1; h <= 23; h += 2) x += sinf(6.2831853f * ph * (float)h) / (float)h * 0.9f;                 /* square */
        else if (shape == 2) x = ph < 0.25f ? ph * 4 : ph < 0.75f ? 2 - ph * 4 : ph * 4 - 4;                                          /* triangle */
        else x = sinf(6.2831853f * ph);
        put(in, (uint32_t)i, x * 0.8f);
    }
    ins_update_rate(in);
}

static void pluck_into(float *buf, uint32_t n, float freq, float gain)
{
    int L = (int)(SR / freq + 0.5f);
    float *dl = malloc(sizeof(float) * (size_t)L);
    for (int i = 0; i < L; i++) dl[i] = frand();
    int k = 0; float prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        float x = dl[k];
        float y = (x + prev) * 0.4985f;
        prev = x; dl[k] = y; k = (k + 1) % L;
        buf[i] += x * gain;
    }
    free(dl);
}

void song_demo(Song *s)
{
    song_free(s);
    song_init(s);
    snprintf(s->name, sizeof s->name, "DEMO");
    rng_state = 0x1234567u;
    Ins *in;
    in = mk(s, 1, "KICK", 0.40f);
    { double ph = 0; for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR; float fq = 45 + 140 * expf(-t * 28); ph += 6.2831853 * fq / SR; put(in, i, sinf((float)ph) * expf(-t * 7.5f) + (i < 60 ? frand() * 0.3f : 0)); } }
    in = mk(s, 2, "SNARE", 0.28f);
    { float lp = 0; for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR, nz = frand(); lp += (nz - lp) * 0.55f; put(in, i, lp * expf(-t * 17) * 0.8f + sinf(6.2831853f * 185 * t) * expf(-t * 24) * 0.6f); } }
    in = mk(s, 3, "CL HAT", 0.07f);
    { float pv = 0; for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR, nz = frand(); put(in, i, (nz - pv) * 0.6f * expf(-t * 75)); pv = nz; } }
    in->vol = 44;
    in = mk(s, 4, "OP HAT", 0.40f);
    { float pv = 0; for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR, nz = frand(); put(in, i, (nz - pv) * 0.55f * expf(-t * 9)); pv = nz; } }
    in->vol = 40;
    in = mk(s, 5, "CLAP", 0.30f);
    { float lp = 0, pv = 0; for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR, nz = frand(); lp += (nz - lp) * 0.4f; float bp = lp - pv; pv = lp * 0.9f; float e = t < 0.03f ? expf(-fmodf(t, 0.01f) * 300) : expf(-(t - 0.03f) * 22); put(in, i, bp * e * 1.6f); } }
    in = mk(s, 6, "TOM", 0.40f);
    { double ph = 0; for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR; ph += 6.2831853 * (90 + 110 * expf(-t * 12)) / SR; put(in, i, sinf((float)ph) * expf(-t * 8) * 0.9f); } }
    in = mk(s, 7, "RIM", 0.06f);
    for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR; put(in, i, (sinf(6.2831853f * 1720 * t) + sinf(6.2831853f * 1090 * t)) * 0.45f * expf(-t * 90)); }
    in = mk(s, 8, "COWBELL", 0.30f);
    for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR; float a = sinf(6.2831853f * 540 * t) > 0 ? 1.0f : -1.0f, b = sinf(6.2831853f * 800 * t) > 0 ? 1.0f : -1.0f; put(in, i, (a + b) * 0.25f * expf(-t * 14)); }
    mk_cycle(s, 9, "SAW", 0);
    mk_cycle(s, 10, "SQUARE", 1);
    mk_cycle(s, 11, "TRIANGLE", 2);
    mk_cycle(s, 12, "SINE", 3);
    in = mk(s, 13, "PLUCK", 1.5f);
    { float *b = calloc(in->frames, sizeof(float)); pluck_into(b, in->frames, 261.6256f, 0.8f); for (uint32_t i = 0; i < in->frames; i++) put(in, i, b[i] * (i < 40 ? i / 40.0f : 1)); free(b); }
    in = mk(s, 14, "MINOR STAB", 1.2f);
    { float *b = calloc(in->frames, sizeof(float)); pluck_into(b, in->frames, 261.6256f, 0.4f); pluck_into(b, in->frames, 311.127f, 0.4f); pluck_into(b, in->frames, 391.995f, 0.4f);
      for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR; put(in, i, b[i] * expf(-t * 2.5f)); } free(b); }
    in = mk(s, 15, "SUB BASS", 0.60f);
    { for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR; float e = t < 0.005f ? t / 0.005f : expf(-(t - 0.005f) * 4); put(in, i, (sinf(6.2831853f * 261.6256f * t) + 0.25f * sinf(6.2831853f * 523.25f * t)) * e * 0.8f); } }
    in = mk(s, 16, "NOISE", 0.50f);
    { float lp = 0; for (uint32_t i = 0; i < in->frames; i++) { float t = (float)i / SR; lp += (frand() - lp) * (0.05f + 0.9f * t / 0.5f); put(in, i, lp * (1 - t / 0.5f) * 0.7f); } }
    in->vol = 36;

    /* a 4-bar groove in A minor: C-4 is note 49, so A-1 = 22, A-2 = 34 ... */
    Pattern *p = &s->pat[0];
    #define SET(r, t, n, i) do { p->c[r][t].note = (uint8_t)(n); p->c[r][t].ins = (uint8_t)(i); } while (0)
    static const int bass[4] = { 22, 22, 18, 20 };              /* A-1 A-1 F-1 G-1 */
    static const int lead[16] = { 58, 0, 61, 0, 65, 0, 61, 63, 58, 0, 61, 0, 68, 65, 63, 61 };
    for (int pi = 0; pi < 2; pi++) {
        p = &s->pat[pi];
        for (int r = 0; r < 64; r++) {
            int bar = r / 16, st = r % 16;
            if (st % 4 == 0) SET(r, 0, REF_NOTE, 1);
            if (st == 14 && bar == 3) SET(r, 0, REF_NOTE, 1);
            if (st % 8 == 4) SET(r, 1, REF_NOTE, pi ? 5 : 2);
            if (st == 15 && bar % 2 == 1) { SET(r, 1, REF_NOTE, 2); p->c[r][1].vol = 25; }
            if (st % 2 == 0) { SET(r, 2, REF_NOTE, st % 4 == 2 ? 4 : 3); if (st % 4 == 0) p->c[r][2].vol = 33; }
            if (st == 0 || st == 3 || st == 6 || st == 10 || st == 12) { SET(r, 3, bass[bar] + 12 + (st == 10 ? 12 : 0), 15); }
            if (pi == 1 && lead[(r / 2) % 16] && r % 2 == 0) { SET(r, 4, lead[(r / 2) % 16], 13); p->c[r][4].vol = 41; }
            if (st == 0) { SET(r, 5, bass[bar] + 24, 14); p->c[r][5].vol = 31; }
        }
    }
    #undef SET
    s->pan[2] = 160; s->pan[4] = 96; s->pan[5] = 150;
    s->nord = 4; s->order[0] = 0; s->order[1] = 1; s->order[2] = 0; s->order[3] = 1;
}
