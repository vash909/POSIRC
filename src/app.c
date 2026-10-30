#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "text.h"

#include "app.h"
#include "net.h"

struct posirc_buffer *posirc_app_current(struct posirc_app *app)
{
    return &app->buffers[app->active_buffer];
}

struct posirc_buffer *posirc_app_find(struct posirc_app *app, const char *name)
{
    size_t index;
    for (index = 0; index < POSIRC_BUFFER_CAPACITY; ++index)
        if (app->buffers[index].type != BUFFER_UNUSED &&
            irc_equal(app->buffers[index].name, name, app->case_mapping))
            return &app->buffers[index];
    return NULL;
}

static void sync_target(struct posirc_app *app)
{
    struct posirc_buffer *buffer = posirc_app_current(app);
    (void)snprintf(app->target, sizeof app->target, "%s",
                   buffer->type == BUFFER_QUERY || buffer->joined ? buffer->name : "");
}

void posirc_app_select(struct posirc_app *app, size_t index)
{
    if (index >= POSIRC_BUFFER_CAPACITY || app->buffers[index].type == BUFFER_UNUSED) return;
    app->active_buffer = index;
    app->buffers[index].unread = 0;
    sync_target(app);
    ++app->revision;
}

void posirc_app_cycle(struct posirc_app *app, int direction)
{
    size_t index = app->active_buffer, count;
    for (count = 0; count < POSIRC_BUFFER_CAPACITY; ++count) {
        index = direction > 0 ? (index + 1) % POSIRC_BUFFER_CAPACITY :
            (index + POSIRC_BUFFER_CAPACITY - 1) % POSIRC_BUFFER_CAPACITY;
        if (app->buffers[index].type != BUFFER_UNUSED) { posirc_app_select(app, index); return; }
    }
}

static void append_to(struct posirc_app *app, struct posirc_buffer *buffer, const char *text)
{
    if (buffer == NULL) buffer = &app->buffers[0];
    if (buffer_append(buffer, text) != 0) ++app->dropped_lines;
    if (buffer != posirc_app_current(app) && buffer->unread < POSIRC_HISTORY_CAPACITY) ++buffer->unread;
    ++app->revision;
}

static void append_line(struct posirc_app *app, const char *text)
{
    append_to(app, posirc_app_current(app), text);
}

static void log_to(struct posirc_app *app, struct posirc_buffer *buffer, const char *format, ...)
{
    char text[POSIRC_TEXT_CAPACITY];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    append_to(app, buffer, text);
}

static struct posirc_buffer *open_buffer(struct posirc_app *app, const char *name, enum buffer_type type)
{
    size_t index;
    struct posirc_buffer *buffer = posirc_app_find(app, name);
    if (buffer != NULL) return buffer;
    for (index = 1; index < POSIRC_BUFFER_CAPACITY; ++index) {
        if (app->buffers[index].type == BUFFER_UNUSED) {
            buffer_init(&app->buffers[index], type, name);
            ++app->revision;
            return &app->buffers[index];
        }
    }
    append_to(app, &app->buffers[0], "Buffer limit reached; use /close on an unused conversation.");
    return NULL;
}

static int send_command(struct posirc_app *app, const char *format, ...)
{
    char line[IRC_WIRE_CAPACITY];
    int length;
    va_list arguments;
    va_start(arguments, format);
    length = vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (length < 0 || length > IRC_WIRE_CAPACITY - 2) {
        append_line(app, "Message exceeds the IRC limit (510 bytes before CRLF).");
        return -1;
    }
    if (net_send(app->net, line) != 0) {
        append_line(app, "Cannot queue message: disconnected, closing or send queue full.");
        return -1;
    }
    return 0;
}

static int channel_valid(const struct posirc_app *app, const char *channel)
{
    return irc_atom_valid(channel, POSIRC_TARGET_CAPACITY - 1) &&
           strchr(app->chantypes, channel[0]) != NULL;
}

static void select_target(struct posirc_app *app, const char *target)
{
    struct posirc_buffer *buffer;
    if (*target == '\0') { posirc_app_select(app, 0); return; }
    buffer = open_buffer(app, target, channel_valid(app, target) ? BUFFER_CHANNEL : BUFFER_QUERY);
    if (buffer != NULL) posirc_app_select(app, (size_t)(buffer - app->buffers));
}

