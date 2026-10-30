/* Exercise the actual renderer against curses cells, without a display server.
 * Including this implementation keeps its rendering helpers private to the UI. */
#include "../src/ui.c"
#include <locale.h>
#include <stdlib.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL UI %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static wchar_t cell(int row, int column, attr_t *attributes, short *pair, wchar_t *combining)
{
    cchar_t value;
    wchar_t characters[CCHARW_MAX];
    CHECK(mvin_wch(row, column, &value) != ERR);
    CHECK(getcchar(&value, characters, attributes, pair, NULL) != ERR);
    *combining = characters[1];
    return characters[0];
}

static void receive(struct posirc_app *app, const char *text)
{
    char line[IRC_WIRE_CAPACITY];
    (void)snprintf(line, sizeof line, "%s", text);
    CHECK(posirc_app_receive(app, line) == 0);
}

static void pane_boundaries(struct posirc_app *app, struct ui_colors *colors)
{
    const int widths[] = {120, 100, 99, 70, 69, 60, 120};
    const char *units[] = {"long-text ", "日😀e\314\201", "\002\00304,02colored\017 "};
    struct posirc_buffer *buffer = posirc_app_current(app);
    size_t kind, i, used;
    char line[POSIRC_TEXT_CAPACITY];
    attr_t attributes;
    short pair;
    wchar_t combining;
    int row, column;
    /* A full-width write must not move to or damage the following row. */
    (void)erase();
    (void)mvaddch(11, 0, 'G');
    draw_text(colors, 10, 119, 20, "XY", A_NORMAL, 1);
    getyx(stdscr, row, column);
    CHECK(row == 10 && column == 119);
    CHECK(cell(10, 119, &attributes, &pair, &combining) == L'X');
    CHECK(cell(11, 0, &attributes, &pair, &combining) == L'G');
    for (kind = 0; kind < sizeof units / sizeof units[0]; ++kind) {
        size_t length = strlen(units[kind]);
        for (used = 0; used + length < sizeof line; used += length)
            memcpy(line + used, units[kind], length);
        line[used] = '\0';
        buffer_clear(buffer);
        CHECK(buffer_append(buffer, line) == 0);
        (void)snprintf(buffer->topic, sizeof buffer->topic, "%s", line);
        for (i = 0; i < sizeof widths / sizeof widths[0]; ++i) {
            int columns = widths[i];
            draw(app, colors, 12, columns);
            for (row = 1; row < 10; ++row) {
                if (columns >= 70)
                    CHECK((mvinch(row, 20) & (A_CHARTEXT | A_ALTCHARSET)) == (ACS_VLINE & (A_CHARTEXT | A_ALTCHARSET)));
                if (columns >= 100)
                    CHECK((mvinch(row, columns - 21) & (A_CHARTEXT | A_ALTCHARSET)) == (ACS_VLINE & (A_CHARTEXT | A_ALTCHARSET)));
            }
            if (columns >= 100) {
                CHECK(cell(1, columns - 20, &attributes, &pair, &combining) == L'U');
                CHECK(cell(2, columns - 20, &attributes, &pair, &combining) == L'@');
            }
        }
    }
}

