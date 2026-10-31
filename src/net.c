#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "irc.h"
#include "net.h"

#define ADDRESS_CAPACITY 16
#define QUEUE_CAPACITY 64
#define CONNECT_SECONDS 10
#define SETUP_SECONDS 30

enum phase { OFFLINE, RESOLVING, CONNECTING, HANDSHAKE, ONLINE, DRAINING };
struct address {
    struct sockaddr_storage value;
    socklen_t length;
};
/* Private pipe protocol between a process and its forked resolver. */
struct resolution {
    int error;
    size_t count;
    struct address addresses[ADDRESS_CAPACITY];
};
struct outgoing {
    net_sent_callback sent;
    unsigned long tag;
    char data[IRC_WIRE_CAPACITY];
    size_t length;
};
struct posirc_net {
    SSL_CTX *tls_context;
    SSL *tls;
    int fd;
    int resolver_fd;
    pid_t resolver_pid;
    struct resolution resolution;
    size_t resolved_bytes;
    size_t next_address;
    enum phase phase;
    short handshake_want, read_want, write_want;
    double deadline, connect_deadline;
    char host[256];
    char ca_file[1024];
    struct outgoing queue[QUEUE_CAPACITY];
    size_t queue_start, queue_count, write_offset;
    char read_buffer[4096];
    net_callback callback;
    void *context;
};

static double now_seconds(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) == 0)
        return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
#endif
    /* Fallback for POSIX implementations without a working monotonic clock. */
    return (double)time(NULL);
}

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static void emit(struct posirc_net *net, enum net_event event, const char *text)
{
    net->callback(net->context, event, text, strlen(text));
}

const char *net_status(const struct posirc_net *net)
{
    if (net == NULL)
        return "offline";
    switch (net->phase) {
    case RESOLVING: return "resolving";
    case CONNECTING: return "connecting";
    case HANDSHAKE: return "verifying TLS";
    case ONLINE: return "TLS verified";
    case DRAINING: return "disconnecting";
    default: return "offline";
    }
}

static void set_phase(struct posirc_net *net, enum phase phase)
{
    net->phase = phase;
    emit(net, NET_STATUS, net_status(net));
}

static void stop_resolver(struct posirc_net *net)
{
    if (net->resolver_fd >= 0) {
        (void)close(net->resolver_fd);
        net->resolver_fd = -1;
    }
    if (net->resolver_pid > 0) {
        /* The child only resolves DNS. Kill also cancels a stuck libc lookup;
         * reap it so repeated connections never accumulate zombies. */
        (void)kill(net->resolver_pid, SIGKILL);
        while (waitpid(net->resolver_pid, NULL, 0) < 0 && errno == EINTR)
            ;
        net->resolver_pid = -1;
    }
}

static void release_connection(struct posirc_net *net)
{
    stop_resolver(net);
    SSL_free(net->tls);
    net->tls = NULL;
    if (net->fd >= 0)
        (void)close(net->fd);
    net->fd = -1;
    SSL_CTX_free(net->tls_context);
    net->tls_context = NULL;
    net->queue_start = net->queue_count = net->write_offset = 0;
    net->read_want = POLLIN;
    net->write_want = 0;
    net->phase = OFFLINE;
}

void net_disconnect(struct posirc_net *net, const char *reason)
{
    release_connection(net);
    emit(net, NET_CLOSED, reason);
}

static void fail_errno(struct posirc_net *net, const char *operation)
{
    char text[256];
    (void)snprintf(text, sizeof text, "%s: %s", operation, strerror(errno));
    net_disconnect(net, text);
}

static void fail_tls(struct posirc_net *net, const char *operation, int error)
{
    char text[256];
    long verify = net->tls != NULL ? SSL_get_verify_result(net->tls) : X509_V_OK;
    unsigned long code = ERR_peek_last_error();
    const char *detail = code != 0 ? ERR_reason_error_string(code) : NULL;
    if (verify != X509_V_OK)
        detail = X509_verify_cert_error_string(verify);
    if (detail == NULL)
        detail = error == SSL_ERROR_ZERO_RETURN ? "peer closed TLS" : "TLS I/O failed";
    (void)snprintf(text, sizeof text, "%s: %s", operation, detail);
    net_disconnect(net, text);
}

struct posirc_net *net_create(const char *ca_file, net_callback callback, void *context)
{
    struct posirc_net *net;
    if (callback == NULL || (ca_file != NULL && strlen(ca_file) >= 1024))
        return NULL;
    net = calloc(1, sizeof *net);
    if (net == NULL)
        return NULL;
    net->fd = net->resolver_fd = -1;
    net->resolver_pid = -1;
    net->callback = callback;
    net->context = context;
    net->read_want = POLLIN;
    if (ca_file != NULL)
        (void)snprintf(net->ca_file, sizeof net->ca_file, "%s", ca_file);
    return net;
}