static void support_defaults(struct posirc_app *app)
{
    (void)snprintf(app->prefix_modes, sizeof app->prefix_modes, "ov");
    (void)snprintf(app->prefix_symbols, sizeof app->prefix_symbols, "@+");
    (void)snprintf(app->chantypes, sizeof app->chantypes, "#&+!");
    (void)snprintf(app->statusmsg, sizeof app->statusmsg, "@+");
    (void)snprintf(app->channel_modes[0], sizeof app->channel_modes[0], "beI");
    (void)snprintf(app->channel_modes[1], sizeof app->channel_modes[1], "k");
    (void)snprintf(app->channel_modes[2], sizeof app->channel_modes[2], "l");
    (void)snprintf(app->channel_modes[3], sizeof app->channel_modes[3], "imnpst");
}

static void support_token(struct posirc_app *app, const char *token)
{
    if (strcmp(token, "CASEMAPPING=ascii") == 0) app->case_mapping = 2;
    else if (strcmp(token, "CASEMAPPING=strict-rfc1459") == 0) app->case_mapping = 1;
    else if (strcmp(token, "CASEMAPPING=rfc1459") == 0) app->case_mapping = 0;
    else if (strncmp(token, "PREFIX=(", 8) == 0) {
        const char *end = strchr(token + 8, ')');
        if (end != NULL && (size_t)(end - token - 8) < sizeof app->prefix_modes &&
            (size_t)(end - token - 8) == strlen(end + 1)) {
            size_t length = (size_t)(end - token - 8);
            memcpy(app->prefix_modes, token + 8, length);
            app->prefix_modes[length] = '\0';
            (void)snprintf(app->prefix_symbols, sizeof app->prefix_symbols, "%s", end + 1);
        }
    } else if (strncmp(token, "CHANTYPES=", 10) == 0 && strlen(token + 10) < sizeof app->chantypes) {
        (void)snprintf(app->chantypes, sizeof app->chantypes, "%s", token + 10);
    } else if (strncmp(token, "STATUSMSG=", 10) == 0 && strlen(token + 10) < sizeof app->statusmsg) {
        (void)snprintf(app->statusmsg, sizeof app->statusmsg, "%s", token + 10);
    } else if (strncmp(token, "CHANMODES=", 10) == 0) {
        const char *group = token + 10;
        size_t index;
        for (index = 0; index < 4; ++index) {
            const char *end = strchr(group, ',');
            size_t length = end != NULL ? (size_t)(end - group) : strlen(group);
            if (length >= sizeof app->channel_modes[index]) return;
            memcpy(app->channel_modes[index], group, length);
            app->channel_modes[index][length] = '\0';
            if (end == NULL) break;
            group = end + 1;
        }
    }
}

static void names_reply(struct posirc_app *app, struct posirc_buffer *buffer, char *names)
{
    char *cursor = names;
    if (!buffer->names_sync) buffer->names_sync = 1;
    while (*cursor != '\0') {
        char *name, *end;
        unsigned int modes = 0;
        while (*cursor == ' ') ++cursor;
        if (*cursor == '\0') break;
        while (*cursor != '\0') {
            const char *symbol = strchr(app->prefix_symbols, *cursor);
            if (symbol == NULL) break;
            modes |= 1u << (unsigned int)(symbol - app->prefix_symbols);
            ++cursor;
        }
        name = cursor;
        while (*cursor != '\0' && *cursor != ' ') ++cursor;
        if (*cursor != '\0') *cursor++ = '\0';
        end = strchr(name, '!'); /* Tolerate userhost-in-names. */
        if (end != NULL) *end = '\0';
        {
            struct posirc_user *old = buffer_user(buffer, name, app->case_mapping);
            char renamed[64];
            size_t hops = 0;
            /* Follow pending renames only when the old nick actually appears
             * in this channel's NAMES snapshot. This avoids phantom members. */
            while (old != NULL && old->renamed_to[0] != '\0' && hops++ < POSIRC_USER_CAPACITY) {
                (void)snprintf(renamed, sizeof renamed, "%s", old->renamed_to);
                name = renamed;
                old = buffer_user(buffer, name, app->case_mapping);
            }
            if (hops < POSIRC_USER_CAPACITY)
                (void)buffer_user_set(buffer, name, modes, 1, 0, app->case_mapping);
        }
    }
    ++app->revision;
}

