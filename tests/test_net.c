#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "app.h"
#include "net.h"

/* Entire integration harness is C. Certificates and loopback listeners are
 * temporary; no external IRC server, certificate authority or shell is used. */
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; \
} } while (0)
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "Fixture failure %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(2); \
} } while (0)

static EVP_PKEY *new_key(void)
{
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY *key = NULL;
    REQUIRE(context != NULL);
    REQUIRE(EVP_PKEY_keygen_init(context) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) == 1);
    REQUIRE(EVP_PKEY_keygen(context, &key) == 1);
    EVP_PKEY_CTX_free(context);
    return key;
}

static void extension(X509 *cert, X509 *issuer, int nid, const char *value)
{
    X509V3_CTX context;
    X509_EXTENSION *entry;
    X509V3_set_ctx(&context, issuer, cert, NULL, NULL, 0);
    entry = X509V3_EXT_conf_nid(NULL, &context, nid, value);
    REQUIRE(entry != NULL);
    REQUIRE(X509_add_ext(cert, entry, -1) == 1);
    X509_EXTENSION_free(entry);
}

static X509 *certificate(EVP_PKEY *key, X509 *issuer, EVP_PKEY *signer,
                         const char *name, const char *san, int expired)
{
    static long serial = 1;
    X509 *cert = X509_new();
    X509_NAME *subject;
    REQUIRE(cert != NULL);
    REQUIRE(X509_set_version(cert, 2) == 1);
    REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(cert), serial++) == 1);
    REQUIRE(X509_gmtime_adj(X509_getm_notBefore(cert), -7200) != NULL);
    REQUIRE(X509_gmtime_adj(X509_getm_notAfter(cert), expired ? -3600 : 86400) != NULL);
    REQUIRE(X509_set_pubkey(cert, key) == 1);
    subject = X509_get_subject_name(cert);
    REQUIRE(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                      (const unsigned char *)name, -1, -1, 0) == 1);
    REQUIRE(X509_set_issuer_name(cert, issuer != NULL ? X509_get_subject_name(issuer) : subject) == 1);
    extension(cert, issuer != NULL ? issuer : cert, NID_basic_constraints,
              issuer == NULL ? "critical,CA:TRUE" : "critical,CA:FALSE");
    extension(cert, issuer != NULL ? issuer : cert, NID_key_usage,
              issuer == NULL ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature,keyEncipherment");
    if (san != NULL) {
        extension(cert, issuer, NID_subject_alt_name, san);
        extension(cert, issuer, NID_ext_key_usage, "serverAuth");
    }
    REQUIRE(X509_sign(cert, signer, EVP_sha256()) > 0);
    return cert;
}

static void save_ca(char *path, X509 *ca)
{
    int fd = mkstemp(path);
    FILE *stream;
    REQUIRE(fd >= 0);
    stream = fdopen(fd, "w");
    REQUIRE(stream != NULL);
    REQUIRE(PEM_write_X509(stream, ca) == 1);
    REQUIRE(fclose(stream) == 0);
}

static void pause_ms(long milliseconds)
{
    struct timespec duration;
    duration.tv_sec = milliseconds / 1000;
    duration.tv_nsec = (milliseconds % 1000) * 1000000;
    while (nanosleep(&duration, &duration) < 0 && errno == EINTR)
        ;
}

static void server_write(SSL *tls, const char *text)
{
    size_t offset = 0, length = strlen(text);
    while (offset < length) {
        int result = SSL_write(tls, text + offset, (int)(length - offset));
        REQUIRE(result > 0);
        offset += (size_t)result;
    }
}

static void expect_line(SSL *tls, const char *expected)
{
    char line[IRC_WIRE_CAPACITY + 1];
    size_t length = 0;
    while (length < IRC_WIRE_CAPACITY) {
        REQUIRE(SSL_read(tls, line + length, 1) == 1);
        if (line[length++] == '\n')
            break;
    }
    line[length] = '\0';
    if (strcmp(line, expected) != 0) {
        fprintf(stderr, "Server expected [%s], received [%s]\n", expected, line);
        exit(2);
    }
}