void net_destroy(struct posirc_net *net)
{
    if (net != NULL) {
        release_connection(net);
        free(net);
    }
}

/* getaddrinfo has no portable asynchronous API. A short-lived child keeps
 * DNS delays outside the terminal loop without introducing a thread library. */
static void resolve_child(int fd, const char *host, const char *port)
{
    struct addrinfo hints, *result = NULL, *entry;
    struct resolution answer;
    size_t offset = 0;
    memset(&answer, 0, sizeof answer);
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICSERV;
    answer.error = getaddrinfo(host, port, &hints, &result);
    if (answer.error == 0) {
        for (entry = result; entry != NULL && answer.count < ADDRESS_CAPACITY;
             entry = entry->ai_next) {
            if ((entry->ai_family != AF_INET && entry->ai_family != AF_INET6) ||
                entry->ai_addrlen > sizeof(struct sockaddr_storage))
                continue;
            memcpy(&answer.addresses[answer.count].value, entry->ai_addr, entry->ai_addrlen);
            answer.addresses[answer.count++].length = entry->ai_addrlen;
        }
        freeaddrinfo(result);
    }
    while (offset < sizeof answer) {
        ssize_t count = write(fd, (const char *)&answer + offset, sizeof answer - offset);
        if (count > 0)
            offset += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    (void)close(fd);
    _exit(0);
}

int net_connect(struct posirc_net *net, const char *host, const char *port)
{
    int pipes[2];
    unsigned long number = 0;
    const char *cursor;
    if (net == NULL || net_active(net) || host == NULL || *host == '\0' || strlen(host) >= sizeof net->host ||
        port == NULL || *port == '\0')
        return -1;
    for (cursor = host; *cursor != '\0'; ++cursor)
        if ((unsigned char)*cursor <= 32 || (unsigned char)*cursor >= 127)
            return -1;
    for (cursor = port; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9' || number > 6553)
            return -1;
        number = number * 10 + (unsigned long)(*cursor - '0');
    }
    if (number == 0 || number > 65535)
        return -1;
    if (pipe(pipes) < 0) {
        fail_errno(net, "resolver pipe");
        return -1;
    }
    if (!nonblocking(pipes[0])) {
        (void)close(pipes[0]);
        (void)close(pipes[1]);
        fail_errno(net, "resolver flags");
        return -1;
    }
    net->resolver_pid = fork();
    if (net->resolver_pid == 0) {
        (void)close(pipes[0]);
        resolve_child(pipes[1], host, port);
    }
    (void)close(pipes[1]);
    if (net->resolver_pid < 0) {
        (void)close(pipes[0]);
        fail_errno(net, "resolver fork");
        return -1;
    }
    net->resolver_fd = pipes[0];
    net->resolved_bytes = net->next_address = 0;
    memset(&net->resolution, 0, sizeof net->resolution);
    (void)snprintf(net->host, sizeof net->host, "%s", host);
    net->deadline = now_seconds() + SETUP_SECONDS;
    set_phase(net, RESOLVING);
    return 0;
}

static void start_tls(struct posirc_net *net)
{
    unsigned char ip[sizeof(struct in6_addr)];
    int numeric = inet_pton(AF_INET, net->host, ip) == 1 ||
                  inet_pton(AF_INET6, net->host, ip) == 1;
    ERR_clear_error();
    net->tls_context = SSL_CTX_new(TLS_client_method());
    if (net->tls_context == NULL)
        goto failure;
    if (SSL_CTX_set_min_proto_version(net->tls_context, TLS1_2_VERSION) != 1)
        goto failure;
    SSL_CTX_set_verify(net->tls_context, SSL_VERIFY_PEER, NULL);
    if (net->ca_file[0] != '\0') {
        if (SSL_CTX_load_verify_locations(net->tls_context, net->ca_file, NULL) != 1)
            goto failure;
    } else if (SSL_CTX_set_default_verify_paths(net->tls_context) != 1) {
        goto failure;
    }
    net->tls = SSL_new(net->tls_context);
    if (net->tls == NULL || SSL_set_fd(net->tls, net->fd) != 1)
        goto failure;
    SSL_set_hostflags(net->tls, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (numeric) {
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(net->tls), net->host) != 1)
            goto failure;
    } else if (SSL_set1_host(net->tls, net->host) != 1 ||
               SSL_set_tlsext_host_name(net->tls, net->host) != 1) {
        goto failure;
    }
    net->handshake_want = POLLOUT;
    set_phase(net, HANDSHAKE);
    return;
failure:
    fail_tls(net, "TLS setup", SSL_ERROR_SSL);
}