static void channel_mode(struct posirc_app *app, struct posirc_buffer *buffer, struct irc_message *message)
{
    const char *mode = message->params[1];
    size_t parameter = 2;
    int adding = 1;
    for (; *mode != '\0'; ++mode) {
        const char *prefix;
        int consumes;
        if (*mode == '+') { adding = 1; continue; }
        if (*mode == '-') { adding = 0; continue; }
        prefix = strchr(app->prefix_modes, *mode);
        consumes = prefix != NULL || strchr(app->channel_modes[0], *mode) != NULL ||
            strchr(app->channel_modes[1], *mode) != NULL ||
            (adding && strchr(app->channel_modes[2], *mode) != NULL);
        if (consumes && parameter >= message->count) break;
        if (prefix != NULL) {
            const char *nick = message->params[parameter];
            struct posirc_user *user = buffer_user(buffer, nick, app->case_mapping);
            unsigned int modes = user != NULL ? user->modes : 0;
            unsigned int bit = 1u << (unsigned int)(prefix - app->prefix_modes);
            modes = adding ? modes | bit : modes & ~bit;
            (void)buffer_user_set(buffer, nick, modes, 1, 1, app->case_mapping);
        }
        if (consumes) ++parameter;
    }
    ++app->revision;
}

int posirc_app_receive(void *context, char *line)
{
    struct posirc_app *app = context;
    struct irc_message message;
    struct posirc_buffer *buffer = NULL;
    char original[IRC_WIRE_CAPACITY], sender[IRC_WIRE_CAPACITY];
    char *separator;
    size_t index;
    int self;
    (void)snprintf(original, sizeof original, "%s", line);
    if (irc_parse(line, &message) != 0) return -1;
    if (strcmp(message.command, "PING") == 0) {
        if (message.count == 0) return -1;
        return send_command(app, "PONG :%s", message.params[0]);
    }
    if (strcmp(message.command, "PONG") == 0) return 0;
    (void)snprintf(sender, sizeof sender, "%s", message.prefix != NULL ? message.prefix : "server");
    separator = strchr(sender, '!');
    if (separator != NULL) *separator = '\0';
    self = irc_equal(sender, app->nick, app->case_mapping);

    if (strcmp(message.command, "001") == 0 && message.count >= 1) {
        if (!irc_nick_valid(message.params[0])) return -1;
        (void)snprintf(app->nick, sizeof app->nick, "%s", message.params[0]);
        if (!app->registered) {
            app->registered = 1;
            append_to(app, &app->buffers[0], "Registered. Use /join #channel or /query nickname.");
            if (app->autojoin[0] != '\0') (void)send_command(app, "JOIN %s", app->autojoin);
        }
    } else if (strcmp(message.command, "005") == 0) {
        for (index = 1; index < message.count; ++index) support_token(app, message.params[index]);
    } else if (strcmp(message.command, "353") == 0 && message.count >= 4) {
        buffer = posirc_app_find(app, message.params[2]);
        if (buffer != NULL && buffer->joined) names_reply(app, buffer, message.params[3]);
        return 0;
    } else if (strcmp(message.command, "366") == 0 && message.count >= 2) {
        buffer = posirc_app_find(app, message.params[1]);
        if (buffer != NULL) { buffer_names_end(buffer); ++app->revision; }
        return 0;
    } else if (strcmp(message.command, "JOIN") == 0 && message.count >= 1) {
        if (!channel_valid(app, message.params[0])) return 0;
        buffer = self ? open_buffer(app, message.params[0], BUFFER_CHANNEL) : posirc_app_find(app, message.params[0]);
        if (buffer != NULL) {
            if (self) {
                buffer_users_clear(buffer);
                buffer->joined = buffer->names_sync = 1;
                posirc_app_select(app, (size_t)(buffer - app->buffers));
            }
            (void)buffer_user_set(buffer, sender, 0, 1, !self, app->case_mapping);
        }
    } else if ((strcmp(message.command, "PART") == 0 && message.count >= 1) ||
               (strcmp(message.command, "KICK") == 0 && message.count >= 2)) {
        const char *nick = strcmp(message.command, "PART") == 0 ? sender : message.params[1];
        buffer = posirc_app_find(app, message.params[0]);
        if (buffer != NULL) {
            (void)buffer_user_set(buffer, nick, 0, 0, 1, app->case_mapping);
            if (irc_equal(nick, app->nick, app->case_mapping)) {
                buffer->joined = 0;
                buffer_users_clear(buffer);
                sync_target(app);
            }
        }
    } else if (strcmp(message.command, "QUIT") == 0 ||
               (strcmp(message.command, "NICK") == 0 && message.count == 1)) {
        int rename = strcmp(message.command, "NICK") == 0;
        if (rename && !irc_nick_valid(message.params[0])) return -1;
        for (index = 1; index < POSIRC_BUFFER_CAPACITY; ++index) {
            struct posirc_buffer *conversation = &app->buffers[index];
            struct posirc_user *user = buffer_user(conversation, sender, app->case_mapping);
            int query = conversation->type == BUFFER_QUERY && irc_equal(conversation->name, sender, app->case_mapping);
            if ((user != NULL && user->present) || query) log_to(app, conversation, "-- %s", original);
            if (conversation->joined && (user != NULL || conversation->names_sync)) {
                if (rename) buffer_user_rename(conversation, sender, message.params[0], app->case_mapping);
                else (void)buffer_user_set(conversation, sender, 0, 0, 1, app->case_mapping);
            }
            if (query && rename) {
                struct posirc_buffer *other = posirc_app_find(app, message.params[0]);
                if (other == NULL || other == conversation) {
                    (void)snprintf(conversation->name, sizeof conversation->name, "%s", message.params[0]);
                } else {
                    size_t entry;
                    for (entry = 0; entry < conversation->history_count; ++entry)
                        append_to(app, other, buffer_line(conversation, entry));
                    if (conversation->draft[0] != '\0') {
                        if (other->draft[0] == '\0') {
                            (void)snprintf(other->draft, sizeof other->draft, "%s", conversation->draft);
                            other->cursor = conversation->cursor;
                        } else log_to(app, other, "Unsent draft from renamed query: %s", conversation->draft);
                    }
                    if (app->active_buffer == index) posirc_app_select(app, (size_t)(other - app->buffers));
                    buffer_free(conversation);
                }
            }
        }
        if (self && rename) (void)snprintf(app->nick, sizeof app->nick, "%s", message.params[0]);
        sync_target(app);
        ++app->revision;
        return 0;
    } else if ((strcmp(message.command, "PRIVMSG") == 0 || strcmp(message.command, "NOTICE") == 0) && message.count >= 2) {
        const char *target = message.params[0];
        int notice = strcmp(message.command, "NOTICE") == 0;
        if (strchr(app->statusmsg, target[0]) != NULL && channel_valid(app, target + 1)) ++target;
        if (channel_valid(app, target)) {
            buffer = open_buffer(app, target, BUFFER_CHANNEL);
        } else if (irc_equal(target, app->nick, app->case_mapping) && (!notice || separator != NULL) && irc_nick_valid(sender)) {
            buffer = open_buffer(app, sender, BUFFER_QUERY);
        } else if (self && irc_nick_valid(target)) {
            buffer = open_buffer(app, target, BUFFER_QUERY);
        }
        if (strncmp(message.params[1], "\001ACTION ", 8) == 0) {
            char *body = message.params[1] + 8;
            size_t length = strlen(body);
            if (length > 0 && body[length - 1] == 1) body[length - 1] = '\0';
            log_to(app, buffer, "* %s %s", sender, body);
        } else {
            log_to(app, buffer, "[%s] %s%s%s %s", target, notice ? "-" : "<", sender,
                   notice ? "-" : ">", message.params[1]);
        }
        return 0;
    } else if (strcmp(message.command, "MODE") == 0 && message.count >= 2) {
        buffer = posirc_app_find(app, message.params[0]);
        if (buffer != NULL && buffer->joined) channel_mode(app, buffer, &message);
    } else if ((strcmp(message.command, "TOPIC") == 0 && message.count >= 2) ||
               (strcmp(message.command, "332") == 0 && message.count >= 3)) {
        size_t channel = strcmp(message.command, "TOPIC") == 0 ? 0 : 1;
        buffer = posirc_app_find(app, message.params[channel]);
        if (buffer != NULL) text_clean(buffer->topic, sizeof buffer->topic, message.params[channel + 1], 1);
    } else if (message.count >= 2 && channel_valid(app, message.params[1])) {
        buffer = posirc_app_find(app, message.params[1]);
    }
    log_to(app, buffer, "-- %s", original);
    if (strcmp(message.command, "ERROR") == 0 && app->net != NULL) {
        net_disconnect(app->net, "Server ended the IRC session.");
        return 1;
    }
    return 0;
}

