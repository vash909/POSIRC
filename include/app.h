#ifndef POSIRC_APP_H
#define POSIRC_APP_H

#include <stddef.h>
#include <time.h>

#include "irc.h"
#include "buffer.h"

#define POSIRC_VERSION "0.3.1-beta.1"

struct posirc_net;

/* One connection, bounded conversation slots, and lazily allocated histories
 * and user lists. All state belongs to the main thread. */
struct posirc_app {
    struct posirc_buffer buffers[POSIRC_BUFFER_CAPACITY];
    size_t active_buffer, dropped_lines;
    unsigned long revision, message_serial;
    int running, quitting, registered, case_mapping;
    char nick[64];
    char target[POSIRC_TARGET_CAPACITY];
    char autojoin[POSIRC_TARGET_CAPACITY];
    char server[256], server_port[6];
    char prefix_modes[16], prefix_symbols[16], chantypes[16], statusmsg[16];
    char channel_modes[4][64];
    struct posirc_net *net;
    struct irc_frame frame;
    time_t registration_started;
};

void posirc_app_init(struct posirc_app *app);
int posirc_app_setup(struct posirc_app *app, const char *ca_file);
void posirc_app_destroy(struct posirc_app *app);
int posirc_app_connect(struct posirc_app *app, const char *host, const char *port);
void posirc_app_step(struct posirc_app *app, int input_fd, int timeout_ms);
/* Process a terminated local command or a message to the active target. */
void posirc_app_submit(struct posirc_app *app, const char *line);
/* Buffer slots are stable until /close. Index zero is the server buffer. */
struct posirc_buffer *posirc_app_current(struct posirc_app *app);
struct posirc_buffer *posirc_app_find(struct posirc_app *app, const char *name);
void posirc_app_select(struct posirc_app *app, size_t index);
void posirc_app_cycle(struct posirc_app *app, int direction);
/* Process a mutable IRC payload without CRLF; also used by protocol tests. */
int posirc_app_receive(void *context, char *line);
/* Return a borrowed line from the active buffer, oldest first. */
const char *posirc_app_line(const struct posirc_app *app, size_t index);

#endif
