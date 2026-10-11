#ifndef REBOX_JEOPARDY_NET_H
#define REBOX_JEOPARDY_NET_H
/*
 * Bounded, non-blocking LAN HTTP + WebSocket server for the Jeopardy service.
 *
 * The poll loop owns every connection and every game action (single-thread
 * serialization). HTTP request parsing and RFC6455 framing live here; all game
 * authority lives in jeopardy_service. Client websocket frames must be masked;
 * unmasked or oversized/badly framed input is rejected. Slow clients never
 * block the loop because sockets are non-blocking and every buffer is capped.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct JpqNet JpqNet;
typedef uint32_t JpqConnId;

typedef struct {
    JpqConnId conn;
    char method[8];
    char path[1024];
    char query[1024];
    char ip[64];
    const char *body;
    size_t body_len;
} JpqHttpRequest;

/* Return a malloc'd response body (freed by the caller) or NULL for none. */
typedef char *(*JpqHttpFn)(void *ud, const JpqHttpRequest *req, size_t *out_len, const char **ctype, int *status);
typedef int (*JpqWsPathFn)(void *ud, const JpqHttpRequest *req);
typedef void (*JpqWsOpenFn)(void *ud, JpqConnId conn);
typedef void (*JpqWsTextFn)(void *ud, JpqConnId conn, const char *data, size_t len);
typedef void (*JpqWsCloseFn)(void *ud, JpqConnId conn, int code);
typedef void (*JpqLineFn)(void *ud, JpqConnId conn, const char *line, size_t len);
typedef void (*JpqTickFn)(void *ud, uint64_t now_ms);

typedef struct {
    JpqHttpFn http;
    JpqWsPathFn ws_path;
    JpqWsOpenFn ws_open;
    JpqWsTextFn ws_text;
    JpqWsCloseFn ws_close;
    JpqLineFn line;
    JpqTickFn tick;
} JpqNetOps;

JpqNet *jpq_net_create(const JpqNetOps *ops, void *ud);
/* Bind a TCP listener; port 0 picks an ephemeral port reported by *port_out. */
int jpq_net_listen_tcp(JpqNet *n, const char *host, int port, int *port_out);
/* Bind a Unix stream listener whose newline-delimited lines go to ops.line. */
int jpq_net_listen_unix_lines(JpqNet *n, const char *path);
void jpq_net_run(JpqNet *n);
void jpq_net_stop(JpqNet *n);
void jpq_net_destroy(JpqNet *n);
int jpq_net_ws_send(JpqNet *n, JpqConnId conn, const char *data, size_t len);
/* Queue a newline-terminated reply on a private line-channel connection.
 * Returns -1 (and queues nothing) when conn is not a line connection. */
int jpq_net_line_send(JpqNet *n, JpqConnId conn, const char *data, size_t len);
/* Request a graceful (or coded) close once queued frames flush. */
void jpq_net_conn_close(JpqNet *n, JpqConnId conn, int code);
int jpq_net_bound_port(JpqNet *n);
#endif
