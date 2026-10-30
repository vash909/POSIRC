#ifndef POSIRC_NET_H
#define POSIRC_NET_H

#include <stddef.h>

struct posirc_net;
/* NET_FINISHED is a successful local drain; NET_CLOSED is any other closure. */
enum net_event { NET_STATUS, NET_READY, NET_DATA, NET_CLOSED, NET_FINISHED };
/* Data is borrowed until return. NET_DATA is a byte span; other events carry
 * terminated text. Callbacks may queue writes or disconnect, but not free net. */
typedef void (*net_callback)(void *, enum net_event, const char *, size_t);

struct posirc_net *net_create(const char *ca_file, net_callback callback, void *context);
void net_destroy(struct posirc_net *net);
int net_connect(struct posirc_net *net, const char *host, const char *port);
void net_disconnect(struct posirc_net *net, const char *reason);
/* Queue one complete IRC payload, without CRLF. Reject overflow/injection.
 * A successful return means queued locally, not delivered to the server. */
int net_send(struct posirc_net *net, const char *line);
/* Called once after the whole frame is accepted by SSL_write, never on failure.
 * Receives net_create's context. This is transport progress, not a delivery ACK. */
typedef void (*net_sent_callback)(void *, unsigned long);
int net_send_tracked(struct posirc_net *net, const char *line,
                     unsigned long tag, net_sent_callback sent);
/* Drain queued data and send TLS close_notify, with a two-second deadline. */
void net_finish(struct posirc_net *net);
/* Number of queued frames, including a partially written frame. */
size_t net_pending(const struct posirc_net *net);
/* Poll the transport and optionally an input descriptor (-1 to omit).
 * timeout_ms must be nonnegative. All network I/O is nonblocking. */
void net_step(struct posirc_net *net, int input_fd, int timeout_ms);
int net_active(const struct posirc_net *net);
int net_ready(const struct posirc_net *net);
const char *net_status(const struct posirc_net *net);

#endif
