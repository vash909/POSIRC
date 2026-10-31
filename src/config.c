#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app.h"
#include "config.h"
#include "text.h"

char *posirc_config_path(void)
{
    const char *directory = getenv("HOME");
    char *path;
    size_t capacity;
    if (directory == NULL || *directory == '\0') return NULL;
    capacity = strlen(directory) + sizeof "/.posirc";
    path = malloc(capacity);
    if (path != NULL) (void)snprintf(path, capacity, "%s/.posirc", directory);
    return path;
}

int posirc_config_load(struct posirc_app *app, const char *path)
{
    char line[384], io[512];
    FILE *file = fopen(path, "r");
    int failed;
    if (file == NULL) return errno == ENOENT ? 0 : -1;
    (void)setvbuf(file, io, _IOFBF, sizeof io);
    while (fgets(line, sizeof line, file) != NULL) {
        char *value;
        size_t length = strcspn(line, "\r\n");
        if (line[length] == '\0') {
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF) { }
        }
        line[length] = '\0';
        value = strchr(line, '=');
        if (value == NULL) continue;
        *value++ = '\0';
        if (strcmp(line, "server") == 0) (void)snprintf(app->server, sizeof app->server, "%s", value);
        else if (strcmp(line, "port") == 0) (void)snprintf(app->server_port, sizeof app->server_port, "%s", value);
        else if (strcmp(line, "nick") == 0) {
            size_t bytes = 0, characters = 0;
            uint32_t ignored;
            while (value[bytes] != '\0' && characters < IRC_LOCAL_NICK_CHARACTERS) {
                bytes += utf8_decode(value + bytes, &ignored);
                ++characters;
            }
            value[bytes] = '\0';
            (void)snprintf(app->nick, sizeof app->nick, "%s", value);
        }
    }
    failed = ferror(file);
    if (fclose(file) != 0) failed = 1;
    return failed ? -1 : 0;
}

int posirc_config_save(const struct posirc_app *app, const char *path)
{
    char io[512];
    size_t capacity = strlen(path) + sizeof ".XXXXXX";
    char *temporary = malloc(capacity);
    FILE *file;
    int fd, failed;
    if (temporary == NULL) return -1;
    (void)snprintf(temporary, capacity, "%s.XXXXXX", path);
    fd = mkstemp(temporary);
    if (fd < 0) { free(temporary); return -1; }
    file = fdopen(fd, "w");
    if (file == NULL) { (void)close(fd); (void)unlink(temporary); free(temporary); return -1; }
    (void)setvbuf(file, io, _IOFBF, sizeof io);
    failed = fprintf(file, "server=%s\nport=%s\nnick=%s\n", app->server, app->server_port, app->nick) < 0;
    if (fclose(file) != 0) failed = 1;
    if (!failed && rename(temporary, path) != 0) failed = 1;
    if (failed) (void)unlink(temporary);
    free(temporary);
    return failed ? -1 : 0;
}
