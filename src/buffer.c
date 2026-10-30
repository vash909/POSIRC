#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "buffer.h"
#include "irc.h"
#include "text.h"

void buffer_init(struct posirc_buffer *buffer, enum buffer_type type, const char *name)
{
    memset(buffer, 0, sizeof *buffer);
    buffer->type = type;
    (void)snprintf(buffer->name, sizeof buffer->name, "%s", name);
}

void buffer_clear(struct posirc_buffer *buffer)
{
    size_t index;
    for (index = 0; index < POSIRC_HISTORY_CAPACITY; ++index) {
        free(buffer->history[index]);
        buffer->history[index] = NULL;
    }
    buffer->history_count = buffer->history_start = buffer->scroll = buffer->unread = 0;
}

void buffer_users_clear(struct posirc_buffer *buffer)
{
    free(buffer->users);
    buffer->users = NULL;
    buffer->user_count = buffer->user_capacity = buffer->user_scroll = 0;
    buffer->names_sync = buffer->users_truncated = 0;
}

void buffer_free(struct posirc_buffer *buffer)
{
    buffer_clear(buffer);
    buffer_users_clear(buffer);
    memset(buffer, 0, sizeof *buffer);
}

int buffer_append_pending(struct posirc_buffer *buffer, const char *text, unsigned long tag)
{
    char clean[POSIRC_TEXT_CAPACITY];
    struct posirc_history *entry;
    size_t slot, length;
    text_clean(clean, sizeof clean, text, 1);
    length = strlen(clean) + 1;
    slot = (buffer->history_start + buffer->history_count) % POSIRC_HISTORY_CAPACITY;
    entry = buffer->history[slot];
    /* Reuse a ring slot's allocation after wraparound. Grow before changing
     * indices so an allocation failure leaves the existing history intact. */
    if (entry == NULL || entry->capacity < length) {
        struct posirc_history *grown = realloc(entry, sizeof *entry + length);
        if (grown == NULL) return -1;
        entry = grown;
        entry->capacity = length;
        buffer->history[slot] = entry;
    }
    memcpy(entry->text, clean, length);
    entry->pending = tag;
    if (buffer->history_count == POSIRC_HISTORY_CAPACITY)
        buffer->history_start = (slot + 1) % POSIRC_HISTORY_CAPACITY;
    else
        ++buffer->history_count;
    if (buffer->scroll > 0 && buffer->scroll < buffer->history_count - 1) ++buffer->scroll;
    return 0;
}

int buffer_append(struct posirc_buffer *buffer, const char *text)
{
    return buffer_append_pending(buffer, text, 0);
}

int buffer_complete(struct posirc_buffer *buffer, unsigned long tag, int sent)
{
    size_t index;
    int changed = 0;
    for (index = 0; index < buffer->history_count; ++index) {
        struct posirc_history *entry = buffer->history[(buffer->history_start + index) % POSIRC_HISTORY_CAPACITY];
        if (entry->pending != 0 && (tag == 0 || entry->pending == tag)) {
            size_t length = strlen(entry->text);
            if (length >= 9 && strcmp(entry->text + length - 9, " [queued]") == 0) {
                if (sent) entry->text[length - 9] = '\0';
                else memcpy(entry->text + length - 9, " [unsent]", 10);
                changed = 1;
            }
            entry->pending = 0;
        }
    }
    return changed;
}

const char *buffer_line(const struct posirc_buffer *buffer, size_t index)
{
    if (index >= buffer->history_count) return NULL;
    return buffer->history[(buffer->history_start + index) % POSIRC_HISTORY_CAPACITY]->text;
}

struct posirc_user *buffer_user(struct posirc_buffer *buffer, const char *nick, int mapping)
{
    size_t index;
    for (index = 0; index < buffer->user_count; ++index)
        if (irc_equal(buffer->users[index].nick, nick, mapping)) return &buffer->users[index];
    return NULL;
}

