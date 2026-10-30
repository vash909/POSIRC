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
    posirc_app_destroy(&app);
    (void)endwin();
    delscreen(screen);
    (void)fclose(output); (void)fclose(input);
}

int main(void)
{
    CHECK(setlocale(LC_ALL, "C.UTF-8") != NULL || setlocale(LC_ALL, "en_US.UTF-8") != NULL);
    CHECK(setenv("LINES", "24", 1) == 0 && setenv("COLUMNS", "120", 1) == 0);
    render_case("xterm-256color", 1);
    render_case("vt100", 0);
    if (failures) return EXIT_FAILURE;
    puts("OK: wide curses cells, combining marks, CJK, emoji, sidebars and color/monochrome rendering");
    return EXIT_SUCCESS;
}
