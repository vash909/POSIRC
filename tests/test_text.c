#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void check_utf8(void)
{
    char cleaned[64], tiny[4], input[32] = "A";
    size_t cursor = 1;
    uint32_t c;
    struct utf8_input decoder = {{0}, 0, 0};
    CHECK(utf8_input_byte(&decoder, 0xc3, &c) == 0);
    CHECK(utf8_input_byte(&decoder, 0xa9, &c) == 1 && c == 0xe9);
    CHECK(utf8_input_byte(&decoder, 0xf0, &c) == 0);
    CHECK(utf8_input_byte(&decoder, 0x9f, &c) == 0);
    CHECK(utf8_input_byte(&decoder, 0x98, &c) == 0);
    CHECK(utf8_input_byte(&decoder, 0x80, &c) == 1 && c == 0x1f600);
    CHECK(utf8_input_byte(&decoder, 0xc3, &c) == 0);
    CHECK(utf8_input_byte(&decoder, 'X', &c) == 1 && c == 'X');
    CHECK(utf8_input_byte(&decoder, 0x80, &c) == -1);
    CHECK(utf8_valid("caffè 日本語 😀 e\314\201"));
    CHECK(!utf8_valid("\300\200"));
    CHECK(!utf8_valid("\355\240\200"));
    CHECK(!utf8_valid("\364\220\200\200"));
    CHECK(!utf8_valid("\342\202"));
    CHECK(utf8_decode("😀", &c) == 4 && c == 0x1f600);
    text_clean(cleaned, sizeof cleaned, "è\033[31m\00304red\017\xff", 1);
    CHECK(strcmp(cleaned, "è?[31m\00304red\017�") == 0);
    text_clean(tiny, sizeof tiny, "éé", 1);
    CHECK(strcmp(tiny, "é") == 0 && utf8_valid(tiny));
    CHECK(text_insert(input, sizeof input, &cursor, 0x1f600) == 0);
    CHECK(strcmp(input, "A😀") == 0 && cursor == 5);
    cursor = utf8_previous(input, cursor);
    CHECK(cursor == 1);
    CHECK(text_insert(input, sizeof input, &cursor, 0x65e5) == 0);
    CHECK(strcmp(input, "A日😀") == 0);
    text_delete(input, &cursor, 0);
    CHECK(strcmp(input, "A日") == 0);
    text_delete(input, &cursor, 1);
    CHECK(strcmp(input, "A") == 0 && cursor == 1);
    CHECK(text_insert(input, 2, &cursor, 0xe9) == -1 && strcmp(input, "A") == 0);
    CHECK(text_insert(input, sizeof input, &cursor, 0xd800) == -1);
}

static void check_formatting(void)
{
    struct text_reader reader;
    uint32_t c;
    text_reader_init(&reader, "\002\00304,02é\017日\00399!\00400ff00,0000ffX\003,Y");
    CHECK(text_next(&reader, &c) && c == 0xe9);
    CHECK(reader.style.attributes == TEXT_BOLD);
    CHECK(reader.style.foreground == 0xff0000 && reader.style.background == 0x00007f);
    CHECK(text_next(&reader, &c) && c == 0x65e5);
    CHECK(reader.style.attributes == 0 && reader.style.foreground == -1 && reader.style.background == -1);
    CHECK(text_next(&reader, &c) && c == '!');
    CHECK(reader.style.foreground == -1);
    CHECK(text_next(&reader, &c) && c == 'X');
    CHECK(reader.style.foreground == 0x00ff00 && reader.style.background == 0x0000ff);
    CHECK(text_next(&reader, &c) && c == ',' && reader.style.foreground == -1);
    CHECK(text_next(&reader, &c) && c == 'Y');
    CHECK(!text_next(&reader, &c));
    text_reader_init(&reader, "\00304,rest\00412ZZZZx");
    CHECK(text_next(&reader, &c) && c == ',' && reader.style.foreground == 0xff0000);
    CHECK(text_next(&reader, &c) && c == 'r');
    text_reader_init(&reader, "\035\037\026X\035\037\026Y");
    CHECK(text_next(&reader, &c) && reader.style.attributes == (TEXT_ITALIC | TEXT_UNDERLINE | TEXT_REVERSE));
    CHECK(text_next(&reader, &c) && reader.style.attributes == 0);
    CHECK(text_palette(16) == 0x470000 && text_palette(98) == 0xffffff && text_palette(99) == -1);
    text_reader_init(&reader, "\00304\00312");
    CHECK(!text_next(&reader, &c));
}

/* Frozen scalar sanitizer for differential checks of the ASCII fast path. */
static void scalar_clean(char *out, size_t capacity, const char *in, int formatting)
{
    size_t used = 0;
    if (capacity == 0) return;
    while (*in) {
        uint32_t c;
        char encoded[5];
        size_t n = utf8_decode(in, &c);
        in += n;
        if ((c < 32 || (c >= 127 && c <= 159) || c == 0x2028 || c == 0x2029 ||
             (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069)) &&
            !(formatting && text_format_code(c))) c = '?';
        n = utf8_encode(c, encoded);
        if (used + n >= capacity) break;
        memcpy(out + used, encoded, n);
        used += n;
    }
    out[used] = '\0';
}

static void differential_clean(void)
{
    unsigned int seed = 7;
    size_t trial, capacity, i;
    int formatting;
    for (trial = 0; trial < 2000; ++trial) {
        char input[64], expected[70], actual[70];
        for (i = 0; i < sizeof input - 1; ++i) {
            seed = seed * 1664525u + 1013904223u;
            input[i] = trial < 256 ? (char)trial : (char)(seed >> 24);
        }
        input[sizeof input - 1] = '\0';
        for (capacity = 0; capacity <= sizeof actual; ++capacity) {
            for (formatting = 0; formatting <= 1; ++formatting) {
                memset(actual, 0x55, sizeof actual);
                memset(expected, 0x55, sizeof expected);
                scalar_clean(expected, capacity, input, formatting);
                text_clean(actual, capacity, input, formatting);
                CHECK(memcmp(actual, expected, sizeof actual) == 0);
            }
        }
    }
}

int main(void)
{
    differential_clean();
    check_utf8();
    check_formatting();
    if (failures) return EXIT_FAILURE;
    puts("OK: UTF-8 validation, safe truncation, editing and IRC formatting");
    return EXIT_SUCCESS;
}
