#include <string.h>

#include "text.h"

size_t utf8_decode(const char *text, uint32_t *codepoint)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t length, index;
    uint32_t value, minimum;
    *codepoint = 0xfffd;
    if (*s == 0) return 0;
    if (*s < 0x80) { *codepoint = *s; return 1; }
    if (*s >= 0xc2 && *s <= 0xdf) { length = 2; value = *s & 31u; minimum = 0x80; }
    else if (*s >= 0xe0 && *s <= 0xef) { length = 3; value = *s & 15u; minimum = 0x800; }
    else if (*s >= 0xf0 && *s <= 0xf4) { length = 4; value = *s & 7u; minimum = 0x10000; }
    else return 1;
    for (index = 1; index < length; ++index) {
        if ((s[index] & 0xc0u) != 0x80u) return 1;
        value = (value << 6) | (s[index] & 63u);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return 1;
    *codepoint = value;
    return length;
}

size_t utf8_encode(uint32_t c, char output[5])
{
    size_t length;
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    if (c < 0x80) { output[0] = (char)c; length = 1; }
    else if (c < 0x800) {
        output[0] = (char)(0xc0u | (c >> 6)); output[1] = (char)(0x80u | (c & 63u)); length = 2;
    } else if (c < 0x10000) {
        output[0] = (char)(0xe0u | (c >> 12)); output[1] = (char)(0x80u | ((c >> 6) & 63u));
        output[2] = (char)(0x80u | (c & 63u)); length = 3;
    } else {
        output[0] = (char)(0xf0u | (c >> 18)); output[1] = (char)(0x80u | ((c >> 12) & 63u));
        output[2] = (char)(0x80u | ((c >> 6) & 63u)); output[3] = (char)(0x80u | (c & 63u)); length = 4;
    }
    output[length] = '\0';
    return length;
}

int utf8_valid(const char *text)
{
    while (*text != '\0') {
        uint32_t c;
        size_t length = utf8_decode(text, &c);
        if (length == 1 && (unsigned char)*text >= 0x80) return 0;
        text += length;
    }
    return 1;
}

int utf8_input_byte(struct utf8_input *input, unsigned char byte, uint32_t *codepoint)
{
    if (input->length != 0 && (byte & 0xc0u) != 0x80u) input->length = 0;
    if (input->length == 0) {
        if (byte < 0x80) { *codepoint = byte; return 1; }
        if (byte >= 0xc2 && byte <= 0xdf) input->expected = 2;
        else if (byte >= 0xe0 && byte <= 0xef) input->expected = 3;
        else if (byte >= 0xf0 && byte <= 0xf4) input->expected = 4;
        else return -1;
    }
    input->bytes[input->length++] = (char)byte;
    input->bytes[input->length] = '\0';
    if (input->length < input->expected) return 0;
    input->length = 0;
    return utf8_decode(input->bytes, codepoint) == input->expected ? 1 : -1;
}

size_t utf8_previous(const char *text, size_t offset)
{
    if (offset > 0) {
        --offset;
        while (offset > 0 && ((unsigned char)text[offset] & 0xc0u) == 0x80u) --offset;
    }
    return offset;
}

int text_insert(char *text, size_t capacity, size_t *cursor, uint32_t codepoint)
{
    char encoded[5];
    size_t length = strlen(text), count = utf8_encode(codepoint, encoded);
    if (codepoint == 0 || count == 0 || length + count >= capacity || *cursor > length) return -1;
    memmove(text + *cursor + count, text + *cursor, length - *cursor + 1);
    memcpy(text + *cursor, encoded, count);
    *cursor += count;
    return 0;
}

void text_delete(char *text, size_t *cursor, int backward)
{
    size_t length = strlen(text), start = *cursor, end = start;
    uint32_t ignored;
    if (start > length) return;
    if (backward) start = utf8_previous(text, start);
    else if (start < length) end += utf8_decode(text + start, &ignored);
    memmove(text + start, text + end, length - end + 1);
    *cursor = start;
}

int text_format_code(uint32_t c)
{
    return c == 2 || c == 3 || c == 4 || c == 15 || c == 17 || c == 22 ||
           c == 29 || c == 30 || c == 31;
}

