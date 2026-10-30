#ifndef POSIRC_UI_H
#define POSIRC_UI_H

#include <signal.h>

struct posirc_app;

/* Own the terminal until exit; return zero on success, -1 on setup failure.
 * The signal flag is read by the loop; handlers must not call curses. */
int posirc_ui_run(struct posirc_app *app, const volatile sig_atomic_t *stop_requested);

#endif
