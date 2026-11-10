/* Hardware layer for the Akai MPC Sample: screen (DRM), control surface (raw MIDI), audio (ALSA PCM).
 * The stock application must be stopped: this program owns all three devices. MIT licence. */
#ifndef HW_H
#define HW_H
#include <stdint.h>

#define SCR_W 320
#define SCR_H 240

void hw_log(const char *fmt, ...);
uint32_t hw_ms(void);

/* Screen. Draw into scr_fb (XRGB8888, SCR_W * SCR_H), then call scr_present: it sends only
 * the rows that changed, because every update is an SPI transfer on the real panel. */
extern uint32_t scr_fb[SCR_W * SCR_H];
int  scr_open(void);
void scr_present(void);
void scr_close(void);

/* Control surface. The callback runs on the reader thread with one complete MIDI message. */
int  surf_open(void (*on_message)(const uint8_t *msg, int len));
void surf_pad_rgb(int pad, int r, int g, int b);     /* 0..127 each; only sent when it changes */
void surf_led(int cc, int value);                    /* button LED; only sent when it changes */
void surf_request_knobs(void);                       /* ask the control surface to report where its knobs are (the request the stock app sends at start) */
void surf_close(void);

/* Audio. The callback runs on a real-time thread and fills interleaved stereo floats. */
int  aud_open(void (*render)(float *out, int frames));
void aud_close(void);
unsigned aud_underruns(void);
#endif
