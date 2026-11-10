/*
 * Hardware layer for the Akai MPC Sample (AC50), stock application stopped.
 *
 * Screen:  /dev/dri/card0 as DRM master, one 320x240 XRGB8888 dumb buffer on the CRTC. The
 *          panel is an SPI display that only updates on DIRTYFB and a full-screen update
 *          takes tens of milliseconds, so scr_present compares the new frame with the one
 *          on the panel and sends only the bands of rows that differ.
 * Surface: raw MIDI device of the card whose id contains "Surface". Pads are on channel 10,
 *          buttons and knobs on channel 1. Pad colours are SysEx, button LEDs are CCs.
 * Audio:   the I2S card's PCM device, S32_LE 44.1 kHz, written from a SCHED_FIFO thread.
 *
 * MIT licence, see LICENSE.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>
#include "drm_uapi.h"
#include "hw.h"

void hw_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("tracker: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

uint32_t hw_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int find_card(const char *a, const char *b, const char *c)
{
    for (int n = 0; n < 8; n++) {
        char p[64], id[64] = {0};
        snprintf(p, sizeof p, "/proc/asound/card%d/id", n);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (!fgets(id, sizeof id, f)) id[0] = 0;
        fclose(f);
        if ((a && strcasestr(id, a)) || (b && strcasestr(id, b)) || (c && strcasestr(id, c))) return n;
    }
    return -1;
}

/* ---------------------------------------------------------------- screen ---- */
uint32_t scr_fb[SCR_W * SCR_H];
static uint32_t shown[SCR_W * SCR_H];
static int drm_fd = -1, shown_valid;
static uint32_t fb_id, crtc_id, conn_id, dumb_handle, scan_pitch;
static uint64_t scan_size;
static uint8_t *scanout;

static int drm_call(unsigned long req, void *arg, const char *what)
{
    int r;
    while ((r = ioctl(drm_fd, req, arg)) < 0 && errno == EINTR) {}
    if (r < 0) hw_log("drm %s: %s", what, strerror(errno));
    return r;
}

