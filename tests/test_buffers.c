#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void receive(struct posirc_app *app, const char *line)
{
    char copy[IRC_WIRE_CAPACITY];
    (void)snprintf(copy, sizeof copy, "%s", line);
    CHECK(posirc_app_receive(app, copy) == 0);
}

static int present(struct posirc_buffer *buffer, const char *nick)
{
    struct posirc_user *user = buffer_user(buffer, nick, 0);
    return user != NULL && user->present;
}

static void completion_history(void)
{
    struct posirc_buffer buffer;
    size_t i;
    buffer_init(&buffer, BUFFER_QUERY, "peer");
    CHECK(buffer_append_pending(&buffer, "same [queued]", 11) == 0);
    CHECK(buffer_append_pending(&buffer, "same [queued]", 12) == 0);
    CHECK(buffer_complete(&buffer, 12, 1));
    CHECK(strcmp(buffer_line(&buffer, 0), "same [queued]") == 0);
    CHECK(strcmp(buffer_line(&buffer, 1), "same") == 0);
    CHECK(buffer_complete(&buffer, 0, 0));
    CHECK(strcmp(buffer_line(&buffer, 0), "same [unsent]") == 0);
    CHECK(!buffer_complete(&buffer, 11, 1));
    buffer_clear(&buffer);
    CHECK(buffer_append_pending(&buffer, "old [queued]", 13) == 0);
    for (i = 0; i < POSIRC_HISTORY_CAPACITY; ++i) CHECK(buffer_append(&buffer, "new") == 0);
    CHECK(!buffer_complete(&buffer, 13, 1));
    CHECK(strcmp(buffer_line(&buffer, 0), "new") == 0);
    buffer_clear(&buffer);
    CHECK(buffer_append_pending(&buffer, "cleared [queued]", 14) == 0);
    buffer_clear(&buffer);
    CHECK(buffer_append_pending(&buffer, "retained [queued]", 15) == 0);
    CHECK(!buffer_complete(&buffer, 14, 1));
    CHECK(strcmp(buffer_line(&buffer, 0), "retained [queued]") == 0);
    buffer_free(&buffer);
    buffer_init(&buffer, BUFFER_QUERY, "replacement");
    CHECK(!buffer_complete(&buffer, 15, 1));
    buffer_free(&buffer);
}

int main(void)
{
    completion_history();
    struct posirc_app app;
    struct posirc_buffer *one, *two, *query;
    size_t selected, index;
    posirc_app_init(&app);
    receive(&app, ":srv 005 posirc PREFIX=(qaohv)~&@%+ CHANMODES=beI,k,l,imnpst :supported");
    receive(&app, ":posirc!u@h JOIN :#uno");
    one = posirc_app_find(&app, "#uno");
    CHECK(one != NULL && one->joined);
    receive(&app, ":srv 353 posirc = #uno :@posirc +alice bob");
    receive(&app, ":bob!u@h PART #uno :gone");
    receive(&app, ":late!u@h JOIN #uno");
    receive(&app, ":ghost!u@h NICK renamed");
    receive(&app, ":srv 353 posirc = #uno :bob @late ghost");
    receive(&app, ":srv 366 posirc #uno :end");
    CHECK(!present(one, "bob") && present(one, "late") && !present(one, "ghost") && present(one, "renamed"));
    CHECK(buffer_user(one, "late", 0)->modes == 0);
    CHECK(buffer_user(one, "posirc", 0)->modes == (1u << 2));
    receive(&app, ":renamed!u@h NICK :RENAMED");
    CHECK(strcmp(buffer_user(one, "renamed", 0)->nick, "RENAMED") == 0);
    receive(&app, ":op!u@h MODE #uno +kl-o secret 4 posirc");
    CHECK(buffer_user(one, "posirc", 0)->modes == 0);
    receive(&app, ":op!u@h MODE #uno +ov alice late");
    CHECK(buffer_user(one, "alice", 0)->modes == ((1u << 2) | (1u << 4)));
    receive(&app, ":posirc!u@h JOIN :#日本語");
    two = posirc_app_find(&app, "#日本語");
    CHECK(two != NULL && two->joined);
    receive(&app, ":unknown!u@h NICK different");
    CHECK(!present(two, "different"));
    receive(&app, ":srv 353 posirc = #日本語 :posirc alice");
    receive(&app, ":srv 366 posirc #日本語 :end");
    selected = app.active_buffer;
    receive(&app, ":alice!u@h PRIVMSG #uno :\00304caffè 😀\017");
    CHECK(app.active_buffer == selected && one->unread > 0);
    CHECK(strstr(buffer_line(one, one->history_count - 1), "caffè 😀") != NULL);
    CHECK(strstr(buffer_line(two, two->history_count - 1), "caffè") == NULL);
    receive(&app, ":José PRIVMSG posirc :こんにちは");
    query = posirc_app_find(&app, "José");
    CHECK(query != NULL && query->type == BUFFER_QUERY && query->unread == 1);
    CHECK(app.active_buffer == selected);
    posirc_app_submit(&app, "/query José");
    CHECK(posirc_app_current(&app) == query && query->unread == 0);
    (void)snprintf(query->draft, sizeof query->draft, "bozza è"); query->cursor = strlen(query->draft);
    posirc_app_cycle(&app, -1);
    CHECK(strcmp(query->draft, "bozza è") == 0);
    receive(&app, ":José!u@h NICK :Pepe");
    CHECK(posirc_app_find(&app, "José") == NULL && posirc_app_find(&app, "Pepe") == query);
    receive(&app, ":alice!u@h QUIT :bye");
    CHECK(!present(one, "alice") && !present(two, "alice"));
    receive(&app, ":op!u@h KICK #uno late :bye");
    CHECK(!present(one, "late"));
    receive(&app, ":srv 332 posirc #日本語 :\002日本の話題");
    CHECK(strcmp(two->topic, "\002日本の話題") == 0);
    posirc_app_submit(&app, "/target #日本語");
    posirc_app_submit(&app, "/close");
    CHECK(two->joined && two->type == BUFFER_CHANNEL);
    receive(&app, ":op!u@h KICK #日本語 posirc :bye");
    CHECK(!two->joined && buffer_user_count(two) == 0 && app.target[0] == '\0');
    posirc_app_submit(&app, "/close");
    CHECK(two->type == BUFFER_UNUSED && app.active_buffer == 0);
    posirc_app_submit(&app, "/query Pepe");
    posirc_app_submit(&app, "/clear");
    CHECK(query->history_count == 0 && one->history_count != 0);
    posirc_app_submit(&app, "/close");
    CHECK(query->type == BUFFER_UNUSED);
    /* Slot reuse, bounded storage and stable server slot. */
    for (index = 0; index < POSIRC_BUFFER_CAPACITY + 3; ++index) {
        char command[64];
        (void)snprintf(command, sizeof command, "/query nick%zu", index);
        posirc_app_submit(&app, command);
    }
    CHECK(app.buffers[0].type == BUFFER_SERVER);
    CHECK(posirc_app_find(&app, "nick66") == NULL);
    posirc_app_destroy(&app);
    if (failures) return EXIT_FAILURE;
    puts("OK: separate buffers, unread state, user lists, NAMES races, MODE, nicknames and Unicode routing");
    return EXIT_SUCCESS;
}
