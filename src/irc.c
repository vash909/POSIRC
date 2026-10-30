#include <string.h>

#include "irc.h"
#include "text.h"

int irc_feed(struct irc_frame *frame, const char *data, size_t length,
             irc_line_callback callback, void *context)
{
    size_t index;

    for (index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)data[index];
        if (byte == 0)
            return -1;
        if (byte == '\n') {
            if (frame->length == 0 || frame->data[frame->length - 1] != '\r')
                return -1;
            frame->data[frame->length - 1] = '\0';
            frame->length = 0;
            if (frame->data[0] != '\0' && callback(context, frame->data) != 0)
                return -1;
        } else {
            if (frame->length >= sizeof frame->data - 1)
                return -1;
            if (frame->length > 0 && frame->data[frame->length - 1] == '\r')
                return -1;
            frame->data[frame->length++] = (char)byte;
        }
    }
    return 0;
}

int irc_parse(char *line, struct irc_message *message)
{
    char *cursor = line;
    size_t length, index;

    memset(message, 0, sizeof *message);
    /* Tolerate a bounded tag block without negotiating or interpreting tags. */
    if (*cursor == '@') {
        cursor = strchr(cursor, ' ');
        if (cursor == NULL)
            return -1;
        ++cursor;
    }
    while (*cursor == ' ')
        ++cursor;
    if (*cursor == ':') {
        message->prefix = ++cursor;
        cursor = strchr(cursor, ' ');
        if (cursor == NULL || cursor == message->prefix)
            return -1;
        *cursor++ = '\0';
    }
    while (*cursor == ' ')
        ++cursor;
    message->command = cursor;
    while (*cursor != '\0' && *cursor != ' ')
        ++cursor;
    if (*cursor != '\0')
        *cursor++ = '\0';
    length = strlen(message->command);
    if (length == 0)
        return -1;
    for (index = 0; index < length; ++index) {
        unsigned char c = (unsigned char)message->command[index];
        if (c >= 'a' && c <= 'z')
            message->command[index] = (char)(c - 'a' + 'A');
        else if (!(c >= 'A' && c <= 'Z') &&
                 !(length == 3 && c >= '0' && c <= '9'))
            return -1;
    }
    /* A command is either entirely alphabetic or exactly three digits. */
    for (index = 0; index < length; ++index) {
        int numeric = message->command[0] >= '0' && message->command[0] <= '9';
        int digit = message->command[index] >= '0' && message->command[index] <= '9';
        if (numeric != digit)
            return -1;
    }
    while (*cursor != '\0') {
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            break;
        if (message->count == IRC_PARAM_CAPACITY)
            return -1;
        if (*cursor == ':') {
            message->params[message->count++] = cursor + 1;
            break;
        }
        message->params[message->count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;
        if (*cursor != '\0')
            *cursor++ = '\0';
    }
    return 0;
}

int irc_atom_valid(const char *text, size_t limit)
{
    size_t length = 0;
    if (text == NULL || *text == '\0' || !utf8_valid(text))
        return 0;
    while (*text != '\0') {
        unsigned char c = (unsigned char)*text++;
        if (c <= 32 || c == 127 || c == ':' || c == ',' || ++length > limit)
            return 0;
    }
    return 1;
}

int irc_nick_valid(const char *text)
{
    size_t index;
    if (!irc_atom_valid(text, 63))
        return 0;
    for (index = 0; text[index] != '\0'; ++index) {
        unsigned char c = (unsigned char)text[index];
        if (c >= 128 || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            strchr("[]\\`_^{|}", (int)c) != NULL)
            continue;
        if (index > 0 && ((c >= '0' && c <= '9') || c == '-'))
            continue;
        return 0;
    }
    return 1;
}

static unsigned char fold(unsigned char c, int mapping)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c - 'A' + 'a');
    if (mapping != 2) {
        if (c == '[') return '{';
        if (c == ']') return '}';
        if (c == '\\') return '|';
        if (mapping == 0 && c == '^') return '~';
    }
    return c;
}

int irc_equal(const char *left, const char *right, int mapping)
{
    while (*left != '\0' && *right != '\0') {
        if (fold((unsigned char)*left++, mapping) != fold((unsigned char)*right++, mapping))
            return 0;
    }
    return *left == *right;
}