/* TCP simultaneous-open can connect a loopback socket to itself when its
 * ephemeral port equals an unbound destination port. It is not the requested
 * server; continue address fallback instead of feeding our ClientHello back
 * into TLS (notably with localhost's IPv6 address on NetBSD). */
static int connected_to_self(int fd)
{
    struct sockaddr_storage local, peer;
    socklen_t local_size = sizeof local, peer_size = sizeof peer;
    if (getsockname(fd, (struct sockaddr *)&local, &local_size) < 0 ||
        getpeername(fd, (struct sockaddr *)&peer, &peer_size) < 0 ||
        local.ss_family != peer.ss_family)
        return 0;
    if (local.ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)&local;
        const struct sockaddr_in *b = (const struct sockaddr_in *)&peer;
        return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
    }
    if (local.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&local;
        const struct sockaddr_in6 *b = (const struct sockaddr_in6 *)&peer;
        return a->sin6_port == b->sin6_port && a->sin6_scope_id == b->sin6_scope_id &&
               memcmp(&a->sin6_addr, &b->sin6_addr, sizeof a->sin6_addr) == 0;
    }
    return 0;
}

static void next_address(struct posirc_net *net)
{
    if (net->fd >= 0)
        (void)close(net->fd);
    net->fd = -1;
    while (net->next_address < net->resolution.count) {
        const struct address *address = &net->resolution.addresses[net->next_address++];
        int result;
        net->fd = socket(address->value.ss_family, SOCK_STREAM, 0);
        if (net->fd < 0)
            continue;
        if (!nonblocking(net->fd)) {
            (void)close(net->fd);
            net->fd = -1;
            continue;
        }
        result = connect(net->fd, (const struct sockaddr *)&address->value, address->length);
        if (result == 0 && !connected_to_self(net->fd)) {
            start_tls(net);
            return;
        }
        if (result < 0 && (errno == EINPROGRESS || errno == EINTR)) {
            net->connect_deadline = now_seconds() + CONNECT_SECONDS;
            set_phase(net, CONNECTING);
            return;
        }
        (void)close(net->fd);
        net->fd = -1;
    }
    net_disconnect(net, "Cannot connect to any resolved server address.");
}

int net_active(const struct posirc_net *net)
{
    return net != NULL && net->phase != OFFLINE;
}

int net_ready(const struct posirc_net *net)
{
    return net != NULL && net->phase == ONLINE;
}

int net_send_tracked(struct posirc_net *net, const char *line,
                     unsigned long tag, net_sent_callback sent)
{
    size_t length, slot;
    if (!net_ready(net) || line == NULL || net->queue_count == QUEUE_CAPACITY)
        return -1;
    length = strlen(line);
    if (length == 0 || length > IRC_WIRE_CAPACITY - 2 || strpbrk(line, "\r\n") != NULL)
        return -1;
    slot = (net->queue_start + net->queue_count) % QUEUE_CAPACITY;
    memcpy(net->queue[slot].data, line, length);
    memcpy(net->queue[slot].data + length, "\r\n", 2);
    net->queue[slot].length = length + 2;
    net->queue[slot].tag = tag;
    net->queue[slot].sent = sent;
    ++net->queue_count;
    return 0;
}

int net_send(struct posirc_net *net, const char *line)
{
    return net_send_tracked(net, line, 0, NULL);
}

void net_finish(struct posirc_net *net)
{
    if (net_ready(net)) {
        net->deadline = now_seconds() + 2;
        set_phase(net, DRAINING);
    } else if (net_active(net)) {
        net_disconnect(net, "Connection cancelled.");
    }
}

static short wanted(int error)
{
    if (error == SSL_ERROR_WANT_READ) return POLLIN;
    if (error == SSL_ERROR_WANT_WRITE) return POLLOUT;
    return 0;
}

static void handshake(struct posirc_net *net)
{
    int result, error;
    ERR_clear_error();
    result = SSL_connect(net->tls);
    if (result == 1) {
        set_phase(net, ONLINE);
        if (net_ready(net))
            emit(net, NET_READY, SSL_get_version(net->tls));
        return;
    }
    error = SSL_get_error(net->tls, result);
    net->handshake_want = wanted(error);
    if (net->handshake_want == 0)
        fail_tls(net, "TLS handshake", error);
}

static void write_queue(struct posirc_net *net)
{
    struct outgoing *message;
    int result, error;
    if (net->queue_count == 0)
        return;
    message = &net->queue[net->queue_start];
    ERR_clear_error();
    /* The queue slot, pointer and length remain unchanged on WANT_*, even
     * when new lines are enqueued. This is required by SSL_write. */
    result = SSL_write(net->tls, message->data + net->write_offset,
                       (int)(message->length - net->write_offset));
    if (result > 0) {
        net->write_want = 0;
        net->write_offset += (size_t)result;
        if (net->write_offset == message->length) {
            net->write_offset = 0;
            net->queue_start = (net->queue_start + 1) % QUEUE_CAPACITY;
            --net->queue_count;
            if (message->sent != NULL) message->sent(net->context, message->tag);
        }
        return;
    }
    error = SSL_get_error(net->tls, result);
    net->write_want = wanted(error);
    if (net->write_want == 0)
        fail_tls(net, "TLS write", error);
}