static void network_event(void *context, enum net_event event, const char *text, size_t length)
{
    struct posirc_app *app = context;
    ++app->revision;
    switch (event) {
    case NET_STATUS:
        break;
    case NET_READY:
        log_to(app, &app->buffers[0], "Connected to %s using %s; certificate verified.", app->server, text);
        app->registration_started = time(NULL);
        if (send_command(app, "NICK %s", app->nick) != 0 ||
            send_command(app, "USER posirc 0 * :POSIRC client") != 0)
            net_disconnect(app->net, "Cannot queue IRC registration.");
        break;
    case NET_DATA:
        if (irc_feed(&app->frame, text, length, posirc_app_receive, app) != 0 && net_active(app->net))
            net_disconnect(app->net, "Invalid/oversized IRC frame or failed protocol reply.");
        break;
    case NET_CLOSED:
        app->registered = 0;
        app->frame.length = 0;
        {
            size_t index;
            for (index = 1; index < POSIRC_BUFFER_CAPACITY; ++index) {
                app->buffers[index].joined = 0;
                buffer_users_clear(&app->buffers[index]);
            }
            sync_target(app);
        }
        log_to(app, &app->buffers[0], "Disconnected: %s", text);
        if (app->quitting)
            app->running = 0;
        break;
    }
}