static void drm_dirty(int x1, int y1, int x2, int y2)
{
    struct mpc_drm_clip_rect clip = { (uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2 };
    struct mpc_drm_fb_dirty_cmd cmd = { fb_id, 0, 0, 1, (uint64_t)(uintptr_t)&clip };
    drm_call(MPC_DRM_IOCTL_MODE_DIRTYFB, &cmd, "dirtyfb");
}

int scr_open(void)
{
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { hw_log("/dev/dri/card0: %s", strerror(errno)); return -1; }
    if (ioctl(drm_fd, MPC_DRM_IOCTL_SET_MASTER, 0) < 0) { hw_log("drm set master: %s (is ac50 or another copy still running?)", strerror(errno)); return -1; }
    uint32_t crtcs[4] = {0}, conns[4] = {0};
    struct mpc_drm_card_res res = { 0, (uint64_t)(uintptr_t)crtcs, (uint64_t)(uintptr_t)conns, 0, 0, 4, 4, 0, 0, 0, 0, 0 };
    if (drm_call(MPC_DRM_IOCTL_MODE_GETRESOURCES, &res, "getresources") < 0) return -1;
    if (!res.count_crtcs || !res.count_connectors) { hw_log("drm: no crtc/connector"); return -1; }
    crtc_id = crtcs[0]; conn_id = conns[0];
    struct mpc_drm_get_connector con; memset(&con, 0, sizeof con); con.connector_id = conn_id;
    if (drm_call(MPC_DRM_IOCTL_MODE_GETCONNECTOR, &con, "getconnector") < 0) return -1;
    if (!con.count_modes) { hw_log("drm: connector has no modes"); return -1; }
    uint32_t nmodes = con.count_modes;                 /* 1 on the device, dozens in QEMU */
    struct mpc_drm_modeinfo *modes = calloc(nmodes, sizeof *modes);
    memset(&con, 0, sizeof con); con.connector_id = conn_id; con.modes_ptr = (uint64_t)(uintptr_t)modes; con.count_modes = nmodes;
    if (drm_call(MPC_DRM_IOCTL_MODE_GETCONNECTOR, &con, "getconnector modes") < 0) return -1;
    struct mpc_drm_modeinfo mode = modes[0];
    for (uint32_t i = 0; i < nmodes; i++) if (modes[i].hdisplay == SCR_W && modes[i].vdisplay == SCR_H) { mode = modes[i]; break; }
    free(modes);
    if (mode.hdisplay != SCR_W || mode.vdisplay != SCR_H) { hw_log("drm: no %dx%d mode", SCR_W, SCR_H); return -1; }
    struct mpc_drm_create_dumb dumb = { SCR_H, SCR_W, 32, 0, 0, 0, 0 };
    if (drm_call(MPC_DRM_IOCTL_MODE_CREATE_DUMB, &dumb, "create dumb") < 0) return -1;
    dumb_handle = dumb.handle; scan_pitch = dumb.pitch; scan_size = dumb.size;
    struct mpc_drm_fb_cmd2 add; memset(&add, 0, sizeof add);
    add.width = SCR_W; add.height = SCR_H; add.pixel_format = MPC_DRM_FORMAT_XRGB8888;
    add.handles[0] = dumb.handle; add.pitches[0] = dumb.pitch;
    if (drm_call(MPC_DRM_IOCTL_MODE_ADDFB2, &add, "addfb2") < 0) return -1;
    fb_id = add.fb_id;
    struct mpc_drm_map_dumb map = { dumb.handle, 0, 0 };
    if (drm_call(MPC_DRM_IOCTL_MODE_MAP_DUMB, &map, "map dumb") < 0) return -1;
    void *p = mmap(NULL, scan_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, (off_t)map.offset);
    if (p == MAP_FAILED) { hw_log("mmap dumb: %s", strerror(errno)); return -1; }
    scanout = p;
    memset(scanout, 0, scan_size);
    struct mpc_drm_crtc set; memset(&set, 0, sizeof set);
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id; set.count_connectors = 1;
    set.crtc_id = crtc_id; set.fb_id = fb_id; set.mode = mode; set.mode_valid = 1;
    if (drm_call(MPC_DRM_IOCTL_MODE_SETCRTC, &set, "setcrtc") < 0) return -1;
    drm_dirty(0, 0, SCR_W, SCR_H);
    hw_log("screen %ux%u pitch %u", mode.hdisplay, mode.vdisplay, scan_pitch);
    return 0;
}

static void flush_rect(int x1, int y1, int x2, int y2)
{
    for (int y = y1; y < y2; y++) {
        memcpy(scanout + (size_t)y * scan_pitch + (size_t)x1 * 4, scr_fb + (size_t)y * SCR_W + x1, (size_t)(x2 - x1) * 4);
        memcpy(shown + (size_t)y * SCR_W + x1, scr_fb + (size_t)y * SCR_W + x1, (size_t)(x2 - x1) * 4);
    }
    drm_dirty(x1, y1, x2, y2);
}

void scr_present(void)
{
    if (!scanout) return;
    if (!shown_valid) { shown_valid = 1; flush_rect(0, 0, SCR_W, SCR_H); return; }
    /* group changed rows into bands; rows closer than GAP are sent as one band */
    enum { GAP = 6, MAXBANDS = 5 };
    int bands = 0, by1 = -1, by2 = 0, bx1 = SCR_W, bx2 = 0, total_y1 = -1, total_y2 = 0;
    struct { int x1, y1, x2, y2; } band[MAXBANDS];
    for (int y = 0; y <= SCR_H; y++) {
        int x1 = SCR_W, x2 = 0;
        if (y < SCR_H) {
            const uint32_t *a = scr_fb + (size_t)y * SCR_W, *b = shown + (size_t)y * SCR_W;
            if (memcmp(a, b, SCR_W * 4)) {
                x1 = 0; while (a[x1] == b[x1]) x1++;
                x2 = SCR_W; while (a[x2 - 1] == b[x2 - 1]) x2--;
            }
        }
        if (x2 > x1) {
            if (by1 < 0) { by1 = y; bx1 = x1; bx2 = x2; }
            else { if (x1 < bx1) bx1 = x1; if (x2 > bx2) bx2 = x2; }
            by2 = y + 1;
            if (total_y1 < 0) total_y1 = y;
            total_y2 = y + 1;
        } else if (by1 >= 0 && (y - by2 >= GAP || y == SCR_H)) {
            if (bands < MAXBANDS) { band[bands].x1 = bx1; band[bands].y1 = by1; band[bands].x2 = bx2; band[bands].y2 = by2; }
            bands++; by1 = -1;
        }
    }
    if (bands == 0) return;
    if (bands > MAXBANDS) { flush_rect(0, total_y1, SCR_W, total_y2); return; }
    for (int i = 0; i < bands; i++) flush_rect(band[i].x1, band[i].y1, band[i].x2, band[i].y2);
}

void scr_close(void)
{
    if (scanout) { memset(scanout, 0, scan_size); drm_dirty(0, 0, SCR_W, SCR_H); munmap(scanout, scan_size); scanout = NULL; }
    if (drm_fd >= 0) {
        if (fb_id) { uint32_t id = fb_id; ioctl(drm_fd, MPC_DRM_IOCTL_MODE_RMFB, &id); }
        if (dumb_handle) { struct mpc_drm_destroy_dumb d = { dumb_handle }; ioctl(drm_fd, MPC_DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
        ioctl(drm_fd, MPC_DRM_IOCTL_DROP_MASTER, 0);
        close(drm_fd); drm_fd = -1;
    }
}

/* --------------------------------------------------------------- surface ---- */
static int midi_fd = -1;
static pthread_t in_thread;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*on_msg)(const uint8_t *, int);
static int pad_sent[16], led_sent[128];

static int msg_len(uint8_t status)
{
    switch (status & 0xF0) { case 0xC0: case 0xD0: return 2; case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3; default: return 1; }
}

static void *input_thread(void *arg)
{
    (void)arg;
    uint8_t buf[256], msg[3]; int n = 0, need = 0, in_sysex = 0;
    for (;;) {
        ssize_t r = read(midi_fd, buf, sizeof buf);
        if (r < 0) { if (errno == EINTR) continue; hw_log("midi read: %s", strerror(errno)); break; }
        if (r == 0) break;
        for (ssize_t i = 0; i < r; i++) {
            uint8_t b = buf[i];
            if (b >= 0xF8) continue;
            if (in_sysex) { if (b == 0xF7) in_sysex = 0; continue; }
            if (b == 0xF0) { in_sysex = 1; n = 0; continue; }
            if (b & 0x80) { msg[0] = b; n = 1; need = msg_len(b); if (need == 1) n = 0; continue; }
            if (n == 0) continue;
            msg[n++] = b;
            if (n == need) { on_msg(msg, n); n = 1; }
        }
    }
    return NULL;
}

int surf_open(void (*cb)(const uint8_t *, int))
{
    on_msg = cb;
    for (int i = 0; i < 16; i++) pad_sent[i] = -1;
    for (int i = 0; i < 128; i++) led_sent[i] = -1;
    int card = find_card("surface", NULL, NULL);
    if (card < 0) { hw_log("no control surface MIDI device found"); return -1; }
    char path[64]; snprintf(path, sizeof path, "/dev/snd/midiC%dD0", card);
    midi_fd = open(path, O_RDWR | O_CLOEXEC);
    if (midi_fd < 0) { hw_log("%s: %s", path, strerror(errno)); return -1; }
    /* The stock app leaves the input substream in timestamp-framing mode; ask for plain bytes. */
    struct snd_rawmidi_params prm; memset(&prm, 0, sizeof prm);
    prm.stream = SNDRV_RAWMIDI_STREAM_INPUT; prm.buffer_size = 4096; prm.avail_min = 1; prm.mode = 0;
    if (ioctl(midi_fd, SNDRV_RAWMIDI_IOCTL_PARAMS, &prm) < 0) hw_log("rawmidi params: %s", strerror(errno));
    hw_log("control surface: %s", path);
    pthread_create(&in_thread, NULL, input_thread, NULL);
    return 0;
}

static void midi_out(const uint8_t *b, size_t n)
{
    if (midi_fd < 0) return;
    pthread_mutex_lock(&out_lock);
    if (write(midi_fd, b, n) < 0) {}
    pthread_mutex_unlock(&out_lock);
}

void surf_pad_rgb(int pad, int r, int g, int b)
{
    if (pad < 0 || pad > 15) return;
    int key = (r & 127) << 16 | (g & 127) << 8 | (b & 127);
    if (pad_sent[pad] == key) return;
    pad_sent[pad] = key;
    uint8_t sx[12] = { 0xF0, 0x47, 0x7F, 0x5C, 0x65, 0x00, 0x04, (uint8_t)pad, (uint8_t)(r & 127), (uint8_t)(g & 127), (uint8_t)(b & 127), 0xF7 };
    midi_out(sx, sizeof sx);
}

void surf_led(int cc, int value)
{
    if (cc < 0 || cc > 127 || led_sent[cc] == value) return;
    led_sent[cc] = value;
    uint8_t m[3] = { 0xB0, (uint8_t)cc, (uint8_t)value };
    midi_out(m, 3);
}

void surf_request_knobs(void)
{
    static const uint8_t req[8] = { 0xF0, 0x47, 0x7F, 0x5C, 0x42, 0x00, 0x00, 0xF7 };
    midi_out(req, sizeof req);
}

void surf_close(void)
{
    if (midi_fd < 0) return;
    for (int i = 0; i < 16; i++) surf_pad_rgb(i, 0, 0, 0);
    for (int i = 0; i < 128; i++) if (led_sent[i] > 0) surf_led(i, 0);
}

/* ----------------------------------------------------------------- audio ---- */
#define RATE 44100
#define MAXPERIOD 1024
static int pcm_fd = -1, pcm_channels = 2, period = 128, nperiods = 4;
static pthread_t audio_thread;
static volatile int audio_quit;
static volatile unsigned underruns;
static void (*render_cb)(float *, int);

static void hw_set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
    struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m->bits, 0, sizeof m->bits);
    m->bits[bit >> 5] = 1u << (bit & 31);
}