static void read_data(struct posirc_net *net)
{
    int result, error;
    ERR_clear_error();
    result = SSL_read(net->tls, net->read_buffer, (int)sizeof net->read_buffer);
    if (result > 0) {
        net->read_want = POLLIN;
        net->callback(net->context, NET_DATA, net->read_buffer, (size_t)result);
        return;
    }
    error = SSL_get_error(net->tls, result);
    net->read_want = wanted(error);
    if (net->read_want == 0)
        fail_tls(net, "TLS read", error);
}

static void read_resolution(struct posirc_net *net)
{
    ssize_t count = read(net->resolver_fd, (char *)&net->resolution + net->resolved_bytes,
                         sizeof net->resolution - net->resolved_bytes);
    if (count > 0) {
        net->resolved_bytes += (size_t)count;
        if (net->resolved_bytes == sizeof net->resolution) {
            stop_resolver(net);
            if (net->resolution.error != 0)
                net_disconnect(net, gai_strerror(net->resolution.error));
            else
                next_address(net);
        }
    } else if (count == 0) {
        net_disconnect(net, "Resolver ended without a complete result.");
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        fail_errno(net, "resolver read");
    }
}

size_t net_pending(const struct posirc_net *net)
{
    return net->queue_count;
}

void net_step(struct posirc_net *net, int input_fd, int timeout_ms)
{
    struct pollfd descriptors[2];
    short events = 0, ready;
    int result;
    double now = now_seconds();
    if (net_active(net) && net->phase != ONLINE && now >= net->deadline) {
        net_disconnect(net, "Connection timed out.");
        return;
    }
    if (net->phase == CONNECTING && now >= net->connect_deadline)
        next_address(net);
    if (net->phase == RESOLVING) events = POLLIN;
    if (net->phase == CONNECTING) events = POLLOUT;
    if (net->phase == HANDSHAKE) events = net->handshake_want;
    if (net->phase == ONLINE || net->phase == DRAINING) {
        if (net->read_want == POLLOUT)
            events = POLLOUT;
        else if (net->write_want != 0)
            events = net->write_want;
        else
            events = (short)(POLLIN | (net->queue_count > 0 ? POLLOUT : 0));
        if (net->write_want == 0 && net->read_want != POLLOUT && SSL_pending(net->tls) > 0)
            timeout_ms = 0;
        if (net->phase == DRAINING && net->queue_count == 0) {
            /* Best effort close_notify, never wait for the peer's reply. */
            (void)SSL_shutdown(net->tls);
            release_connection(net);
            emit(net, NET_FINISHED, "Disconnected.");
            return;
        }
    }
    descriptors[0].fd = input_fd;
    descriptors[0].events = POLLIN;
    descriptors[0].revents = 0;
    descriptors[1].fd = net->phase == RESOLVING ? net->resolver_fd : net->fd;
    descriptors[1].events = events;
    descriptors[1].revents = 0;
    result = poll(descriptors, 2, timeout_ms);
    if (result < 0) {
        if (errno != EINTR)
            fail_errno(net, "poll");
        return;
    }
    ready = descriptors[1].revents;
    if (net->phase == RESOLVING && ready != 0) {
        read_resolution(net);
    } else if (net->phase == CONNECTING && ready != 0) {
        int error = 0;
        socklen_t length = sizeof error;
        if (getsockopt(net->fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0 ||
            connected_to_self(net->fd))
            next_address(net);
        else
            start_tls(net);
    } else if (net->phase == HANDSHAKE && ready != 0) {
        handshake(net);
    } else if (net->phase == ONLINE || net->phase == DRAINING) {
        short failures = POLLERR | POLLHUP | POLLNVAL;
        /* Bound work per tick to keep input responsive under server traffic.
         * A write retry takes precedence; a read needing WRITE must finish
         * before starting a new application write. */
        if (net->read_want == POLLOUT) {
            if (ready & (POLLOUT | failures))
                read_data(net);
        } else if (net->write_want != 0) {
            if (ready & (net->write_want | failures))
                write_queue(net);
        } else {
            if (net->queue_count > 0 && (ready & (POLLOUT | failures)))
                write_queue(net);
            if (net_active(net) && net->write_want == 0 &&
                ((ready & (POLLIN | failures)) || SSL_pending(net->tls) > 0))
                read_data(net);
        }
    }
}
