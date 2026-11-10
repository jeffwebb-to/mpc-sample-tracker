/* MPCTRACKER: a standalone pattern tracker for the Akai MPC Sample, run from an SD card with
 * the stock application stopped. MIT licence, see LICENSE.
 *   tracker --card <folder> [--no-intro]    the card's root; songs and samples live in <folder>/tracker */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "hw.h"
#include "intro.h"
#include "ui.h"

static void on_signal(int sig) { (void)sig; ui_request_quit(); intro_abort(); }

int main(int argc, char **argv)
{
    const char *card = ".";
    int intro = getenv("TRACKER_NOINTRO") == NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-intro")) intro = 0;
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
    }
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal);
    hw_log("%s %s", APP_NAME, APP_VERSION);
    if (scr_open() < 0) { scr_close(); return 1; }
    ui_init(card);
    if (surf_open(ui_midi) < 0) hw_log("running without the control surface");
    else surf_request_knobs();
    if (aud_open(ui_render_audio) < 0) hw_log("running without audio");
    if (intro) intro_run();
    ui_run();
    aud_close();
    surf_close();
    scr_close();
    hw_log("bye");
    return 0;
}
