/* Start-up screen. MIT licence, see LICENSE. */
#ifndef INTRO_H
#define INTRO_H
void intro_run(void);      /* blocks for about five seconds; any pad or button skips it */
void intro_abort(void);    /* safe from any thread or a signal handler */
int  intro_active(void);
#endif
