#ifndef POSIRC_PIPE_H
#define POSIRC_PIPE_H
#include <signal.h>
/* Stream stdin as literal channel messages; optionally emit received messages
 * as TSV on stdout and keep listening after EOF. No curses or local commands. */
int posirc_pipe_run(const char *host, const char *port, const char *nick,
                    const char *channel, const char *ca_file, int input, int output,
                    const volatile sig_atomic_t *stop);
#endif