enum scenario { CHAT, REJECT, QUEUE, MALFORMED, ABRUPT };

static void serve(int listener, SSL_CTX *context, enum scenario scenario)
{
    int socket_fd = accept(listener, NULL, NULL);
    SSL *tls;
    int accepted;
    (void)alarm(10);
    REQUIRE(socket_fd >= 0);
    (void)close(listener);
    tls = SSL_new(context);
    REQUIRE(tls != NULL && SSL_set_fd(tls, socket_fd) == 1);
    accepted = SSL_accept(tls);
    if (scenario == REJECT) {
        /* TLS 1.3 servers can finish their side before reading the client's
         * verification alert. In either case, no IRC bytes may be received. */
        if (accepted == 1) {
            char byte;
            REQUIRE(SSL_read(tls, &byte, 1) <= 0);
        }
    } else {
        REQUIRE(accepted == 1);
        if (scenario == QUEUE) {
            size_t index;
            char expected[IRC_WIRE_CAPACITY + 1];
            memset(expected, 'x', 510);
            memcpy(expected + 510, "\r\n", 3);
            pause_ms(100);
            for (index = 0; index < 128; ++index)
                expect_line(tls, expected);
            server_write(tls, "DONE\r\n");
        } else {
            expect_line(tls, "NICK tester\r\n");
            expect_line(tls, "USER posirc 0 * :POSIRC client\r\n");
            if (scenario == MALFORMED) {
                server_write(tls, "NOTICE * :invalid bare LF\n");
            } else if (scenario == ABRUPT) {
                (void)close(socket_fd);
                SSL_free(tls);
                SSL_CTX_free(context);
                _exit(0);
            } else {
                server_write(tls, "PING :spl");
                pause_ms(20);
                server_write(tls, "it\r\n:srv 001 tester :Welcome\r\n");
                expect_line(tls, "PONG :split\r\n");
                expect_line(tls, "JOIN #room\r\n");
                server_write(tls, ":tester!u@h JOIN :#room\r\n"
                             ":srv 005 tester CASEMAPPING=ascii :supported\r\n"
                             ":srv 353 tester = #room :@tester +alice José\r\n"
                             ":srv 366 tester #room :end\r\n"
                             ":op!u@h MODE #room +o alice\r\n"
                             ":José!u@h PRIVMSG tester :こんにちは\r\n"
                             ":alice!u@h PRIVMSG #room :\00304caffè 😀\017 hello\033[31m\r\n");
                expect_line(tls, "PRIVMSG #room :caffè 日 😀\r\n");
                expect_line(tls, "PRIVMSG friend :\00304hello friend\017\r\n");
                expect_line(tls, "NICK other\r\n");
                server_write(tls, ":tester!u@h NICK :other\r\n");
                expect_line(tls, "PART #room\r\n");
                server_write(tls, ":other!u@h PART #room :leaving\r\n");
                expect_line(tls, "QUIT :Leaving POSIRC\r\n");
            }
        }
        (void)SSL_shutdown(tls);
    }
    SSL_free(tls);
    (void)close(socket_fd);
    SSL_CTX_free(context);
    _exit(0);
}

static pid_t start_server(X509 *cert, EVP_PKEY *key, enum scenario scenario,
                          char *port, size_t capacity, int version)
{
    struct sockaddr_in address;
    socklen_t length = sizeof address;
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());
    int listener;
    pid_t child;
    REQUIRE(context != NULL);
    REQUIRE(SSL_CTX_use_certificate(context, cert) == 1);
    REQUIRE(SSL_CTX_use_PrivateKey(context, key) == 1);
    REQUIRE(SSL_CTX_set_min_proto_version(context, version) == 1);
    REQUIRE(SSL_CTX_set_max_proto_version(context, version) == 1);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(listener, (struct sockaddr *)&address, sizeof address) == 0);
    REQUIRE(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    REQUIRE(listen(listener, 1) == 0);
    (void)snprintf(port, capacity, "%u", (unsigned int)ntohs(address.sin_port));
    child = fork();
    REQUIRE(child >= 0);
    if (child == 0)
        serve(listener, context, scenario);
    (void)close(listener);
    SSL_CTX_free(context);
    return child;
}

