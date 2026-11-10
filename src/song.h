/* Song data model for the MPC Sample tracker. MIT licence, see LICENSE. */
#ifndef SONG_H
#define SONG_H
#include <stddef.h>
#include <stdint.h>

#define NTRK      8
#define MAXROWS   64
#define MAXPAT    100
#define MAXORD    200
#define NINS      64            /* instruments are numbered 1..64: bank A-D, pad 1-16 */
#define NOTE_NONE 0
#define NOTE_MIN  1             /* C-0 */
#define NOTE_MAX  120           /* B-9 */
#define NOTE_CUT  254
#define REF_NOTE  49            /* C-4: a sample plays at its own rate */
#define MAX_SAMPLE_FRAMES (44100u * 120u)
#define MAX_SONG_BYTES (400u * 1024u * 1024u)    /* all samples together; the device has 2 GB and no swap */

typedef struct { uint8_t note, ins, vol, fx, prm; } Cell;   /* vol: 0 = none, 1..65 = volume 0..64 */
typedef struct { uint8_t rows; Cell c[MAXROWS][NTRK]; } Pattern;

typedef struct {
    char     name[24];
    int16_t *data;              /* interleaved when ch == 2; NULL = empty slot */
    uint32_t frames;
    uint8_t  ch;                /* 1 or 2 */
    uint8_t  vol;               /* default volume 0..64 */
    int8_t   tune, fine;        /* semitones, cents */
    float    base_rate;         /* sample rate of the data */
    float    rate;              /* base_rate with tune and fine applied: the rate at C-4 */
    uint32_t loop_start, loop_len;   /* loop_len 0 = one-shot */
} Ins;

typedef struct {
    char     name[24];
    uint8_t  bpm, speed;
    uint16_t nord;
    uint8_t  pt_mode;           /* ProTracker behaviour: integer period table, slides stop at 113..856, no effect memory for 1xx 2xx Axy */
    uint8_t  order[MAXORD];
    uint8_t  pan[NTRK];         /* 0 left .. 128 centre .. 255 right */
    uint8_t  mute[NTRK];
    Pattern  pat[MAXPAT];
    Ins      ins[NINS + 1];
} Song;

double note_period(int note);                 /* Amiga-style period; 428 at C-4 */
void   ins_update_rate(Ins *in);
void   ins_free(Ins *in);
void   song_init(Song *s);                    /* empty song, one 64-row pattern */
void   song_free(Song *s);
void   song_demo(Song *s);                    /* built-in synthesised kit and a short groove */
int    song_used_patterns(const Song *s);     /* highest pattern that is in the order list or not empty, plus 1 */
int    pattern_empty(const Pattern *p);

/* All loaders return 0 on success, -1 on failure with a short message in err. */
int wav_load(const char *path, Ins *out, char *err, int errlen);
int wav_load_max(const char *path, Ins *out, uint32_t max_frames, char *err, int errlen);   /* reads at most max_frames */
size_t song_sample_bytes(const Song *s);
int song_save(const Song *s, const char *path, char *err, int errlen);
int song_load(Song *s, const char *path, char *err, int errlen);      /* native .mst */
int mod_import(Song *s, const char *path, char *err, int errlen);     /* 4 to 8 channel .mod */

void note_name(int note, char out[4]);        /* "C-4", "^^^", "---" */
#endif
