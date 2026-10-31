#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app.h"
#include "config.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL config %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main(void)
{
    struct posirc_app app, loaded;
    struct stat info;
    char path[] = "/tmp/posirc-config-XXXXXX", line[1024];
    int fd = mkstemp(path);
    FILE *file;
    size_t i;
    CHECK(fd >= 0);
    if (fd < 0) return EXIT_FAILURE;
    (void)close(fd); CHECK(unlink(path) == 0);
    posirc_app_init(&app);
    CHECK(posirc_config_load(&app, path) == 0 && strcmp(app.nick, "posirc") == 0);
    file = fopen(path, "w"); CHECK(file != NULL);
    if (file == NULL) return EXIT_FAILURE;
    fputs("server=irc.example\r\nport=7000\r\nnick=abcdefghijklmnoEXTRA\r\n", file);
    CHECK(fclose(file) == 0);
    CHECK(posirc_config_load(&app, path) == 0);
    CHECK(strcmp(app.server, "irc.example") == 0 && strcmp(app.server_port, "7000") == 0);
    CHECK(strcmp(app.nick, "abcdefghijklmno") == 0);
    CHECK(posirc_config_save(&app, path) == 0);
    CHECK(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);
    posirc_app_init(&loaded);
    CHECK(posirc_config_load(&loaded, path) == 0);
    CHECK(strcmp(loaded.server, app.server) == 0 && strcmp(loaded.nick, app.nick) == 0);
    CHECK(strcmp(loaded.server_port, app.server_port) == 0);
    /* Saved values are copied without nickname/host/port semantic checks. */
    file = fopen(path, "w"); CHECK(file != NULL);
    if (file == NULL) return EXIT_FAILURE;
    fputs("unknown=ignored\nserver=not a hostname\nport=abcd\nnick=1 bad nick", file);
    CHECK(fclose(file) == 0 && posirc_config_load(&loaded, path) == 0);
    CHECK(strcmp(loaded.server, "not a hostname") == 0 && strcmp(loaded.server_port, "abcd") == 0);
    CHECK(strcmp(loaded.nick, "1 bad nick") == 0);
    file = fopen(path, "w"); CHECK(file != NULL);
    if (file == NULL) return EXIT_FAILURE;
    fputs("server=", file);
    memset(line, 'x', sizeof line); CHECK(fwrite(line, 1, sizeof line, file) == sizeof line);
    fputs("nick=must_not_be_a_second_record\nport=6697\nnick=", file);
    for (i = 0; i < 16; ++i) fputs("日", file);
    CHECK(fclose(file) == 0 && posirc_config_load(&loaded, path) == 0);
    CHECK(strlen(loaded.server) == sizeof loaded.server - 1);
    CHECK(strcmp(loaded.server_port, "6697") == 0 && strlen(loaded.nick) == 45);
    CHECK(irc_local_nick_valid(loaded.nick));
    CHECK(posirc_config_save(&loaded, path) == 0 && posirc_config_load(&app, path) == 0);
    CHECK(strcmp(app.nick, loaded.nick) == 0);
    CHECK(unlink(path) == 0);
    posirc_app_destroy(&app); posirc_app_destroy(&loaded);
    if (failures) return EXIT_FAILURE;
    puts("OK: portable config loading, atomic save, length bounds and Unicode nickname limit");
    return EXIT_SUCCESS;
}
