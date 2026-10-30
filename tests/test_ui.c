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
    int text_column = 21 + (int)strlen("[#room] <alice> ");
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
    posirc_app_destroy(&app);
    (void)endwin();
    delscreen(screen);
    (void)fclose(output); (void)fclose(input);
}

int main(void)
{
    (void)alarm(60);
    CHECK(setlocale(LC_ALL, "C.UTF-8") != NULL || setlocale(LC_ALL, "en_US.UTF-8") != NULL);
    CHECK(setenv("LINES", "24", 1) == 0 && setenv("COLUMNS", "120", 1) == 0);
    render_case("xterm-256color", 1);
    render_case("vt100", 0);
    if (failures) return EXIT_FAILURE;
    puts("OK: wide curses cells, combining marks, CJK, emoji, sidebars and color/monochrome rendering");
    return EXIT_SUCCESS;
}
