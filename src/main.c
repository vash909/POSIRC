#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "ui.h"

static volatile sig_atomic_t stop_requested;

/* Terminal cleanup belongs in normal execution, never in a signal handler. */
static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [-s server] [-p port] [-n nickname] [-j channel] [-c ca-file]\n"
            "       %s --help | --version\n"
            "  -s server    Connect on startup (otherwise use /connect)\n"
            "  -p port      TLS port, default 6697\n"
            "  -n nickname  Initial nickname, default posirc\n"
            "  -j channel   Join after registration\n"
            "  -c ca-file   PEM trust file instead of system CA paths\n"
            "TLS and certificate/hostname verification are always enabled.\n",
            program, program);
}

int main(int argc, char **argv)
{
    struct sigaction action;
    struct posirc_app app;
    const char *term, *server = NULL, *port = "6697", *nick = "posirc";
    const char *channel = "", *ca_file = NULL;
    int option, result;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("POSIRC " POSIRC_VERSION);
        return EXIT_SUCCESS;
    }
    opterr = 0;
    while ((option = getopt(argc, argv, "s:p:n:j:c:h")) != -1) {
        switch (option) {
        case 's': server = optarg; break;
        case 'p': port = optarg; break;
        case 'n': nick = optarg; break;
        case 'j': channel = optarg; break;
        case 'c': ca_file = optarg; break;
        case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
        default: usage(stderr, argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc || !irc_nick_valid(nick) ||
        (*channel != '\0' && (!irc_atom_valid(channel, POSIRC_TARGET_CAPACITY - 1) ||
                              strchr("#&+!", channel[0]) == NULL))) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("POSIRC requires an interactive terminal. Try --help.\n", stderr);
        return EXIT_FAILURE;
    }
    term = getenv("TERM");
    if (term == NULL || term[0] == '\0') {
        fputs("POSIRC requires TERM to identify the terminal.\n", stderr);
        return EXIT_FAILURE;
    }
    (void)setlocale(LC_ALL, "");
    memset(&action, 0, sizeof action);
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGHUP, &action, NULL) == -1) {
        perror("POSIRC: sigaction");
        return EXIT_FAILURE;
    }
    /* OpenSSL uses write/send internally; a closed socket must become an I/O
     * error, not terminate the process and leave curses active. */
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) == -1) {
        perror("POSIRC: SIGPIPE");
        return EXIT_FAILURE;
    }
    posirc_app_init(&app);
    (void)snprintf(app.nick, sizeof app.nick, "%s", nick);
    (void)snprintf(app.autojoin, sizeof app.autojoin, "%s", channel);
    if (posirc_app_setup(&app, ca_file) != 0) {
        fputs("POSIRC: cannot allocate transport or CA file path is too long.\n", stderr);
        posirc_app_destroy(&app);
        return EXIT_FAILURE;
    }
    if (server != NULL)
        (void)posirc_app_connect(&app, server, port);
    result = posirc_ui_run(&app, &stop_requested);
    posirc_app_destroy(&app);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
