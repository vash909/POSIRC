#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "irc.h"
#include "net.h"
#include "pipe.h"
#include "text.h"

struct pipe_client {
    struct posirc_net *net;
    struct irc_frame frame;
    const char *channel;
    char nick[64];
    int registered, joined, failed, acknowledged, finished, awaiting_ack;
    int mapping, output;
    double deadline;
};

static double seconds(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) == 0)
        return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
#endif
    return (double)time(NULL);
}

static void fail(struct pipe_client *client, const char *reason)
{
    char clean[1024];
    if (client->failed) return;
    text_clean(clean, sizeof clean, reason, 0);
    fprintf(stderr, "POSIRC pipe: %s\n", clean);
    client->failed = 1;
}

static int queue(struct pipe_client *client, const char *command, const char *argument)
{
    char line[IRC_WIRE_CAPACITY];
    int size = snprintf(line, sizeof line, "%s %s", command, argument);
    if (size < 0 || size > IRC_WIRE_CAPACITY - 2 || net_send(client->net, line) != 0) {
        fail(client, "Cannot queue IRC command.");
        return -1;
    }
    return 0;
}

/* Stable TSV: strip IRC styling, sanitize terminal controls and embedded tabs,
 * and flush each record so downstream commands need not wait for a full buffer. */
static void output_message(struct pipe_client *client, const char *nick, const char *body)
{
    struct text_reader reader;
    uint32_t codepoint;
    char name[IRC_WIRE_CAPACITY * 3], text[IRC_WIRE_CAPACITY * 3];
    size_t used = 0;
    text_clean(name, sizeof name, nick, 0);
    text_reader_init(&reader, body);
    while (text_next(&reader, &codepoint)) {
        char encoded[5];
        size_t length = utf8_encode(codepoint, encoded);
        if (used + length >= sizeof text) { fail(client, "Output record is too long."); return; }
        memcpy(text + used, encoded, length);
        used += length;
    }
    text[used] = '\0';
    if (fprintf(stdout, "%s\t%s\n", name, text) < 0 || fflush(stdout) == EOF)
        fail(client, "Cannot write stdout (downstream pipe closed or output error).");
}

static int receive(void *context, char *line)
{
    struct pipe_client *client = context;
    struct irc_message message;
    char original[IRC_WIRE_CAPACITY], *separator;
    size_t i;
    int self;
    (void)snprintf(original, sizeof original, "%s", line);
    if (irc_parse(line, &message) != 0) return -1;
    separator = message.prefix != NULL ? strchr(message.prefix, '!') : NULL;
    if (separator != NULL) *separator = '\0';
    self = message.prefix != NULL && irc_equal(message.prefix, client->nick, client->mapping);
    if (strcmp(message.command, "PING") == 0 && message.count > 0) {
        char token[IRC_WIRE_CAPACITY];
        (void)snprintf(token, sizeof token, ":%s", message.params[message.count - 1]);
        return queue(client, "PONG", token);
    }
    if (client->output && client->joined && strcmp(message.command, "PRIVMSG") == 0 &&
        message.count >= 2 && message.prefix != NULL &&
        irc_equal(message.params[0], client->channel, client->mapping)) {
        output_message(client, message.prefix, message.params[1]);
    }
    if ((strlen(message.command) == 3 && message.command[0] >= '4' && message.command[0] <= '5' &&
         message.command[1] >= '0' && message.command[1] <= '9' &&
         message.command[2] >= '0' && message.command[2] <= '9' &&
         strcmp(message.command, "422") != 0) ||
        strcmp(message.command, "ERROR") == 0) {
        fail(client, original);
    } else if (strcmp(message.command, "001") == 0 && message.count > 0 && !client->registered) {
        if (!irc_nick_valid(message.params[0])) return -1;
        (void)snprintf(client->nick, sizeof client->nick, "%s", message.params[0]);
        client->registered = 1;
        client->deadline = seconds() + 30;
        return queue(client, "JOIN", client->channel);
    } else if (strcmp(message.command, "005") == 0) {
        for (i = 1; i < message.count; ++i) {
            if (strcmp(message.params[i], "CASEMAPPING=ascii") == 0) client->mapping = 2;
            else if (strcmp(message.params[i], "CASEMAPPING=strict-rfc1459") == 0) client->mapping = 1;
            else if (strcmp(message.params[i], "CASEMAPPING=rfc1459") == 0) client->mapping = 0;
        }
    } else if (strcmp(message.command, "JOIN") == 0 && message.count > 0 && self && client->registered &&
               irc_equal(message.params[0], client->channel, client->mapping)) {
        client->joined = 1;
    } else if (strcmp(message.command, "NICK") == 0 && message.count > 0 && self) {
        if (!irc_nick_valid(message.params[0])) return -1;
        (void)snprintf(client->nick, sizeof client->nick, "%s", message.params[0]);
    } else if ((strcmp(message.command, "PART") == 0 && message.count > 0 && self &&
                irc_equal(message.params[0], client->channel, client->mapping)) ||
               (strcmp(message.command, "KICK") == 0 && message.count > 1 &&
                irc_equal(message.params[0], client->channel, client->mapping) &&
                irc_equal(message.params[1], client->nick, client->mapping))) {
        fail(client, "Left or was kicked from the destination channel.");
    } else if (strcmp(message.command, "PONG") == 0 && message.count > 0 && client->awaiting_ack &&
               strcmp(message.params[message.count - 1], "posirc-pipe-end") == 0) {
        client->acknowledged = 1;
    }
    return client->failed ? -1 : 0;
}

