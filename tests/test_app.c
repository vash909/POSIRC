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

int main(void)
{
    struct posirc_app app;
    size_t index;
    int errors = 0;
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
    posirc_app_submit(&app, "/quit");
    errors += check("quit stops application", !app.running);

    posirc_app_destroy(&app);
    if (errors != 0)
        return EXIT_FAILURE;
    puts("OK: application commands and history checks passed");
    return EXIT_SUCCESS;
}