static int unsafe(uint32_t c)
{
    return c < 32 || (c >= 127 && c <= 159) || c == 0x2028 || c == 0x2029 ||
           (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
}

void text_clean(char *output, size_t capacity, const char *input, int formatting)
{
    size_t used = 0;
    if (capacity == 0) return;
    while (*input != '\0') {
        /* Printable ASCII needs neither Unicode decoding nor sanitizing.
         * Stop before controls/high bytes and keep the scalar path unchanged. */
        const char *start = input;
        size_t room = capacity - used - 1;
        while ((unsigned char)*input >= 32 && (unsigned char)*input < 127 &&
               (size_t)(input - start) < room) ++input;
        if (input != start) {
            size_t length = (size_t)(input - start);
            memcpy(output + used, start, length);
            used += length;
        }
        if (*input == '\0' || used == capacity - 1) break;
        uint32_t c;
        char encoded[5];
        size_t length = utf8_decode(input, &c);
        input += length;
        if (unsafe(c) && !(formatting && text_format_code(c))) c = '?';
        length = utf8_encode(c, encoded);
        if (used + length >= capacity) break;
        memcpy(output + used, encoded, length);
        used += length;
    }
    output[used] = '\0';
}

int text_palette(unsigned int index)
{
    /* IRC's standard and extended palette. Terminal approximation is separate. */
    static const int palette[99] = {
        0xffffff,0x000000,0x00007f,0x009300,0xff0000,0x7f0000,0x9c009c,0xfc7f00,
        0xffff00,0x00fc00,0x009393,0x00ffff,0x0000fc,0xff00ff,0x7f7f7f,0xd2d2d2,
        0x470000,0x472100,0x474700,0x324700,0x004700,0x00472c,0x004747,0x002747,0x000047,0x2e0047,0x470047,0x47002a,
        0x740000,0x743a00,0x747400,0x517400,0x007400,0x007449,0x007474,0x004074,0x000074,0x4b0074,0x740074,0x740045,
        0xb50000,0xb56300,0xb5b500,0x7db500,0x00b500,0x00b571,0x00b5b5,0x0063b5,0x0000b5,0x7500b5,0xb500b5,0xb5006b,
        0xff0000,0xff8c00,0xffff00,0xb2ff00,0x00ff00,0x00ffa0,0x00ffff,0x008cff,0x0000ff,0xa500ff,0xff00ff,0xff0098,
        0xff5959,0xffb459,0xffff71,0xcfff60,0x6fff6f,0x65ffc9,0x6dffff,0x59b4ff,0x5959ff,0xc459ff,0xff66ff,0xff59bc,
        0xff9c9c,0xffd39c,0xffff9c,0xe2ff9c,0x9cff9c,0x9cffdb,0x9cffff,0x9cd3ff,0x9c9cff,0xdc9cff,0xff9cff,0xff94d3,
        0x000000,0x131313,0x282828,0x363636,0x4d4d4d,0x656565,0x818181,0x9f9f9f,0xbcbcbc,0xe2e2e2,0xffffff
    };
    return index < 99 ? palette[index] : -1;
}

void text_reader_init(struct text_reader *reader, const char *text)
{
    reader->cursor = text;
    reader->style.foreground = reader->style.background = -1;
    reader->style.attributes = 0;
}

static int color_value(const char **cursor, int hex)
{
    const char *s = *cursor;
    unsigned int value = 0;
    size_t index, maximum = hex ? 6 : 2;
    for (index = 0; index < maximum; ++index) {
        unsigned int digit;
        if (s[index] >= '0' && s[index] <= '9') digit = (unsigned int)(s[index] - '0');
        else if (hex && s[index] >= 'a' && s[index] <= 'f') digit = (unsigned int)(s[index] - 'a' + 10);
        else if (hex && s[index] >= 'A' && s[index] <= 'F') digit = (unsigned int)(s[index] - 'A' + 10);
        else break;
        value = value * (hex ? 16u : 10u) + digit;
    }
    if (index == 0 || (hex && index != 6)) return -2;
    *cursor += index;
    return hex ? (int)value : text_palette(value);
}

int text_next(struct text_reader *reader, uint32_t *codepoint)
{
    while (*reader->cursor != '\0') {
        uint32_t c;
        reader->cursor += utf8_decode(reader->cursor, &c);
        switch (c) {
        case 2: reader->style.attributes ^= TEXT_BOLD; break;
        case 31: reader->style.attributes ^= TEXT_UNDERLINE; break;
        case 22: reader->style.attributes ^= TEXT_REVERSE; break;
        case 29: reader->style.attributes ^= TEXT_ITALIC; break;
        case 15:
            reader->style.attributes = 0;
            reader->style.foreground = reader->style.background = -1;
            break;
        case 3:
        case 4: {
            int color = color_value(&reader->cursor, c == 4);
            if (color == -2) {
                reader->style.foreground = reader->style.background = -1;
            } else {
                reader->style.foreground = color;
                if (*reader->cursor == ',') {
                    const char *background = reader->cursor + 1;
                    color = color_value(&background, c == 4);
                    if (color != -2) {
                        reader->style.background = color;
                        reader->cursor = background;
                    }
                }
            }
            break;
        }
        case 17: case 30: break; /* Already monospace; no portable strikeout. */
        default:
            *codepoint = unsafe(c) ? '?' : c;
            return 1;
        }
    }
    return 0;
}
