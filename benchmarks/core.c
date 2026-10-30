#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "buffer.h"
#include "text.h"

/* Linux/GNU linker wrappers count calls made by the linked client objects;
 * they do not instrument allocations internal to libc/OpenSSL. */
static unsigned long allocations, frees;
void *__real_malloc(size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n) { ++allocations; return __real_malloc(n); }
void *__wrap_realloc(void *p, size_t n) { ++allocations; return __real_realloc(p, n); }
void __wrap_free(void *p) { ++frees; __real_free(p); }
static double cpu_seconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) exit(2);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
int main(void)
{
    char text[401], out[1024];
    struct posirc_buffer buffer;
    size_t i, kind;
    const size_t iterations = 1000000;
    volatile unsigned long checksum = 0;
    printf("layout,buffer_bytes,%zu\nlayout,user_bytes,%zu\n", sizeof buffer, sizeof(struct posirc_user));
    for (kind = 0; kind < 3; ++kind) {
        double start, elapsed;
        const char *unit = kind == 0 ? "abcdefgh" : kind == 1 ? "é日abc" : "\00304ab\017cd";
        for (i = 0; i < 400; i += 8) memcpy(text + i, unit, 8);
        text[400] = '\0';
        start = cpu_seconds();
        for (i = 0; i < iterations; ++i) { text_clean(out, sizeof out, text, 1); checksum += (unsigned char)out[i % 400]; }
        elapsed = cpu_seconds() - start;
        printf("clean_%zu,ns_per_op,%.3f\n", kind, elapsed * 1e9 / iterations);
    }
    memset(text, 'x', 400); text[400] = '\0';
    buffer_init(&buffer, BUFFER_QUERY, "bench");
    for (i = 0; i < POSIRC_HISTORY_CAPACITY; ++i) if (buffer_append(&buffer, text) != 0) return 2;
    allocations = frees = 0;
    {
        double start = cpu_seconds();
        for (i = 0; i < iterations; ++i) if (buffer_append(&buffer, text) != 0) return 2;
        printf("history,ns_per_op,%.3f\nhistory,allocation_calls,%lu\nhistory,free_calls,%lu\n",
               (cpu_seconds() - start) * 1e9 / iterations, allocations, frees);
    }
    checksum += (unsigned char)buffer_line(&buffer, 0)[0];
    buffer_free(&buffer);
    fprintf(stderr, "checksum=%lu\n", checksum);
    return 0;
}
