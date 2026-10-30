#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED 1
#endif

#include <curses.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "app.h"
#include "net.h"
#include "text.h"
#include "ui.h"

#define UI_PAIR_CAPACITY 256
struct ui_colors {
    int enabled, defaults, count;
    int rgb[256];
    struct { short foreground, background; } pairs[UI_PAIR_CAPACITY];
    short used;
};

static void colors_init(struct ui_colors *colors)
{
    int index;
    memset(colors, 0, sizeof *colors);
    if (!has_colors() || start_color() == ERR) return;
    colors->enabled = 1;
#ifdef NCURSES_VERSION
    colors->defaults = use_default_colors() == OK;
#endif
    colors->count = COLORS < 256 ? COLORS : 256;
    for (index = 0; index < colors->count; ++index) {
        short r, g, b;
        if (color_content((short)index, &r, &g, &b) == OK)
            colors->rgb[index] = ((r * 255 / 1000) << 16) | ((g * 255 / 1000) << 8) | (b * 255 / 1000);
        else colors->rgb[index] = 0;
    }
}

static long color_distance(int a, int b)
{
    long r = ((a >> 16) & 255) - ((b >> 16) & 255);
    long g = ((a >> 8) & 255) - ((b >> 8) & 255);
    long blue = (a & 255) - (b & 255);
    return r * r + g * g + blue * blue;
}

static short closest_color(const struct ui_colors *colors, int rgb, int background)
{
    int index, best = 0;
    long distance = LONG_MAX;
    if (rgb < 0) return colors->defaults ? -1 : (short)(background ? COLOR_BLACK : COLOR_WHITE);
    for (index = 0; index < colors->count; ++index) {
        long candidate = color_distance(rgb, colors->rgb[index]);
        if (candidate < distance) { distance = candidate; best = index; }
    }
    return (short)best;
}

static short color_pair(struct ui_colors *colors, const struct text_style *style)
{
    short foreground, background, index;
    if (!colors->enabled || (style->foreground < 0 && style->background < 0)) return 0;
    /* First consult RGB values through the stable pair cache. Pairs are never
     * redefined: doing so would recolor cells already on the screen. */
    foreground = closest_color(colors, style->foreground, 0);
    background = closest_color(colors, style->background, 1);
    for (index = 1; index <= colors->used; ++index)
        if (colors->pairs[index].foreground == foreground && colors->pairs[index].background == background)
            return index;
    if (colors->used + 1 >= UI_PAIR_CAPACITY || colors->used + 1 >= COLOR_PAIRS) return 0;
    index = (short)(colors->used + 1);
    if (init_pair(index, foreground, background) == ERR) return 0;
    colors->used = index;
    colors->pairs[index].foreground = foreground;
    colors->pairs[index].background = background;
    return index;
}

static attr_t style_attributes(const struct text_style *style)
{
    attr_t attributes = A_NORMAL;
    if (style->attributes & TEXT_BOLD) attributes |= A_BOLD;
    if (style->attributes & TEXT_UNDERLINE) attributes |= A_UNDERLINE;
    if (style->attributes & TEXT_REVERSE) attributes |= A_REVERSE;
#ifdef A_ITALIC
    if (style->attributes & TEXT_ITALIC) attributes |= A_ITALIC;
#endif
    return attributes;
}

static int scalar_width(uint32_t c)
{
    int width;
    if (c > (uint32_t)WCHAR_MAX) return 1;
    width = wcwidth((wchar_t)c);
    return width < 0 ? 1 : width;
}

/* Only curses receives output. No server string is ever interpreted as a
 * terminal escape sequence. Clip in terminal cells, not bytes or codepoints. */
