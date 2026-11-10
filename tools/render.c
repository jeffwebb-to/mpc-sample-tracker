/* Host test tool: render a song to a WAV file with the same replayer the device uses.
 *   render demo out.wav [seconds]        the built-in demo song
 *   render song.mst|song.mod out.wav [seconds]
 *   render --roundtrip in out.mst        load, save as native, load again, compare
 * Build: cc -O2 -Isrc tools/render.c src/song.c src/player.c -lm -lpthread -o build/render */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <strings.h>
#include <string.h>
#include "song.h"
#include "player.h"

static Song song, song2;

static int load(Song *s, const char *path)
{
    char err[64] = "";
    size_t n = strlen(path);
    if (!strcmp(path, "demo")) { song_init(s); song_demo(s); return 0; }
    int r = (n > 4 && !strcasecmp(path + n - 4, ".mod")) ? mod_import(s, path, err, sizeof err) : song_load(s, path, err, sizeof err);
    if (r < 0) fprintf(stderr, "%s: %s\n", path, err);
    return r;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "--roundtrip")) {
        char err[64] = "";
        if (load(&song, argv[2]) < 0) return 1;
        if (song_save(&song, argv[3], err, sizeof err) < 0) { fprintf(stderr, "save: %s\n", err); return 1; }
        if (song_load(&song2, argv[3], err, sizeof err) < 0) { fprintf(stderr, "reload: %s\n", err); return 1; }
        int bad = memcmp(song.pat, song2.pat, sizeof song.pat) || memcmp(song.order, song2.order, sizeof song.order) || song.nord != song2.nord || song.bpm != song2.bpm;
        for (int i = 1; i <= NINS; i++) {
            Ins *a = &song.ins[i], *b = &song2.ins[i];
            if (!a->data != !b->data) { bad = 1; continue; }
            if (a->data && (a->frames != b->frames || a->ch != b->ch || memcmp(a->data, b->data, sizeof(int16_t) * a->frames * a->ch) || a->loop_len != b->loop_len || a->rate != b->rate)) bad = 1;
        }
        printf("roundtrip %s\n", bad ? "MISMATCH" : "ok");
        return bad;
    }
    if (argc < 3) { fprintf(stderr, "usage: render demo|file out.wav [seconds]\n"); return 2; }
    if (load(&song, argv[1]) < 0) return 1;
    double secs = argc > 3 ? atof(argv[3]) : 16;
    int frames = (int)(secs * MIX_RATE);
    printf("song '%s' bpm %d speed %d orders %d patterns %d\n", song.name, song.bpm, song.speed, song.nord, song_used_patterns(&song));
    pl_init(&song);
    pl_play(PL_SONG, 0, 0, 0);
    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    uint32_t bytes = (uint32_t)frames * 4, v32; uint16_t v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + bytes; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f); v16 = 1; fwrite(&v16, 2, 1, f); v16 = 2; fwrite(&v16, 2, 1, f);
    v32 = MIX_RATE; fwrite(&v32, 4, 1, f); v32 = MIX_RATE * 4; fwrite(&v32, 4, 1, f); v16 = 4; fwrite(&v16, 2, 1, f); v16 = 16; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    float buf[2 * 128]; double peak = 0, sumsq = 0; int last_row = -1, rows = 0;
    for (int done = 0; done < frames; done += 128) {
        pl_lock(); pl_render(buf, 128); pl_unlock();
        PlayPos p = pl_pos();
        if (p.row != last_row) { last_row = p.row; rows++; }
        for (int i = 0; i < 256; i++) {
            double x = buf[i]; if (x < 0 ? -x > peak : x > peak) peak = x < 0 ? -x : x; sumsq += x * x;
            int16_t s = (int16_t)(buf[i] * 32767.0f); fwrite(&s, 2, 1, f);
        }
    }
    fclose(f);
    printf("rendered %.1f s, %d rows, peak %.3f, rms %.4f\n", secs, rows, peak, sqrt(sumsq / (frames * 2.0)));
    return 0;
}