static void wrapped_history(struct posirc_app *app, struct ui_colors *colors, int expect_color)
{
    struct posirc_buffer *buffer = posirc_app_current(app);
    attr_t attributes;
    short pair;
    wchar_t combining;
    char line[POSIRC_TEXT_CAPACITY];
    size_t i;
    buffer_clear(buffer);
    for (i = 0; i < 180; ++i) line[i] = (char)('a' + i % 26);
    line[i] = '\0';
    CHECK(buffer_append(buffer, line) == 0);
    draw(app, colors, 12, 60); /* 59 cells, four rows, no sidebars. */
    for (i = 0; i < 180; ++i)
        CHECK(cell(2 + (int)(i / 59), (int)(i % 59), &attributes, &pair, &combining) == (wchar_t)line[i]);
    CHECK(buffer->history_count == 1 && strcmp(buffer_line(buffer, 0), line) == 0);
    draw(app, colors, 7, 60); /* The tail of a message taller than the pane. */
    CHECK(cell(2, 0, &attributes, &pair, &combining) == (wchar_t)line[59]);
    scroll_history(buffer, 59, 1, 1);
    draw(app, colors, 7, 60);
    CHECK(buffer->scroll == 0 && buffer->scroll_row == 1);
    CHECK(cell(2, 0, &attributes, &pair, &combining) == L'a');
    CHECK(buffer_append(buffer, "new message while scrolled") == 0);
    draw(app, colors, 7, 60);
    CHECK(buffer->scroll == 1 && buffer->scroll_row == 1);
    CHECK(cell(2, 0, &attributes, &pair, &combining) == L'a');
    scroll_history(buffer, 59, 10000, 0);
    draw(app, colors, 7, 60);
    CHECK(buffer->scroll == 0 && buffer->scroll_row == 0);
    CHECK(cell(4, 0, &attributes, &pair, &combining) == L'n');
    scroll_history(buffer, 59, 10000, 1);
    draw(app, colors, 7, 60);
    CHECK(buffer->scroll == 1 && buffer->scroll_row == 3);
    CHECK(cell(2, 0, &attributes, &pair, &combining) == L'a');
    /* Reflow clamps a within-message position after a width change. */
    draw(app, colors, 12, 99);
    CHECK(buffer->scroll_row < wrapped_rows(buffer_line(buffer, 0), 77));
    buffer_clear(buffer);
    CHECK(buffer->scroll == 0 && buffer->scroll_row == 0);
    memset(line, 'x', 58);
    (void)snprintf(line + 58, sizeof line - 58, "\002\00304日e\314\201\017z");
    CHECK(buffer_append(buffer, line) == 0);
    draw(app, colors, 12, 60);
    CHECK(cell(2, 58, &attributes, &pair, &combining) == L' ');
    CHECK(cell(3, 0, &attributes, &pair, &combining) == 0x65e5);
    CHECK((attributes & A_BOLD) != 0 && (expect_color ? pair != 0 : pair == 0));
    CHECK(cell(3, 2, &attributes, &pair, &combining) == L'e');
    CHECK(combining == 0x301 && (attributes & A_BOLD) != 0);
    CHECK(cell(3, 3, &attributes, &pair, &combining) == L'z');
    CHECK((attributes & A_BOLD) == 0 && pair == 0);
    CHECK(wrapped_rows("", 59) == 1);
    CHECK(wrapped_rows("\002\017", 59) == 1);
    CHECK(wrapped_rows("abc", 1) == 3);
    CHECK(wrapped_rows("日", 1) == 1);
    /* Repeated ring turnover keeps a paused view bounded and drawable. */
    scroll_history(buffer, 59, 1, 1);
    for (i = 0; i < POSIRC_HISTORY_CAPACITY * 2; ++i) CHECK(buffer_append(buffer, "replacement") == 0);
    draw(app, colors, 12, 60);
    CHECK(buffer->history_count == POSIRC_HISTORY_CAPACITY);
    CHECK(buffer->scroll < POSIRC_HISTORY_CAPACITY && buffer->scroll_row == 0);
    buffer_clear(buffer);
    CHECK(buffer_append(buffer, "") == 0 && buffer_append(buffer, "\002\017") == 0);
    CHECK(buffer_append(buffer, "after empty lines") == 0);
    draw(app, colors, 12, 60);
    CHECK(cell(4, 0, &attributes, &pair, &combining) == L'a');
}