static void hw_set_int(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v; i->integer = 1;
}

static int pcm_try(int channels)
{
    struct snd_pcm_hw_params hw; memset(&hw, 0, sizeof hw);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(hw.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xff, sizeof hw.masks[0].bits);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++)
        hw.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = ~0u;
    hw.rmask = ~0u; hw.info = ~0u;
    hw_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S32_LE);
    hw_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    hw_set_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, (unsigned)channels);
    hw_set_int(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
    hw_set_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, (unsigned)period);
    hw_set_int(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, (unsigned)(nperiods * period));
    return ioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
}

static void *audio_main(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = 40 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) hw_log("SCHED_FIFO not available");
    static float mix[2 * MAXPERIOD];
    static int32_t buf[4 * MAXPERIOD];
    while (!audio_quit) {
        render_cb(mix, period);
        for (int i = 0; i < period; i++)
            for (int c = 0; c < pcm_channels; c++) {
                float x = c < 2 ? mix[2 * i + c] : 0.0f;
                if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
                buf[i * pcm_channels + c] = (int32_t)(x * 2147483392.0f);
            }
        struct snd_xferi x = { .buf = buf, .frames = (snd_pcm_uframes_t)period };
        while (ioctl(pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            if (errno == EINTR) { if (audio_quit) break; continue; }
            if (errno != EPIPE || ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { hw_log("pcm write: %s", strerror(errno)); audio_quit = 1; break; }
            underruns++;
        }
    }
    return NULL;
}

unsigned aud_underruns(void) { return underruns; }

int aud_open(void (*render)(float *, int))
{
    render_cb = render;
    const char *e = getenv("TRACKER_PERIOD");
    if (e && atoi(e) >= 32 && atoi(e) <= MAXPERIOD) period = atoi(e);
    e = getenv("TRACKER_PERIODS");
    if (e && atoi(e) >= 2 && atoi(e) <= 16) nperiods = atoi(e);
    int card = -1;
    char target[128] = {0};
    ssize_t l = readlink("/dev/snd/by-path/platform-sound", target, sizeof target - 1);
    if (l > 0) { const char *q = strrchr(target, 'C'); if (q) sscanf(q, "C%d", &card); }
    if (card < 0) card = find_card("i2s", "rockchip", "loopback");
    if (card < 0) { hw_log("no sound card found"); return -1; }
    char p[32]; snprintf(p, sizeof p, "/dev/snd/pcmC%dD0p", card);
    pcm_fd = open(p, O_RDWR | O_CLOEXEC);
    if (pcm_fd < 0) { hw_log("%s: %s", p, strerror(errno)); return -1; }
    /* the device takes 2 channels and small periods; the emulator's loopback card wants 4 channels and 256 */
    static const int try_period[] = { 0, 256, 512 }, try_ch[] = { 2, 4 };
    int ok = 0;
    for (int i = 0; i < 3 && !ok; i++)
        for (int j = 0; j < 2 && !ok; j++) {
            int keep = period;
            if (try_period[i]) period = try_period[i];
            if (pcm_try(try_ch[j]) == 0) { pcm_channels = try_ch[j]; ok = 1; } else period = keep;
        }
    if (!ok) { hw_log("pcm hw_params: %s", strerror(errno)); close(pcm_fd); pcm_fd = -1; return -1; }
    if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { hw_log("pcm prepare: %s", strerror(errno)); close(pcm_fd); pcm_fd = -1; return -1; }
    /* the stock app may switch the speaker off when it stops; switch it on if no headphones are in */
    snprintf(p, sizeof p, "/dev/snd/controlC%d", card);
    int ctl = open(p, O_RDWR | O_CLOEXEC);
    if (ctl >= 0) {
        struct snd_ctl_elem_value v; long spk = -1, hp = -1;
        memset(&v, 0, sizeof v); v.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER; strcpy((char *)v.id.name, "Builtin Speaker Switch");
        if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_READ, &v) == 0) spk = v.value.integer.value[0];
        memset(&v, 0, sizeof v); v.id.iface = SNDRV_CTL_ELEM_IFACE_CARD; strcpy((char *)v.id.name, "Headphones Jack");
        if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_READ, &v) == 0) hp = v.value.integer.value[0];
        if (spk == 0 && hp == 0) {
            memset(&v, 0, sizeof v); v.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER; strcpy((char *)v.id.name, "Builtin Speaker Switch");
            v.value.integer.value[0] = 1;
            if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) == 0) hw_log("speaker switched on");
        }
        close(ctl);
    }
    hw_log("audio pcmC%dD0p: S32_LE %dch %d Hz, period %d x %d", card, pcm_channels, RATE, period, nperiods);
    audio_quit = 0;
    if (pthread_create(&audio_thread, NULL, audio_main, NULL) != 0) { close(pcm_fd); pcm_fd = -1; return -1; }
    return 0;
}

void aud_close(void)
{
    if (pcm_fd < 0) return;
    audio_quit = 1;
    pthread_join(audio_thread, NULL);
    ioctl(pcm_fd, SNDRV_PCM_IOCTL_DROP);
    close(pcm_fd); pcm_fd = -1;
}