static void event(void *context, enum net_event kind, const char *data, size_t length)
{
    struct pipe_client *client = context;
    switch (kind) {
    case NET_READY:
        client->deadline = seconds() + 60;
        (void)queue(client, "NICK", client->nick);
        (void)queue(client, "USER", "posirc 0 * :POSIRC client");
        break;
    case NET_DATA:
        if (irc_feed(&client->frame, data, length, receive, client) != 0)
            fail(client, "Invalid IRC frame or failed protocol reply.");
        break;
    case NET_CLOSED: fail(client, data); break;
    case NET_FINISHED: client->finished = 1; break;
    case NET_STATUS: break;
    }
}

static int send_line(struct pipe_client *client, char *line, size_t length)
{
    char message[IRC_WIRE_CAPACITY];
    int size;
    if (length > 0 && line[length - 1] == '\r') --length;
    line[length] = '\0';
    if (length == 0) return 0;
    if (!utf8_valid(line) || strchr(line, '\r') != NULL) {
        fail(client, "Input must be valid UTF-8 without embedded CR or NUL bytes.");
        return -1;
    }
    size = snprintf(message, sizeof message, "PRIVMSG %s :%s", client->channel, line);
    if (size < 0 || size > IRC_WIRE_CAPACITY - 2) {
        fail(client, "Input line exceeds the IRC limit (510 bytes including PRIVMSG and target).");
        return -1;
    }
    if (net_send(client->net, message) != 0) {
        fail(client, "Cannot queue input line.");
        return -1;
    }
    return 0;
}

int posirc_pipe_run(const char *host, const char *port, const char *nick,
                    const char *channel, const char *ca_file, int read_input, int output,
                    const volatile sig_atomic_t *stop)
{
    struct pipe_client client;
    char input[4096], line[IRC_WIRE_CAPACITY];
    size_t offset = 0, available = 0, used = 0;
    int eof = 0, finishing = 0;
    double blocked_since = 0;
    memset(&client, 0, sizeof client);
    client.channel = channel;
    client.output = output;
    eof = !read_input;
    (void)snprintf(client.nick, sizeof client.nick, "%s", nick);
    client.net = net_create(ca_file, event, &client);
    if (client.net == NULL) { fail(&client, "Cannot allocate TLS transport."); return -1; }
    if (net_connect(client.net, host, port) != 0) fail(&client, "Cannot start connection.");
    while (!client.failed && !client.finished && !*stop) {
        int ready_for_input = client.joined && !eof && net_pending(client.net) < 32;
        net_step(client.net, ready_for_input && offset == available ? STDIN_FILENO : -1,
                 ready_for_input && offset < available ? 0 : 100);
        if (client.failed || client.finished) break;
        if (!client.joined) {
            if (net_ready(client.net) && seconds() >= client.deadline)
                fail(&client, client.registered ? "Channel JOIN timed out." : "Registration timed out.");
            continue;
        }
        if (client.awaiting_ack) {
            if (client.acknowledged && !finishing) {
                if (queue(&client, "QUIT", ":Leaving POSIRC") == 0) {
                    finishing = 1;
                    net_finish(client.net);
                }
            } else if (!finishing && seconds() >= client.deadline) {
                fail(&client, "Server acknowledgement timed out.");
            }
            continue;
        }
        /* Leave half the bounded queue available for protocol replies. Do not
         * read more stdin until the transport catches up (Unix backpressure). */
        if (net_pending(client.net) >= 32) {
            if (blocked_since == 0) blocked_since = seconds();
            if (seconds() - blocked_since >= 30) fail(&client, "Output queue stalled.");
            continue;
        }
        blocked_since = 0;
        if (eof) {
            if (used > 0 && send_line(&client, line, used) != 0) break;
            used = 0;
            if (output) continue;
            if (queue(&client, "PING", ":posirc-pipe-end") == 0) {
                client.awaiting_ack = 1;
                client.deadline = seconds() + 30;
            }
            continue;
        }
        if (offset == available) {
            struct pollfd descriptor;
            int result;
            ssize_t count;
            descriptor.fd = STDIN_FILENO;
            descriptor.events = POLLIN;
            descriptor.revents = 0;
            result = poll(&descriptor, 1, 0);
            if (result < 0) { if (errno != EINTR) fail(&client, "Cannot poll stdin."); continue; }
            if (result == 0) continue;
            count = read(STDIN_FILENO, input, sizeof input);
            if (count < 0) {
                if (errno != EINTR && errno != EAGAIN) fail(&client, "Cannot read stdin.");
                continue;
            }
            if (count == 0) { eof = 1; continue; }
            offset = 0;
            available = (size_t)count;
        }
        while (offset < available) {
            char byte = input[offset++];
            if (byte == '\n') { (void)send_line(&client, line, used); used = 0; break; }
            if (byte == '\0' || used == sizeof line - 1) {
                fail(&client, byte == '\0' ? "NUL byte in stdin." : "Input line is too long.");
                break;
            }
            line[used++] = byte;
        }
    }
    if (*stop) fail(&client, "Interrupted.");
    net_destroy(client.net);
    return client.finished && !client.failed ? 0 : -1;
}