static void server_history(struct posirc_app *app, struct ui_colors *colors)
{
    struct posirc_buffer *buffer = &app->buffers[0];
    char topic[301], reply[IRC_WIRE_CAPACITY], line[128];
    const char *prefix = "#alpha | 12 users | First topic ";
    attr_t attributes;
    short pair;
    wchar_t combining;
    size_t i;
    posirc_app_select(app, 0);
    buffer_clear(buffer);
    memset(topic, 'x', sizeof topic - 1); topic[sizeof topic - 1] = '\0';
    receive(app, ":srv 321 posirc Channel :Users Name");
    (void)snprintf(reply, sizeof reply, ":srv 322 posirc #alpha 12 :First topic %s", topic);
    receive(app, reply);
    (void)snprintf(reply, sizeof reply, ":srv 322 posirc #beta 34 :Second topic %s", topic);
    receive(app, reply);
    receive(app, ":srv 323 posirc :End of LIST");
    draw(app, colors, 12, 60);
    for (i = 0; i < strlen(prefix); ++i)
        CHECK(cell(3, (int)i, &attributes, &pair, &combining) == (wchar_t)prefix[i]);
    for (i = 56; i < 59; ++i) {
        CHECK(cell(3, (int)i, &attributes, &pair, &combining) == L'*');
        CHECK(cell(4, (int)i, &attributes, &pair, &combining) == L'*');
    }
    CHECK(cell(4, 0, &attributes, &pair, &combining) == L'#');
    CHECK(cell(4, 1, &attributes, &pair, &combining) == L'b');
    CHECK(cell(5, 0, &attributes, &pair, &combining) == L'E');
    CHECK(cell(6, 0, &attributes, &pair, &combining) == L' ');
    CHECK(strstr(buffer_line(buffer, 1), topic) != NULL); /* Only the display is clipped. */
    draw(app, colors, 12, 120);
    CHECK(cell(3, 21, &attributes, &pair, &combining) == L'#');
    CHECK(cell(4, 21, &attributes, &pair, &combining) == L'#');
    for (i = 116; i < 119; ++i)
        CHECK(cell(3, (int)i, &attributes, &pair, &combining) == L'*');
    draw(app, colors, 7, 60);
    CHECK(cell(2, 0, &attributes, &pair, &combining) == L'#');
    scroll_history(buffer, 59, 1, 1);
    draw(app, colors, 7, 60);
    CHECK(buffer->scroll == 1 && buffer->scroll_row == 0);
    CHECK(cell(2, 0, &attributes, &pair, &combining) == L'C');
    CHECK(cell(4, 1, &attributes, &pair, &combining) == L'b');
    scroll_history(buffer, 59, 10000, 0);
    CHECK(buffer->scroll == 0 && buffer->scroll_row == 0);
    buffer_clear(buffer);
    memset(line, 'x', 59);
    (void)snprintf(line + 59, sizeof line - 59, "\002\017");
    CHECK(buffer_append(buffer, line) == 0);
    memset(line, 'x', 55);
    (void)snprintf(line + 55, sizeof line - 55, "\002\00304e\314\201long remainder");
    CHECK(buffer_append(buffer, line) == 0);
    draw(app, colors, 12, 60);
    CHECK(cell(2, 58, &attributes, &pair, &combining) == L'x'); /* Exact fit, no marker. */
    CHECK(cell(3, 55, &attributes, &pair, &combining) == L'e');
    CHECK(combining == 0x301 && (attributes & A_BOLD) != 0);
    CHECK(cell(3, 56, &attributes, &pair, &combining) == L'*');
    CHECK(pair == 0 && (attributes & A_BOLD) == 0); /* Marker has a neutral style. */
    buffer_clear(buffer);
    memset(line, 'x', 55);
    (void)snprintf(line + 55, sizeof line - 55, "日long remainder");
    CHECK(buffer_append(buffer, line) == 0);
    draw(app, colors, 12, 60);
    CHECK(cell(2, 55, &attributes, &pair, &combining) == L' '); /* No half-wide glyph. */
    CHECK(cell(2, 56, &attributes, &pair, &combining) == L'*');
}

static void render_case(const char *terminal, int expect_color)
{
    FILE *output = tmpfile(), *input = tmpfile();
    SCREEN *screen;
    struct posirc_app app;
    struct ui_colors colors;
    attr_t attributes;
    short pair;
    wchar_t combining;
    char terminal_name[64];
    int text_column = 21 + (int)strlen("<alice> ");
    CHECK(output != NULL && input != NULL);
    if (output == NULL || input == NULL) exit(EXIT_FAILURE);
    fprintf(stderr, "Testing curses terminal: %s\n", terminal);
    /* NetBSD's X/Open curses declaration takes a mutable terminal name. */
    (void)snprintf(terminal_name, sizeof terminal_name, "%s", terminal);
    screen = newterm(terminal_name, output, input);
    CHECK(screen != NULL);
    if (screen == NULL) exit(EXIT_FAILURE);
    colors_init(&colors);
    posirc_app_init(&app);
    receive(&app, ":posirc!u@h JOIN :#room");
    receive(&app, ":srv 353 posirc = #room :@posirc +alice");
    receive(&app, ":srv 366 posirc #room :end");
    receive(&app, ":srv 332 posirc #room :Topic 日本語");
    receive(&app, ":alice!u@h PRIVMSG #room :\002\00304,02R\017 e\314\201 日 😀");
    draw(&app, &colors, 24, 120);
    CHECK(cell(1, 0, &attributes, &pair, &combining) == L'B');
    CHECK(cell(1, 100, &attributes, &pair, &combining) == L'U');
    CHECK(cell(2, 100, &attributes, &pair, &combining) == L'@');
    /* JOIN and topic events precede the chat line in its private history. */
    CHECK(cell(4, text_column, &attributes, &pair, &combining) == L'R');
    CHECK((attributes & A_BOLD) != 0);
    CHECK(expect_color ? pair != 0 : pair == 0);
    CHECK(cell(4, text_column + 2, &attributes, &pair, &combining) == L'e');
    CHECK(combining == 0x301 && pair == 0 && (attributes & A_BOLD) == 0);
    CHECK(cell(4, text_column + 4, &attributes, &pair, &combining) == 0x65e5);
    CHECK(cell(4, text_column + 7, &attributes, &pair, &combining) == 0x1f600);
    /* Resizing hides sidebars while retaining the conversation. */
    draw(&app, &colors, 12, 60);
    CHECK(cell(1, 0, &attributes, &pair, &combining) == L'T');
    pane_boundaries(&app, &colors);
    wrapped_history(&app, &colors, expect_color);
    server_history(&app, &colors);
    posirc_app_destroy(&app);
    (void)endwin();
    delscreen(screen);
    (void)fclose(output); (void)fclose(input);
}

