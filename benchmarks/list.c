#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "app.h"

/* Core-only LIST replay: no network, TLS, redraws or artificial sleeps. */
int main(void)
{
    const size_t counts[] = {256, 10000, 100000};
    size_t run, kind;
    puts("run,replies,cpu_ms,history_entries,history_bytes,conversation_buffers,user_entries");
    for (run = 1; run <= 5; ++run) for (kind = 0; kind < sizeof counts / sizeof counts[0]; ++kind) {
        struct posirc_app app;
        struct timespec start, end;
        size_t i, bytes = 0, buffers = 0, users = 0;
        char line[IRC_WIRE_CAPACITY];
        posirc_app_init(&app);
        buffer_clear(&app.buffers[0]);
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start) != 0) return 2;
        for (i = 0; i < counts[kind]; ++i) {
            (void)snprintf(line, sizeof line, ":srv 322 posirc #channel%06zu 42 :Benchmark topic 日本語", i);
            if (posirc_app_receive(&app, line) != 0) return 2;
        }
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end) != 0) return 2;
        for (i = 0; i < POSIRC_HISTORY_CAPACITY; ++i) {
            struct posirc_history *entry = app.buffers[0].history[i];
            if (entry != NULL) bytes += sizeof *entry + entry->capacity;
        }
        for (i = 1; i < POSIRC_BUFFER_CAPACITY; ++i) {
            buffers += app.buffers[i].type != BUFFER_UNUSED;
            users += app.buffers[i].user_count;
        }
        printf("%zu,%zu,%.6f,%zu,%zu,%zu,%zu\n", run, counts[kind],
               (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6,
               app.buffers[0].history_count, bytes, buffers, users);
        posirc_app_destroy(&app);
    }
    return 0;
}
