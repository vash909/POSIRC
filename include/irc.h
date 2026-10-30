#ifndef POSIRC_IRC_H
#define POSIRC_IRC_H

#include <stddef.h>

/* Classic IRC: 512 bytes on the wire, including CRLF. No IRCv3 capabilities
 * are requested yet. Strings passed to the parser exclude CRLF. */
#define IRC_WIRE_CAPACITY 512
#define IRC_PARAM_CAPACITY 15

struct irc_frame {
    char data[IRC_WIRE_CAPACITY];
    size_t length;
};

struct irc_message {
    char *prefix;
    char *command;
    char *params[IRC_PARAM_CAPACITY];
    size_t count;
};

/* The callback borrows a mutable, terminated line until it returns. Return
 * nonzero to stop. Feed returns -1 for malformed/oversized input or callback
 * failure; the connection must then be closed, not resynchronized silently. */
typedef int (*irc_line_callback)(void *, char *);
int irc_feed(struct irc_frame *frame, const char *data, size_t length,
             irc_line_callback callback, void *context);
int irc_parse(char *line, struct irc_message *message);

/* Validate UTF-8 parameters and nicknames. Non-ASCII identifiers are accepted
 * locally; the server determines its own identifier policy and byte limits. */
int irc_atom_valid(const char *text, size_t limit);
int irc_nick_valid(const char *text);
/* mapping: 0 = RFC1459, 1 = strict RFC1459, 2 = ASCII. */
int irc_equal(const char *left, const char *right, int mapping);

#endif