static void input_history(void)
{
    struct posirc_app app;
    struct posirc_buffer *buffer;
    char last_input[POSIRC_TEXT_CAPACITY] = "";
    size_t count;
    posirc_app_init(&app);
    buffer = posirc_app_current(&app);
    (void)snprintf(buffer->draft, sizeof buffer->draft, "unfinished");
    buffer->cursor = 3;
    recall_input(buffer, last_input);
    CHECK(strcmp(buffer->draft, "unfinished") == 0 && buffer->cursor == 3);
    (void)snprintf(buffer->draft, sizeof buffer->draft, "/nick tester");
    submit_input(&app, last_input);
    CHECK(strcmp(app.nick, "tester") == 0 && strcmp(last_input, "/nick tester") == 0);
    CHECK(buffer->draft[0] == '\0' && buffer->cursor == 0);
    recall_input(buffer, last_input);
    CHECK(strcmp(buffer->draft, "/nick tester") == 0 && buffer->cursor == strlen(buffer->draft));
    text_delete(buffer->draft, &buffer->cursor, 1);
    CHECK(strcmp(last_input, "/nick tester") == 0); /* Editing the recalled line leaves the slot alone. */
    (void)snprintf(buffer->draft, sizeof buffer->draft, "/query friend");
    submit_input(&app, last_input);
    buffer = posirc_app_current(&app);
    CHECK(strcmp(buffer->name, "friend") == 0 && buffer->draft[0] == '\0');
    recall_input(buffer, last_input);
    CHECK(strcmp(buffer->draft, "/query friend") == 0);
    (void)snprintf(buffer->draft, sizeof buffer->draft, "caffè 日 😀");
    submit_input(&app, last_input);
    CHECK(strcmp(last_input, "caffè 日 😀") == 0);
    count = buffer->history_count;
    submit_input(&app, last_input); /* Empty Enter must not resend the saved entry. */
    CHECK(buffer->history_count == count && strcmp(last_input, "caffè 日 😀") == 0);
    posirc_app_select(&app, 0);
    buffer = posirc_app_current(&app);
    recall_input(buffer, last_input);
    CHECK(strcmp(buffer->draft, "caffè 日 😀") == 0 && buffer->cursor == strlen(last_input));
    recall_input(buffer, last_input);
    CHECK(strcmp(buffer->draft, "caffè 日 😀") == 0); /* Up has no older entries. */
    memset(buffer->draft, 'x', sizeof buffer->draft - 1);
    buffer->draft[sizeof buffer->draft - 1] = '\0';
    submit_input(&app, last_input);
    recall_input(buffer, last_input);
    CHECK(strlen(last_input) == POSIRC_TEXT_CAPACITY - 1 && strcmp(buffer->draft, last_input) == 0);
    CHECK(buffer->cursor == POSIRC_TEXT_CAPACITY - 1);
    posirc_app_destroy(&app);
}

int main(void)
{
    (void)alarm(60);
    CHECK(setlocale(LC_ALL, "C.UTF-8") != NULL || setlocale(LC_ALL, "en_US.UTF-8") != NULL);
    CHECK(setenv("LINES", "24", 1) == 0 && setenv("COLUMNS", "120", 1) == 0);
    input_history();
    render_case("xterm-256color", 1);
    render_case("vt100", 0);
    if (failures) return EXIT_FAILURE;
    puts("OK: wide curses cells, combining marks, CJK, emoji, sidebars and color/monochrome rendering");
    return EXIT_SUCCESS;
}
