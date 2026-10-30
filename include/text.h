#ifndef POSIRC_TEXT_H
#define POSIRC_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* Locale-independent UTF-8 on the wire. Decode returns bytes consumed (one
 * replacement character for invalid input), or zero at the end of a string. */
size_t utf8_decode(const char *text, uint32_t *codepoint);
size_t utf8_encode(uint32_t codepoint, char output[5]);
int utf8_valid(const char *text);
struct utf8_input {
    char bytes[5];
    size_t length, expected;
};
/* Incremental terminal input: 1 scalar ready, 0 incomplete, -1 invalid.
 * Invalid continuation resets the sequence and reconsiders the current byte. */
int utf8_input_byte(struct utf8_input *input, unsigned char byte, uint32_t *codepoint);
size_t utf8_previous(const char *text, size_t offset);
/* Editing offsets are UTF-8 byte boundaries; deletion removes one scalar. */
int text_insert(char *text, size_t capacity, size_t *cursor, uint32_t codepoint);
void text_delete(char *text, size_t *cursor, int backward);
int text_format_code(uint32_t codepoint);
/* Preserve valid UTF-8 and optional IRC formatting; reject terminal controls.
 * Never split a UTF-8 sequence, even when the destination is too small. */
void text_clean(char *output, size_t capacity, const char *input, int formatting);

struct text_style {
    int foreground, background; /* RGB, or -1 for terminal default. */
    unsigned int attributes;
};
#define TEXT_BOLD 1u
#define TEXT_UNDERLINE 2u
#define TEXT_REVERSE 4u
#define TEXT_ITALIC 8u

struct text_reader {
    const char *cursor;
    struct text_style style;
};
void text_reader_init(struct text_reader *reader, const char *text);
/* Consume formatting and return the next Unicode scalar and its current style. */
int text_next(struct text_reader *reader, uint32_t *codepoint);
int text_palette(unsigned int index);

#endif