static void draw_text(struct ui_colors *colors, int row, int column, int width,
                      const char *text, attr_t extra, int formatting)
{
    struct text_reader reader;
    char plain[POSIRC_TEXT_CAPACITY * 2];
    uint32_t codepoint;
    int used = 0, previous_foreground = -2, previous_background = -2;
    int screen_rows, screen_columns;
    short pair = 0;
    getmaxyx(stdscr, screen_rows, screen_columns);
    if (row < 0 || row >= screen_rows || column < 0 || column >= screen_columns) return;
    if (width > screen_columns - column) width = screen_columns - column;
    if (width <= 0) return;
    if (!formatting) { text_clean(plain, sizeof plain, text, 0); text = plain; }
    text_reader_init(&reader, text);
    while (text_next(&reader, &codepoint)) {
        wchar_t sequence[CCHARW_MAX];
        cchar_t character;
        int cells = scalar_width(codepoint);
        size_t count = 1;
        struct text_style style = reader.style;
        if (codepoint > (uint32_t)WCHAR_MAX || wcwidth((wchar_t)codepoint) < 0) codepoint = '?';
        if (cells == 0) { sequence[0] = L' '; sequence[count++] = (wchar_t)codepoint; cells = 1; }
        else sequence[0] = (wchar_t)codepoint;
        while (count < CCHARW_MAX - 1) {
            struct text_reader next = reader;
            uint32_t combining;
            if (!text_next(&next, &combining) || scalar_width(combining) != 0 ||
                next.style.foreground != style.foreground || next.style.background != style.background ||
                next.style.attributes != style.attributes) break;
            sequence[count++] = (wchar_t)combining;
            reader = next;
        }
        sequence[count] = L'\0';
        if (used + cells > width) break;
        if (style.foreground != previous_foreground || style.background != previous_background) {
            pair = color_pair(colors, &style);
            previous_foreground = style.foreground;
            previous_background = style.background;
        }
        if (setcchar(&character, sequence, style_attributes(&style) | extra, pair, NULL) == OK)
            /* Copy cells without curses' cursor advance/wrap side effects.
             * Layout, including any wrapping, belongs to this renderer. */
            (void)mvadd_wchnstr(row, column + used, &character, 1);
        used += cells;
    }
    (void)attrset(A_NORMAL);
}

static int draft_width(uint32_t c)
{
    return c < 32 ? 2 : scalar_width(c);
}

static void draw_input(struct ui_colors *colors, const struct posirc_buffer *buffer, int row, int columns)
{
    size_t offset = 0, index = 0, used = 0;
    int cells = 0, cursor_cells;
    char visible[POSIRC_TEXT_CAPACITY * 2];
    uint32_t c;
    while (index < buffer->cursor) {
        index += utf8_decode(buffer->draft + index, &c);
        cells += draft_width(c);
    }
    while (cells > columns - 4 && offset < buffer->cursor) {
        offset += utf8_decode(buffer->draft + offset, &c);
        cells -= draft_width(c);
    }
    cursor_cells = cells;
    index = offset;
    while (buffer->draft[index] != '\0' && used + 5 < sizeof visible) {
        char encoded[5];
        size_t length = utf8_decode(buffer->draft + index, &c);
        index += length;
        if (c < 32) { visible[used++] = '^'; visible[used++] = (char)(c + 64); }
        else { length = utf8_encode(c, encoded); memcpy(visible + used, encoded, length); used += length; }
    }
    visible[used] = '\0';
    draw_text(colors, row, 0, 2, "> ", A_NORMAL, 0);
    draw_text(colors, row, 2, columns - 3, visible, A_NORMAL, 0);
    (void)move(row, 2 + cursor_cells);
}