static void join_server(pid_t child)
{
    int status = 0;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    CHECK(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int history_has(const struct posirc_app *app, const char *text)
{
    size_t slot, index;
    for (slot = 0; slot < POSIRC_BUFFER_CAPACITY; ++slot)
        for (index = 0; index < app->buffers[slot].history_count; ++index)
            if (strstr(buffer_line(&app->buffers[slot], index), text) != NULL) return 1;
    return 0;
}

static void app_scenario(X509 *cert, EVP_PKEY *key, const char *ca_file,
                         enum scenario scenario, const char *expected, int version, const char *host)
{
    char port[16];
    pid_t child = start_server(cert, key, scenario, port, sizeof port, version);
    struct posirc_app app;
    time_t deadline = time(NULL) + 8;
    int stage = 0;
    posirc_app_init(&app);
    (void)snprintf(app.nick, sizeof app.nick, "tester");
    (void)snprintf(app.autojoin, sizeof app.autojoin, "#room");
    REQUIRE(posirc_app_setup(&app, ca_file) == 0);
    REQUIRE(posirc_app_connect(&app, host, port) == 0);
    while (net_active(app.net) && time(NULL) < deadline) {
        posirc_app_step(&app, -1, 10);
        if (scenario == CHAT) {
            if (stage == 0 && history_has(&app, "hello?[31m")) {
                CHECK(app.registered && strcmp(app.target, "#room") == 0);
                CHECK(app.case_mapping == 2);
                {
                    struct posirc_buffer *room = posirc_app_find(&app, "#room");
                    struct posirc_buffer *query = posirc_app_find(&app, "José");
                    CHECK(room != NULL && buffer_user_count(room) == 3);
                    CHECK(query != NULL && query->unread == 1 && posirc_app_current(&app) == room);
                    CHECK(buffer_user(room, "alice", 2)->modes == 3);
                    CHECK(history_has(&app, "caffè 😀"));
                }
                posirc_app_submit(&app, "caffè 日 😀");
                posirc_app_submit(&app, "/msg friend \00304hello friend\017");
                posirc_app_submit(&app, "/nick other");
                stage = 1;
            } else if (stage == 1 && strcmp(app.nick, "other") == 0) {
                posirc_app_submit(&app, "/part");
                stage = 2;
            } else if (stage == 2 && app.target[0] == '\0') {
                posirc_app_submit(&app, "/quit");
                stage = 3;
            }
        }
    }
    CHECK(!net_active(app.net));
    if (scenario == CHAT) {
        CHECK(stage == 3 && !app.running);
        CHECK(history_has(&app, "certificate verified"));
    } else {
        CHECK(!app.registered);
        CHECK(history_has(&app, expected));
        if (scenario == REJECT)
            CHECK(!history_has(&app, "certificate verified"));
    }
    posirc_app_destroy(&app);
    join_server(child);
}

struct queue_test {
    struct posirc_net *net;
    size_t sent;
    int ready, done;
};

static void queue_event(void *context, enum net_event event, const char *data, size_t length)
{
    struct queue_test *test = context;
    if (event == NET_READY)
        test->ready = 1;
    if (event == NET_DATA && length >= 4 && memcmp(data, "DONE", 4) == 0)
        test->done = 1;
}

static void queue_scenario(X509 *cert, EVP_PKEY *key, const char *ca_file)
{
    char port[16], line[IRC_WIRE_CAPACITY];
    struct queue_test test = {NULL, 0, 0, 0};
    pid_t child = start_server(cert, key, QUEUE, port, sizeof port, TLS1_2_VERSION);
    time_t deadline = time(NULL) + 8;
    test.net = net_create(ca_file, queue_event, &test);
    REQUIRE(test.net != NULL);
    CHECK(net_connect(test.net, "127.0.0.1", "0") == -1);
    CHECK(net_connect(test.net, "127.0.0.1", "65536") == -1);
    CHECK(net_connect(test.net, "bad\nname", port) == -1);
    /* Cancel while DNS is pending, then reuse the same transport. */
    REQUIRE(net_connect(test.net, "localhost", port) == 0);
    net_disconnect(test.net, "cancel test");
    CHECK(!net_active(test.net));
    REQUIRE(net_connect(test.net, "127.0.0.1", port) == 0);
    while (!test.ready && net_active(test.net) && time(NULL) < deadline)
        net_step(test.net, -1, 10);
    CHECK(test.ready);
    memset(line, 'x', 510);
    line[510] = '\0';
    CHECK(net_send(test.net, "PING :x\r\nJOIN #injected") == -1);
    CHECK(net_send(test.net, "") == -1);
    while (test.sent < 64 && net_send(test.net, line) == 0)
        ++test.sent;
    CHECK(test.sent == 64);
    CHECK(net_send(test.net, line) == -1);
    while (!test.done && net_active(test.net) && time(NULL) < deadline) {
        net_step(test.net, -1, 10);
        while (test.sent < 128 && net_send(test.net, line) == 0)
            ++test.sent;
    }
    CHECK(test.sent == 128 && test.done);
    net_destroy(test.net);
    join_server(child);
}

int main(void)
{
    EVP_PKEY *ca_key, *server_key, *other_key;
    X509 *ca, *valid, *wrong_name, *expired, *other_ca, *dns_only;
    char ca_path[] = "/tmp/posirc-test-ca-XXXXXX";
    char other_path[] = "/tmp/posirc-test-other-XXXXXX";
    (void)signal(SIGPIPE, SIG_IGN);
    ca_key = new_key(); server_key = new_key(); other_key = new_key();
    ca = certificate(ca_key, NULL, ca_key, "POSIRC test CA", NULL, 0);
    other_ca = certificate(other_key, NULL, other_key, "Other test CA", NULL, 0);
    valid = certificate(server_key, ca, ca_key, "localhost", "DNS:localhost,IP:127.0.0.1", 0);
    wrong_name = certificate(server_key, ca, ca_key, "wrong.invalid", "DNS:wrong.invalid", 0);
    expired = certificate(server_key, ca, ca_key, "localhost", "DNS:localhost", 1);
    dns_only = certificate(server_key, ca, ca_key, "localhost", "DNS:localhost", 0);
    save_ca(ca_path, ca); save_ca(other_path, other_ca);
    app_scenario(valid, server_key, ca_path, CHAT, "", TLS1_2_VERSION, "localhost");
    puts("OK: TLS 1.2, registration, PING, JOIN, chat, NICK, PART, QUIT and display sanitizing");
#ifdef TLS1_3_VERSION
    app_scenario(valid, server_key, ca_path, CHAT, "", TLS1_3_VERSION, "localhost");
    puts("OK: TLS 1.3 IRC session");
#endif
    app_scenario(valid, server_key, other_path, REJECT, "TLS handshake", TLS1_2_VERSION, "localhost");
    app_scenario(wrong_name, server_key, ca_path, REJECT, "hostname mismatch", TLS1_2_VERSION, "localhost");
    app_scenario(expired, server_key, ca_path, REJECT, "expired", TLS1_2_VERSION, "localhost");
    app_scenario(dns_only, server_key, ca_path, REJECT, "IP address mismatch", TLS1_2_VERSION, "127.0.0.1");
    puts("OK: untrusted CA, wrong hostname/IP and expired certificate rejected");
    app_scenario(valid, server_key, ca_path, MALFORMED, "Invalid/oversized", TLS1_2_VERSION, "localhost");
    app_scenario(valid, server_key, ca_path, ABRUPT, "TLS read", TLS1_2_VERSION, "localhost");
    queue_scenario(valid, server_key, ca_path);
    puts("OK: malformed frames, abrupt close, IP verification, bounded queue and wraparound");
    CHECK(unlink(ca_path) == 0); CHECK(unlink(other_path) == 0);
    X509_free(dns_only);
    X509_free(ca); X509_free(other_ca); X509_free(valid); X509_free(wrong_name); X509_free(expired);
    EVP_PKEY_free(ca_key); EVP_PKEY_free(server_key); EVP_PKEY_free(other_key);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
