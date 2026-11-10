/* Replayer and mixer. MIT licence, see LICENSE. */
#ifndef PLAYER_H
#define PLAYER_H
#include "song.h"

#define MIX_RATE 44100
#define NJAM     16                 /* extra voices for pads played by hand, one per pad */

enum { PL_STOPPED = 0, PL_PATTERN = 1, PL_SONG = 2 };

typedef struct { int mode, ord, pat, row; float frac; int speed, bpm; } PlayPos;   /* frac: how far through the row, 0..1; speed and bpm are the live values */

void pl_init(Song *s);
void pl_lock(void);
void pl_unlock(void);

/* Call with the lock held (the audio thread does). Writes interleaved stereo floats. */
void pl_render(float *out, int frames);

/* These take the lock themselves. */
void pl_play(int mode, int ord, int pat, int row);
void pl_stop(void);
void pl_set_loop_pattern(int on);          /* while playing a song: stay in the current pattern */
void pl_jam_on(int key, int ins, int note, int vol);   /* vol 0..64 */
void pl_jam_off(int key);
void pl_skip_once(int pat, int row, int trk);          /* do not trigger this cell the next time it plays */
void pl_ins_changed(int ins);              /* stop every voice that uses this instrument (lock held by caller) */
void pl_stinger(const int16_t *data, uint32_t frames, float rate);   /* one-shot mono sound on its own voice; data must stay valid until pl_stinger_stop */
void pl_stinger_stop(void);
void pl_all_off(void);                     /* lock held by caller */

/* Lock-free reads for the UI. */
PlayPos pl_pos(void);
float   pl_vu(int trk);                    /* peak since the last call, 0..1 */
void    pl_set_master(float gain);
#endif
