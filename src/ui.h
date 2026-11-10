/* MIT licence, see LICENSE. */
#ifndef UI_H
#define UI_H
#include <stdint.h>
void ui_init(const char *card_dir);            /* loads the autosave or the demo song */
void ui_midi(const uint8_t *msg, int len);     /* control-surface reader thread */
void ui_render_audio(float *out, int frames);  /* audio thread */
void ui_run(void);                             /* returns when the user quits; autosaves */
void ui_request_quit(void);
#endif
