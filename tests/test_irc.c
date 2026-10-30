#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "irc.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; \
} } while (0)

struct capture {
    char lines[8][IRC_WIRE_CAPACITY];
    size_t count;
};

static int capture_line(void *context, char *line)
{
    struct capture *capture = context;
    if (capture->count == 8)
        return -1;
    (void)snprintf(capture->lines[capture->count++], IRC_WIRE_CAPACITY, "%s", line);
    return 0;
}

static void framing(void)
{
    const char data[] = "PING :one\r\n:srv NOTICE * :two words\r\n";
    size_t split;
    for (split = 0; split <= sizeof data - 1; ++split) {
        struct irc_frame frame = {{0}, 0};
        struct capture capture = {{{0}}, 0};
        CHECK(irc_feed(&frame, data, split, capture_line, &capture) == 0);
        CHECK(irc_feed(&frame, data + split, sizeof data - 1 - split, capture_line, &capture) == 0);
        CHECK(capture.count == 2);
        CHECK(strcmp(capture.lines[0], "PING :one") == 0);
        CHECK(strcmp(capture.lines[1], ":srv NOTICE * :two words") == 0);
        CHECK(frame.length == 0);
    }
    {
        struct irc_frame frame = {{0}, 0};
        struct capture capture = {{{0}}, 0};
        char limit[IRC_WIRE_CAPACITY + 1];
        memset(limit, 'x', sizeof limit);
        limit[510] = '\r'; limit[511] = '\n';
        CHECK(irc_feed(&frame, limit, 512, capture_line, &capture) == 0);
        CHECK(strlen(capture.lines[0]) == 510);
        memset(&frame, 0, sizeof frame);
        limit[510] = 'x'; limit[511] = '\r'; limit[512] = '\n';
        CHECK(irc_feed(&frame, limit, 513, capture_line, &capture) == -1);
    }
    {
        const char *bad[] = {"PING\n", "PING\rX", "PING\0 :x\r\n"};
        const size_t lengths[] = {5, 6, 11};
        size_t index;
        for (index = 0; index < 3; ++index) {
            struct irc_frame frame = {{0}, 0};
            struct capture capture = {{{0}}, 0};
            CHECK(irc_feed(&frame, bad[index], lengths[index], capture_line, &capture) == -1);
        }
    }
}

static void parsing(void)
{
    struct irc_message message;
    char line[] = ":nick!user@host PRIVMSG #channel :hello world";
    char ping[] = "ping :token";
    char empty[] = "NOTICE nick :";
    char tags[] = "@time=ignored :srv 001 nick :welcome";
    char bad_prefix[] = ":invalid";
    char bad_command[] = "A12 nick :invalid mixed command";
    char too_many[] = "CMD a b c d e f g h i j k l m n o p";
    CHECK(irc_parse(line, &message) == 0);
    CHECK(strcmp(message.prefix, "nick!user@host") == 0);
    CHECK(strcmp(message.command, "PRIVMSG") == 0);
    CHECK(message.count == 2 && strcmp(message.params[1], "hello world") == 0);
    CHECK(irc_parse(ping, &message) == 0 && strcmp(message.command, "PING") == 0);
    CHECK(irc_parse(empty, &message) == 0 && message.count == 2 && message.params[1][0] == '\0');
    CHECK(irc_parse(tags, &message) == 0 && strcmp(message.command, "001") == 0);
    CHECK(irc_parse(bad_prefix, &message) == -1);
    CHECK(irc_parse(bad_command, &message) == -1);
    CHECK(irc_parse(too_many, &message) == -1);
    CHECK(irc_nick_valid("nick_123"));
    CHECK(!irc_nick_valid(NULL));
    CHECK(!irc_nick_valid("1nick"));
    CHECK(!irc_nick_valid("nick\r\nJOIN #other"));
    CHECK(!irc_atom_valid("#one,#two", 128));
    CHECK(!irc_atom_valid(":nick", 128));
    CHECK(!irc_atom_valid("toolong", 3));
    CHECK(irc_equal("NiCK[\\^", "nick{|~", 0));
    CHECK(!irc_equal("nick^", "nick~", 1));
    CHECK(!irc_equal("nick[", "nick{", 2));
}

int main(void)
{
    framing();
    parsing();
    if (failures != 0)
        return EXIT_FAILURE;
    puts("OK: IRC framing, parsing, limits and case mapping");
    return EXIT_SUCCESS;
}
