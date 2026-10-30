#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static int check(const char *label, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int settings(void)
{
    struct posirc_app app;
    char command[400];
    int errors = 0;
    posirc_app_init(&app);
    errors += check("default quit", strcmp(app.quit_message, "Leaving POSIRC") == 0);
    errors += check("default part has no reason", app.part_message[0] == '\0');
    errors += check("default real name", strcmp(app.realname, "POSIRC client") == 0);
    posirc_app_submit(&app, "/set --quit A presto 日");
    posirc_app_submit(&app, "/set --part Ci vediamo 😀");
    posirc_app_submit(&app, "/set --realname Nome Cognome è");
    errors += check("custom quit", strcmp(app.quit_message, "A presto 日") == 0);
    errors += check("custom part", strcmp(app.part_message, "Ci vediamo 😀") == 0);
    errors += check("custom real name", strcmp(app.realname, "Nome Cognome è") == 0);
    posirc_app_submit(&app, "/set --quit bad\r\nQUIT");
    errors += check("setting injection rejected", strcmp(app.quit_message, "A presto 日") == 0);
    memcpy(command, "/set --quit ", 12);
    memset(command + 12, 'x', 256); command[268] = '\0';
    posirc_app_submit(&app, command);
    errors += check("overlong reason rejected without truncation", strcmp(app.quit_message, "A presto 日") == 0);
    command[267] = '\0'; posirc_app_submit(&app, command);
    errors += check("maximum reason accepted", strlen(app.quit_message) == 255);
    posirc_app_submit(&app, "/set --realname");
    errors += check("empty real name rejected", strcmp(app.realname, "Nome Cognome è") == 0);
    posirc_app_submit(&app, "/set --part");
    posirc_app_submit(&app, "/set --quit");
    errors += check("reasons can be cleared", app.quit_message[0] == '\0' && app.part_message[0] == '\0');
    posirc_app_submit(&app, "/set --reset");
    errors += check("settings reset", strcmp(app.quit_message, "Leaving POSIRC") == 0 &&
                    app.part_message[0] == '\0' && strcmp(app.realname, "POSIRC client") == 0);
    posirc_app_submit(&app, "/set --unknown value");
    posirc_app_submit(&app, "/set");
    errors += check("settings query", strstr(posirc_app_line(&app, app.buffers[0].history_count - 1), "--realname: POSIRC client") != NULL);
    posirc_app_destroy(&app);
    return errors;
}

int main(void)
{
    struct posirc_app app;
    size_t index;
    int errors = settings();
    const char *line;

    posirc_app_init(&app);
    errors += check("starts running", app.running);
    posirc_app_submit(&app, "/clear");
    posirc_app_submit(&app, "");
    errors += check("empty input adds no message", app.buffers[0].history_count == 0);
    errors += check("empty history has no line", posirc_app_line(&app, 0) == NULL);

    /* Overflow repeatedly with alternating messages to verify chronological
     * order across wraparound, not just the number of retained entries. */
    for (index = 0; index < POSIRC_HISTORY_CAPACITY * 3 + 1; ++index)
        posirc_app_submit(&app, index % 2 == 0 ? "hello" : "/unknown");
    errors += check("history is bounded", app.buffers[0].history_count == POSIRC_HISTORY_CAPACITY);
    for (index = 0; index < app.buffers[0].history_count; ++index) {
        const char *expected = index % 2 == 0
            ? "Unknown command. Type /help for local commands."
            : "Not registered: message was not sent.";
        line = posirc_app_line(&app, index);
        errors += check("wrapped history order", line != NULL && strcmp(line, expected) == 0);
    }
    errors += check("past end is rejected",
                    posirc_app_line(&app, app.buffers[0].history_count) == NULL);
    errors += check("large index is rejected", posirc_app_line(&app, (size_t)-1) == NULL);

    posirc_app_submit(&app, "/clear");
    posirc_app_submit(&app, "/help");
    line = posirc_app_line(&app, 0);
    errors += check("clear then help", line != NULL && strstr(line, "/connect") != NULL);
    posirc_app_submit(&app, "/me");
    errors += check("empty action rejected", strstr(posirc_app_line(&app, app.buffers[0].history_count - 1), "Usage: /me") != NULL);
    posirc_app_submit(&app, "/me waves");
    errors += check("offline action rejected", strstr(posirc_app_line(&app, app.buffers[0].history_count - 1), "Not registered") != NULL);
    posirc_app_submit(&app, "/nick tester");
    errors += check("offline nickname", strcmp(app.nick, "tester") == 0);
    posirc_app_submit(&app, "/nick bad nick");
    errors += check("invalid nickname rejected", strcmp(app.nick, "tester") == 0);
    posirc_app_submit(&app, "/query friend");
    errors += check("private target", strcmp(app.target, "friend") == 0);
    {
        char join[] = ":tester!u@h JOIN :#room";
        errors += check("own JOIN", posirc_app_receive(&app, join) == 0);
    }
    posirc_app_submit(&app, "/target #room");
    errors += check("channel target", strcmp(app.target, "#room") == 0);
    posirc_app_submit(&app, "/target #room,#other");
    errors += check("multiple targets rejected", strcmp(app.target, "#room") == 0);
    posirc_app_submit(&app, "/nick evil\r\nJOIN #other");
    errors += check("line injection rejected", strcmp(app.nick, "tester") == 0);
    {
        struct posirc_buffer *room = posirc_app_current(&app);
        size_t users[POSIRC_BUFFER_CAPACITY];
        char reply[IRC_WIRE_CAPACITY];
        for (index = 0; index < POSIRC_BUFFER_CAPACITY; ++index) users[index] = app.buffers[index].user_count;
        posirc_app_submit(&app, "/list");
        errors += check("offline LIST rejected", strstr(buffer_line(room, room->history_count - 1), "registration") != NULL);
        posirc_app_submit(&app, "/list nickname");
        errors += check("invalid LIST target", strstr(buffer_line(room, room->history_count - 1), "Usage: /list") != NULL);
        posirc_app_submit(&app, "/list #room extra");
        errors += check("extra LIST argument", strstr(buffer_line(room, room->history_count - 1), "Usage: /list") != NULL);
        for (index = 0; index < 10000; ++index) {
            (void)snprintf(reply, sizeof reply, ":srv 322 tester #channel%zu %zu :Topic 日本語", index, index);
            errors += check("LIST reply", posirc_app_receive(&app, reply) == 0);
        }
        errors += check("LIST retains focus", posirc_app_current(&app) == room);
        errors += check("LIST history bounded", app.buffers[0].history_count == POSIRC_HISTORY_CAPACITY);
        errors += check("LIST keeps newest result", strstr(buffer_line(&app.buffers[0], POSIRC_HISTORY_CAPACITY - 1), "#channel9999 | 9999 users | Topic 日本語") != NULL);
        errors += check("LIST creates no channel buffers", posirc_app_find(&app, "#channel9999") == NULL);
        for (index = 0; index < POSIRC_BUFFER_CAPACITY; ++index)
            errors += check("LIST creates no user lists", app.buffers[index].user_count == users[index]);
    }
    posirc_app_submit(&app, "/quit");
    errors += check("quit stops application", !app.running);

    posirc_app_destroy(&app);
    if (errors != 0)
        return EXIT_FAILURE;
    puts("OK: application commands and history checks passed");
    return EXIT_SUCCESS;
}
