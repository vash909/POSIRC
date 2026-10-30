#ifndef POSIRC_BUFFER_H
#define POSIRC_BUFFER_H

#include <stddef.h>

#define POSIRC_BUFFER_CAPACITY 64
#define POSIRC_HISTORY_CAPACITY 256
#define POSIRC_TEXT_CAPACITY 1024
#define POSIRC_TARGET_CAPACITY 128
#define POSIRC_USER_CAPACITY 4096

enum buffer_type { BUFFER_UNUSED, BUFFER_SERVER, BUFFER_CHANNEL, BUFFER_QUERY };
struct posirc_user {
    char nick[64], renamed_to[64];
    unsigned int modes;
    int present, live;
};
struct posirc_history {
    size_t capacity;
    unsigned long pending;
    char text[];
};
struct posirc_buffer {
    enum buffer_type type;
    char name[POSIRC_TARGET_CAPACITY];
    int joined, names_sync, users_dirty, users_truncated;
    char topic[POSIRC_TEXT_CAPACITY];
    struct posirc_history *history[POSIRC_HISTORY_CAPACITY];
    size_t history_start, history_count, unread, scroll;
    char draft[POSIRC_TEXT_CAPACITY];
    size_t cursor, user_scroll;
    struct posirc_user *users;
    size_t user_count, user_capacity;
};
void buffer_init(struct posirc_buffer *buffer, enum buffer_type type, const char *name);
void buffer_free(struct posirc_buffer *buffer);
void buffer_clear(struct posirc_buffer *buffer);
int buffer_append(struct posirc_buffer *buffer, const char *text);
/* Tag only locally queued messages; completion must match the retained entry. */
int buffer_append_pending(struct posirc_buffer *buffer, const char *text, unsigned long tag);
int buffer_complete(struct posirc_buffer *buffer, unsigned long tag, int sent);
const char *buffer_line(const struct posirc_buffer *buffer, size_t index);
struct posirc_user *buffer_user(struct posirc_buffer *buffer, const char *nick, int mapping);
/* live events take precedence over stale entries in an in-flight NAMES list. */
int buffer_user_set(struct posirc_buffer *buffer, const char *nick, unsigned int modes,
                    int present, int live, int mapping);
void buffer_user_rename(struct posirc_buffer *buffer, const char *old_name, const char *new_name, int mapping);
void buffer_names_end(struct posirc_buffer *buffer);
void buffer_users_clear(struct posirc_buffer *buffer);
void buffer_users_sort(struct posirc_buffer *buffer);
size_t buffer_user_count(const struct posirc_buffer *buffer);

#endif