void posirc_app_init(struct posirc_app *app)
{
    memset(app, 0, sizeof *app);
    app->running = 1;
    buffer_init(&app->buffers[0], BUFFER_SERVER, "*server*");
    support_defaults(app);
    (void)snprintf(app->nick, sizeof app->nick, "posirc");
    append_line(app, "Welcome to POSIRC " POSIRC_VERSION ". TLS is required for every connection.");
    append_line(app, "Use /nick yournick, then /connect server [port]. Default port: 6697.");
    append_line(app, "Type /help for commands. Tab / Shift-Tab switches conversations.");
}

int posirc_app_setup(struct posirc_app *app, const char *ca_file)
{
    app->net = net_create(ca_file, network_event, app);
    return app->net != NULL ? 0 : -1;
}

void posirc_app_destroy(struct posirc_app *app)
{
    size_t index;
    for (index = 0; index < POSIRC_BUFFER_CAPACITY; ++index) buffer_free(&app->buffers[index]);
    net_destroy(app->net);
    app->net = NULL;
}

int posirc_app_connect(struct posirc_app *app, const char *host, const char *port)
{
    if (app->net == NULL || net_active(app->net)) {
        append_line(app, "Disconnect the current session before connecting again.");
        return -1;
    }
    if (strcmp(app->server, host) != 0 || strcmp(app->server_port, port) != 0) {
        size_t index;
        for (index = 1; index < POSIRC_BUFFER_CAPACITY; ++index) buffer_free(&app->buffers[index]);
    }
    (void)snprintf(app->server_port, sizeof app->server_port, "%s", port);
    support_defaults(app);
    app->frame.length = 0;
    app->registered = app->case_mapping = 0;
    select_target(app, "");
    (void)snprintf(app->server, sizeof app->server, "%s", host);
    if (net_connect(app->net, host, port) != 0) {
        append_line(app, "Cannot start connection: check host and numeric port (1-65535).");
        return -1;
    }
    log_to(app, &app->buffers[0], "Connecting to %s:%s with TLS...", host, port);
    return 0;
}

void posirc_app_step(struct posirc_app *app, int input_fd, int timeout_ms)
{
    net_step(app->net, input_fd, timeout_ms);
    if (net_ready(app->net) && !app->registered &&
        difftime(time(NULL), app->registration_started) >= 60)
        net_disconnect(app->net, "IRC registration timed out.");
}

static char *take_word(char **cursor)
{
    char *word;
    while (**cursor == ' ')
        ++*cursor;
    if (**cursor == '\0')
        return NULL;
    word = *cursor;
    while (**cursor != '\0' && **cursor != ' ')
        ++*cursor;
    if (**cursor != '\0')
        *(*cursor)++ = '\0';
    while (**cursor == ' ')
        ++*cursor;
    return word;
}