int buffer_user_set(struct posirc_buffer *buffer, const char *nick, unsigned int modes,
                    int present, int live, int mapping)
{
    struct posirc_user *user = buffer_user(buffer, nick, mapping);
    if (!irc_nick_valid(nick)) return -1;
    if (user == NULL) {
        if (!present && !buffer->names_sync) return 0;
        if (buffer->user_count == POSIRC_USER_CAPACITY) {
            buffer->users_truncated = 1;
            return -1;
        }
        if (buffer->user_count == buffer->user_capacity) {
            size_t capacity = buffer->user_capacity ? buffer->user_capacity * 2 : 32;
            struct posirc_user *users = realloc(buffer->users, capacity * sizeof *users);
            if (users == NULL) { buffer->users_truncated = 1; return -1; }
            buffer->users = users;
            buffer->user_capacity = capacity;
        }
        user = &buffer->users[buffer->user_count++];
        memset(user, 0, sizeof *user);
        (void)snprintf(user->nick, sizeof user->nick, "%s", nick);
    }
    if (!live && user->live) return 0;
    user->modes = modes;
    user->present = present;
    user->live = live && buffer->names_sync;
    if (live) user->renamed_to[0] = '\0';
    buffer->users_dirty = 1;
    if (!present && !buffer->names_sync) {
        size_t index = (size_t)(user - buffer->users);
        memmove(user, user + 1, (buffer->user_count - index - 1) * sizeof *user);
        --buffer->user_count;
    }
    return 0;
}

void buffer_user_rename(struct posirc_buffer *buffer, const char *old_name, const char *new_name, int mapping)
{
    struct posirc_user *user = buffer_user(buffer, old_name, mapping);
    unsigned int modes = user != NULL ? user->modes : 0;
    int present = user != NULL && user->present;
    if (!present && !buffer->names_sync) return;
    if (irc_equal(old_name, new_name, mapping)) {
        if (user != NULL) {
            (void)snprintf(user->nick, sizeof user->nick, "%s", new_name);
            user->live = buffer->names_sync;
            buffer->users_dirty = 1;
        }
        return;
    }
    (void)buffer_user_set(buffer, old_name, 0, 0, 1, mapping);
    user = buffer_user(buffer, old_name, mapping);
    if (buffer->names_sync && user != NULL)
        (void)snprintf(user->renamed_to, sizeof user->renamed_to, "%s", new_name);
    if (present) (void)buffer_user_set(buffer, new_name, modes, 1, 1, mapping);
}

void buffer_names_end(struct posirc_buffer *buffer)
{
    size_t read, write = 0;
    for (read = 0; read < buffer->user_count; ++read) {
        if (!buffer->users[read].present) continue;
        buffer->users[read].live = 0;
        buffer->users[read].renamed_to[0] = '\0';
        buffer->users[write++] = buffer->users[read];
    }
    buffer->user_count = write;
    buffer->names_sync = 0;
    buffer->users_dirty = 1;
}

static unsigned int rank(unsigned int modes)
{
    unsigned int bit;
    for (bit = 0; bit < 16; ++bit) if (modes & (1u << bit)) return bit;
    return UINT_MAX;
}

static int user_compare(const void *left, const void *right)
{
    const struct posirc_user *a = left, *b = right;
    unsigned int ar = rank(a->modes), br = rank(b->modes);
    if (a->present != b->present) return b->present - a->present;
    if (ar != br) return ar < br ? -1 : 1;
    return strcmp(a->nick, b->nick);
}

void buffer_users_sort(struct posirc_buffer *buffer)
{
    if (buffer->users_dirty && buffer->user_count > 1)
        qsort(buffer->users, buffer->user_count, sizeof *buffer->users, user_compare);
    buffer->users_dirty = 0;
}

size_t buffer_user_count(const struct posirc_buffer *buffer)
{
    size_t index, count = 0;
    for (index = 0; index < buffer->user_count; ++index) if (buffer->users[index].present) ++count;
    return count;
}