static void draw(struct posirc_app *app, struct ui_colors *colors, int rows, int columns)
{
    struct posirc_buffer *buffer = posirc_app_current(app);
    char label[POSIRC_TEXT_CAPACITY];
    int left = columns >= 70 ? 20 : 0;
    int right = columns >= 100 && buffer->type == BUFFER_CHANNEL ? 20 : 0;
    int center = left ? left + 1 : 0;
    int width = columns - center - (right ? right + 1 : 0) - 1;
    int body = rows - 4;
    size_t end, first, index, count;
    (void)erase();
    if (rows < 7 || columns < 24) {
        if (rows > 0 && columns > 1) draw_text(colors, 0, 0, columns - 1, "Terminal too small", A_NORMAL, 0);
        (void)refresh();
        return;
    }
    (void)snprintf(label, sizeof label, " POSIRC | %s%s | %s | %s%s", net_status(app->net),
        net_ready(app->net) && !app->registered ? " / registering" : "", app->nick, buffer->name,
        buffer->type == BUFFER_CHANNEL && !buffer->joined ? " [left]" : "");
    draw_text(colors, 0, 0, columns - 1, label, A_REVERSE, 0);
    draw_text(colors, 1, center, width, buffer->topic[0] ? buffer->topic : buffer->name, A_NORMAL, 1);
    if (left) {
        size_t ordinal = 0, selected = 0, total = 0;
        for (index = 0; index < POSIRC_BUFFER_CAPACITY; ++index) {
            if (app->buffers[index].type == BUFFER_UNUSED) continue;
            if (index == app->active_buffer) selected = total;
            ++total;
        }
        first = selected >= (size_t)(body - 1) ? selected - (size_t)(body - 2) : 0;
        draw_text(colors, 1, 0, left, "Buffers", A_BOLD, 0);
        for (index = 0; index < POSIRC_BUFFER_CAPACITY; ++index) {
            struct posirc_buffer *entry = &app->buffers[index];
            if (entry->type == BUFFER_UNUSED) continue;
            if (ordinal >= first && ordinal - first < (size_t)body) {
                (void)snprintf(label, sizeof label, "%2zu%c %s%s", index, entry->unread ? '*' : ' ', entry->name,
                              entry->type == BUFFER_CHANNEL && !entry->joined ? " [left]" : "");
                draw_text(colors, 2 + (int)(ordinal - first), 0, left, label,
                          index == app->active_buffer ? A_REVERSE : A_NORMAL, 0);
            }
            ++ordinal;
        }
    }
    if (right) {
        buffer_users_sort(buffer);
        count = buffer_user_count(buffer);
        if (buffer->user_scroll >= count) buffer->user_scroll = count > 0 ? count - 1 : 0;
        (void)snprintf(label, sizeof label, "Users %zu%s", count, buffer->users_truncated ? "+" : "");
        draw_text(colors, 1, columns - right, right - 1, label, A_BOLD, 0);
        for (index = buffer->user_scroll; index < count && index - buffer->user_scroll < (size_t)body; ++index) {
            unsigned int bit;
            char symbol = ' ';
            for (bit = 0; bit < strlen(app->prefix_symbols); ++bit)
                if (buffer->users[index].modes & (1u << bit)) { symbol = app->prefix_symbols[bit]; break; }
            (void)snprintf(label, sizeof label, "%c%s", symbol, buffer->users[index].nick);
            draw_text(colors, 2 + (int)(index - buffer->user_scroll), columns - right, right - 1, label, A_NORMAL, 0);
        }
    }
    if (buffer->scroll >= buffer->history_count) buffer->scroll = buffer->history_count > 0 ? buffer->history_count - 1 : 0;
    end = buffer->history_count - buffer->scroll;
    first = end > (size_t)body ? end - (size_t)body : 0;
    for (index = first; index < end; ++index)
        draw_text(colors, 2 + (int)(index - first), center, width, buffer_line(buffer, index), A_NORMAL, 1);
    /* Borders are structural cells: restore them after all pane content. */
    if (left) (void)mvvline(1, left, ACS_VLINE, rows - 3);
    if (right) (void)mvvline(1, columns - right - 1, ACS_VLINE, rows - 3);
    (void)snprintf(label, sizeof label, "%s | Tab: buffers | PgUp/Dn: history | F7/F8: users | /help%s",
                   buffer->scroll ? "History" : "Live", app->dropped_lines ? " | memory limit: lines dropped" : "");
    draw_text(colors, rows - 2, 0, columns - 1, label, A_DIM, 0);
    draw_input(colors, buffer, rows - 1, columns);
    (void)refresh();
}