static void send_message(struct posirc_app *app, const char *target, const char *text)
{
    if (!app->registered) {
        append_line(app, "Not registered: message was not sent.");
        return;
    }
    if (!irc_atom_valid(target, POSIRC_TARGET_CAPACITY - 1) || text[0] == '\0') {
        append_line(app, "Choose a target with /join, /query or /target first.");
        return;
    }
    if (send_command(app, "PRIVMSG %s :%s", target, text) == 0) {
        struct posirc_buffer *buffer = open_buffer(app, target,
            channel_valid(app, target) ? BUFFER_CHANNEL : BUFFER_QUERY);
        log_to(app, buffer, "[%s] <%s> %s\017 [queued]", target, app->nick, text);
    }
}

static void disconnect_app(struct posirc_app *app, int quitting)
{
    app->quitting = quitting;
    if (net_ready(app->net)) {
        (void)send_command(app, "QUIT :Leaving POSIRC");
        net_finish(app->net);
    } else if (net_active(app->net)) {
        net_disconnect(app->net, "Connection cancelled.");
    } else if (quitting) {
        app->running = 0;
    }
}

void posirc_app_submit(struct posirc_app *app, const char *line)
{
    char copy[POSIRC_TEXT_CAPACITY], *cursor, *command, *argument, *extra;
    if (line[0] == '\0' || app->quitting)
        return;
    if (strlen(line) >= sizeof copy || !utf8_valid(line) || strpbrk(line, "\r\n") != NULL) {
        append_line(app, "Input is too long, invalid UTF-8 or contains a line separator.");
        return;
    }
    if (line[0] != '/') {
        send_message(app, app->target, line);
        return;
    }
    (void)snprintf(copy, sizeof copy, "%s", line);
    cursor = copy;
    command = take_word(&cursor);
    if (strcmp(command, "/quit") == 0 && *cursor == '\0') {
        disconnect_app(app, 1);
    } else if (strcmp(command, "/disconnect") == 0 && *cursor == '\0') {
        disconnect_app(app, 0);
    } else if (strcmp(command, "/clear") == 0 && *cursor == '\0') {
        buffer_clear(posirc_app_current(app));
        ++app->revision;
    } else if (strcmp(command, "/help") == 0) {
        append_line(app, "/connect host [port]  Connect with verified TLS (default 6697)");
        append_line(app, "/nick nickname        Set or change your nickname");
        append_line(app, "/join #channel [key]  Join a channel; select it after server confirmation");
        append_line(app, "/part [#channel]     Leave a channel (default: current target)");
        append_line(app, "/query nickname      Select a private conversation");
        append_line(app, "/target name         Select an existing channel or conversation");
        append_line(app, "/msg target text     Send a private or channel message");
        append_line(app, "/disconnect | /quit  Disconnect or exit");
        append_line(app, "/clear | /help       Clear transcript or show local commands");
        append_line(app, "/buffer name|number | /buffers | /users | /close");
        append_line(app, "Tab/Shift-Tab or F2/F3: buffers | PgUp/PgDn: history | F7/F8: users");
        append_line(app, "Ctrl-K: color | Ctrl-B: bold | Ctrl-O: reset | Ctrl-T: italic");
        append_line(app, "Ctrl-_: underline | Ctrl-R: reverse | Ctrl-U: clear | Ctrl-L: refresh");
    } else if (strcmp(command, "/buffers") == 0) {
        size_t index;
        for (index = 0; index < POSIRC_BUFFER_CAPACITY; ++index) {
            struct posirc_buffer *buffer = &app->buffers[index];
            if (buffer->type != BUFFER_UNUSED)
                log_to(app, posirc_app_current(app), "%zu: %s%s (%zu unread)", index, buffer->name,
                       buffer->type == BUFFER_CHANNEL && !buffer->joined ? " [left]" : "", buffer->unread);
        }
    } else if (strcmp(command, "/buffer") == 0) {
        struct posirc_buffer *buffer;
        argument = take_word(&cursor);
        buffer = argument != NULL ? posirc_app_find(app, argument) : NULL;
        if (argument != NULL && *cursor == '\0') {
            char *end;
            unsigned long index = strtoul(argument, &end, 10);
            if (buffer != NULL) posirc_app_select(app, (size_t)(buffer - app->buffers));
            else if (*argument != '\0' && *end == '\0' && index < POSIRC_BUFFER_CAPACITY &&
                     app->buffers[index].type != BUFFER_UNUSED) posirc_app_select(app, (size_t)index);
            else append_line(app, "No such buffer. Use /buffers to list conversations.");
        } else append_line(app, "Usage: /buffer number|name");
    } else if (strcmp(command, "/close") == 0 && *cursor == '\0') {
        struct posirc_buffer *buffer = posirc_app_current(app);
        if (buffer->type == BUFFER_SERVER) append_line(app, "The server buffer cannot be closed.");
        else if (buffer->joined) append_line(app, "Use /part before closing a joined channel.");
        else { buffer_free(buffer); posirc_app_select(app, 0); }
    } else if (strcmp(command, "/users") == 0 && *cursor == '\0') {
        struct posirc_buffer *buffer = posirc_app_current(app);
        size_t index;
        buffer_users_sort(buffer);
        log_to(app, buffer, "%zu users%s", buffer_user_count(buffer), buffer->users_truncated ? " (list truncated)" : "");
        for (index = 0; index < buffer->user_count; ++index) {
            unsigned int bit;
            char prefix = ' ';
            if (!buffer->users[index].present) continue;
            for (bit = 0; bit < strlen(app->prefix_symbols); ++bit)
                if (buffer->users[index].modes & (1u << bit)) { prefix = app->prefix_symbols[bit]; break; }
            log_to(app, buffer, "%c%s", prefix, buffer->users[index].nick);
        }
    } else if (strcmp(command, "/connect") == 0) {
        argument = take_word(&cursor);
        extra = take_word(&cursor);
        if (argument == NULL || *cursor != '\0')
            append_line(app, "Usage: /connect host [port]");
        else
            (void)posirc_app_connect(app, argument, extra != NULL ? extra : "6697");
    } else if (strcmp(command, "/nick") == 0) {
        argument = take_word(&cursor);
        if (!irc_nick_valid(argument) || *cursor != '\0') {
            append_line(app, "Usage: /nick nickname (1-63 bytes; server nickname rules apply)");
        } else if (net_active(app->net)) {
            if (!net_ready(app->net))
                append_line(app, "Wait for TLS verification before changing nickname.");
            else if (send_command(app, "NICK %s", argument) == 0 && !app->registered)
                (void)snprintf(app->nick, sizeof app->nick, "%s", argument);
        } else {
            (void)snprintf(app->nick, sizeof app->nick, "%s", argument);
            ++app->revision;
        }
    } else if (strcmp(command, "/query") == 0 || strcmp(command, "/target") == 0) {
        argument = take_word(&cursor);
        if (*cursor != '\0' || (!irc_nick_valid(argument) &&
            !(strcmp(command, "/target") == 0 && argument != NULL && channel_valid(app, argument))))
            append_line(app, "Usage: /query nickname or /target #channel|nickname");
        else
            select_target(app, argument);
    } else if (strcmp(command, "/msg") == 0) {
        argument = take_word(&cursor);
        if (argument == NULL || *cursor == '\0')
            append_line(app, "Usage: /msg target text");
        else
            send_message(app, argument, cursor);
    } else if (strcmp(command, "/join") == 0 || strcmp(command, "/part") == 0) {
        int join = strcmp(command, "/join") == 0;
        argument = take_word(&cursor);
        extra = take_word(&cursor);
        if (!join && argument == NULL)
            argument = app->target;
        if (argument == NULL || !channel_valid(app, argument) || *cursor != '\0' ||
            (extra != NULL && (!join || !irc_atom_valid(extra, 128)))) {
            append_line(app, "Usage: /join #channel [key] or /part [#channel]");
        } else if (!app->registered) {
            append_line(app, "Wait for IRC registration before joining or leaving channels.");
        } else {
            (void)send_command(app, "%s %s%s%s", join ? "JOIN" : "PART", argument,
                               extra != NULL ? " " : "", extra != NULL ? extra : "");
        }
    } else {
        append_line(app, "Unknown command. Type /help for local commands.");
    }
}

const char *posirc_app_line(const struct posirc_app *app, size_t index)
{
    return buffer_line(&app->buffers[app->active_buffer], index);
}
