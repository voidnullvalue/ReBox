#ifndef REBOX_JEOPARDY_SERVICE_H
#define REBOX_JEOPARDY_SERVICE_H
/*
 * Authoritative Jeopardy service: owns the JpqGame, the database/history, the
 * reconnect-token sessions and PINs, and the role/view derivation. The network
 * layer calls these functions on a single thread; every action is serialized and
 * the actor is always derived from the authenticated token or a validated
 * private-native id, never from client-supplied fields.
 */
#include <stddef.h>
#include <stdint.h>
#include "jeopardy.h"
#include "jeopardy_net.h"

typedef struct JpqService JpqService;
typedef void (*JpqSendFn)(void *ud, JpqConnId conn, const char *data, size_t len);
typedef void (*JpqCloseFn)(void *ud, JpqConnId conn, int code);

enum { JPQ_ROLE_UNAUTH = 0, JPQ_ROLE_PUBLIC = 1, JPQ_ROLE_HOST = 2, JPQ_ROLE_PLAYER = 3 };

JpqService *jpq_service_create(void);
void jpq_service_destroy(JpqService *);
int jpq_service_start(JpqService *, const char *data_dir, const char *db_path);
/* Publish the bound port and public URL into the private native metadata. */
int jpq_service_set_port(JpqService *, int port, const char *public_url);
void jpq_service_set_net(JpqService *, JpqSendFn, JpqCloseFn, void *ud);
const char *jpq_service_command_socket(JpqService *);
int jpq_service_quit(JpqService *);
/* Request a host pause (used when the native surface stops). Thread-safe: sets
 * a flag applied on the service tick; never touches rules state off-thread. */
void jpq_service_request_pause(JpqService *);
/* Ask the poll thread to reset any committed quit / armed confirm before a
 * freshly launched native surface reads the public display, then acknowledge.
 * Request and ack are release/acquire atomics; the display is rewritten before
 * the ack is published, so the caller can wait for it before starting the
 * renderer and never race a stale quit=true view. */
void jpq_service_request_relaunch(JpqService *);
int jpq_service_relaunch_done(JpqService *);
/* Installed-environment gate: when enabled, gameplay actions are rejected
 * until the native surface reports active. Both use release/acquire atomics so
 * the lifecycle thread and the poll thread never race on the flag. Host-only
 * runs leave the gate disabled and behave exactly as before. */
void jpq_service_set_native_gate(JpqService *, int gate);
void jpq_service_set_native_active(JpqService *, int active);
/* Flush the current timer remainder to disk before teardown. */
void jpq_service_shutdown(JpqService *);

/* JpqNetOps adapters (ud is the JpqService). */
char *jpq_service_http(void *ud, const JpqHttpRequest *req, size_t *len, const char **ctype, int *status);
int jpq_service_ws_path(void *ud, const JpqHttpRequest *req);
void jpq_service_ws_open(void *ud, JpqConnId conn);
void jpq_service_ws_text(void *ud, JpqConnId conn, const char *data, size_t len);
void jpq_service_ws_close(void *ud, JpqConnId conn, int code);
void jpq_service_line(void *ud, JpqConnId conn, const char *line, size_t len);
void jpq_service_tick(void *ud, uint64_t now_ms);
#endif