int posirc_ui_run(struct posirc_app *app, const volatile sig_atomic_t *stop_requested)
{
    struct ui_colors colors;
    struct utf8_input input = {{0}, 0, 0};
    int rows, columns, old_rows = -1, old_columns = -1, dirty = 1;
    unsigned long revision = app->revision;
    if (initscr() == NULL) { fputs("POSIRC: cannot initialize curses.\n", stderr); return -1; }
    if (cbreak() == ERR || noecho() == ERR || keypad(stdscr, TRUE) == ERR) {
        (void)endwin(); fputs("POSIRC: cannot configure terminal.\n", stderr); return -1;
    }
    colors_init(&colors);
    if (wcwidth(L'\u00e9') < 0) {
        (void)buffer_append(&app->buffers[0], "Unicode display requires a UTF-8 locale, for example LANG=C.UTF-8.");
        ++app->revision;
    }
    timeout(0);
    while (app->running && !*stop_requested) {
        struct posirc_buffer *buffer = posirc_app_current(app);
        wint_t key;
        int kind;
        getmaxyx(stdscr, rows, columns);
        if (dirty || app->revision != revision || rows != old_rows || columns != old_columns) {
            draw(app, &colors, rows, columns);
            old_rows = rows; old_columns = columns; revision = app->revision; dirty = 0;
        }
        /* wget_wch in no-delay mode can discard an incomplete multibyte
         * sequence. Preserve UTF-8 bytes across polls, including slow SSH
         * input, while curses continues to decode function-key sequences. */
        kind = getch();
        if (kind == ERR) { posirc_app_step(app, STDIN_FILENO, 100); continue; }
        if (kind >= KEY_MIN) {
            key = (wint_t)kind;
            kind = KEY_CODE_YES;
            input.length = 0;
        } else {
            uint32_t codepoint;
            int ready = utf8_input_byte(&input, (unsigned char)kind, &codepoint);
            if (ready <= 0) { posirc_app_step(app, -1, 0); continue; }
            key = (wint_t)codepoint;
            kind = OK;
        }
        dirty = 1;
        if (kind == KEY_CODE_YES) {
            uint32_t ignored;
            size_t page = rows > 4 ? (size_t)(rows - 4) : 1;
            if (key == KEY_F(2) || key == KEY_BTAB) posirc_app_cycle(app, -1);
            else if (key == KEY_F(3)) posirc_app_cycle(app, 1);
            else if (key == KEY_LEFT) buffer->cursor = utf8_previous(buffer->draft, buffer->cursor);
            else if (key == KEY_RIGHT) buffer->cursor += utf8_decode(buffer->draft + buffer->cursor, &ignored);
            else if (key == KEY_HOME) buffer->cursor = 0;
            else if (key == KEY_END) buffer->cursor = strlen(buffer->draft);
            else if (key == KEY_BACKSPACE) text_delete(buffer->draft, &buffer->cursor, 1);
            else if (key == KEY_DC) text_delete(buffer->draft, &buffer->cursor, 0);
            else if (key == KEY_PPAGE) buffer->scroll += page;
            else if (key == KEY_NPAGE) buffer->scroll = buffer->scroll > page ? buffer->scroll - page : 0;
            else if (key == KEY_F(7)) buffer->user_scroll = buffer->user_scroll > page ? buffer->user_scroll - page : 0;
            else if (key == KEY_F(8)) buffer->user_scroll += page;
            else if (key == KEY_ENTER) kind = OK, key = '\n';
#ifdef KEY_RESIZE
            else if (key == KEY_RESIZE) (void)clearok(stdscr, TRUE);
#endif
        }
        if (kind == OK) {
            if (key == '\n' || key == '\r') {
                char submitted[POSIRC_TEXT_CAPACITY];
                (void)snprintf(submitted, sizeof submitted, "%s", buffer->draft);
                buffer->draft[0] = '\0'; buffer->cursor = buffer->scroll = 0;
                posirc_app_submit(app, submitted);
            } else if (key == '\t' || key == 14) posirc_app_cycle(app, 1);
            else if (key == 16) posirc_app_cycle(app, -1);
            else if (key == 8 || key == 127) text_delete(buffer->draft, &buffer->cursor, 1);
            else if (key == 21) { buffer->draft[0] = '\0'; buffer->cursor = 0; }
            else if (key == 12) (void)clearok(stdscr, TRUE);
            else {
                uint32_t codepoint = (uint32_t)key;
                if (key == 11) codepoint = 3; /* Ctrl-K: IRC color. */
                if (key == 20) codepoint = 29; /* Ctrl-T: italic. */
                if (key == 18) codepoint = 22; /* Ctrl-R: reverse. */
                if ((key >= 32 && iswprint(key)) || text_format_code(codepoint))
                    (void)text_insert(buffer->draft, sizeof buffer->draft, &buffer->cursor, codepoint);
            }
        }
        posirc_app_step(app, -1, 0);
    }
    (void)endwin();
    return 0;
}
