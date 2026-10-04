/* Small static TV frontend and same-origin API proxy for the HR54.
 * Build: zig cc -target mips-linux-musleabi -mcpu=mips32 -static -O2
 *        -o hr54-www hr54_www.c
 * Usage: hr54-www DOCROOT BACKEND_IPV4 BACKEND_PORT LISTEN_PORT
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *docroot;
static struct sockaddr_in backend;

static int write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void answer(int fd, int code, const char *reason) {
    char header[256];
    int n = snprintf(header, sizeof header,
        "HTTP/1.0 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
        code, reason, strlen(reason), reason);
    if (n > 0 && (size_t)n < sizeof header) write_all(fd, header, (size_t)n);
}

static void serve_file(int fd, const char *method, const char *path) {
    const char *name = NULL, *type = NULL;
    if (!strcmp(path, "/") || !strcmp(path, "/index.html") ||
        !strcmp(path, "/tv/") || !strcmp(path, "/tv/index.html")) {
        name = "tv/index.html"; type = "text/html; charset=utf-8";
    } else if (!strcmp(path, "/tv/app.js")) {
        name = "tv/app.js"; type = "application/javascript";
    } else if (!strcmp(path, "/tv/app.css")) {
        name = "tv/app.css"; type = "text/css";
    } else {
        answer(fd, 404, "Not Found"); return;
    }
    char target[1024], header[512], buffer[8192];
    if (snprintf(target, sizeof target, "%s/%s", docroot, name) >= (int)sizeof target) {
        answer(fd, 500, "Path Too Long"); return;
    }
    int source = open(target, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (source < 0 || fstat(source, &st) || !S_ISREG(st.st_mode)) {
        if (source >= 0) close(source);
        answer(fd, 404, "Not Found"); return;
    }
    int h = snprintf(header, sizeof header,
        "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        type, (long long)st.st_size);
    if (h > 0 && (size_t)h < sizeof header && !write_all(fd, header, (size_t)h) &&
        strcmp(method, "HEAD")) {
        ssize_t n;
        while ((n = read(source, buffer, sizeof buffer)) > 0) {
            if (write_all(fd, buffer, (size_t)n)) break;
        }
    }
    close(source);
}

static void proxy_request(int fd, const char *method, const char *path,
                          const char *body, size_t have, size_t length) {
    /* MPEG-TS is a long-lived response. The per-request 90-second alarm is
       useful for ordinary clients but must not terminate a movie relay. */
    if (!strncmp(path, "/play/", 6)) alarm(0);
    int upstream = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream < 0 || connect(upstream, (struct sockaddr *)&backend, sizeof backend)) {
        if (upstream >= 0) close(upstream);
        answer(fd, 502, "Backend Unavailable"); return;
    }
    char header[4096], buffer[8192];
    int h = snprintf(header, sizeof header,
        "%s %s HTTP/1.0\r\nHost: 192.168.88.25:8130\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        method, path, length);
    if (h <= 0 || (size_t)h >= sizeof header || write_all(upstream, header, (size_t)h)) goto done;
    if (have > length) have = length;
    if (have && write_all(upstream, body, have)) goto done;
    while (have < length) {
        size_t need = length - have;
        if (need > sizeof buffer) need = sizeof buffer;
        ssize_t n = read(fd, buffer, need);
        if (n <= 0 || write_all(upstream, buffer, (size_t)n)) goto done;
        have += (size_t)n;
    }
    for (;;) {
        ssize_t n = read(upstream, buffer, sizeof buffer);
        if (n <= 0 || write_all(fd, buffer, (size_t)n)) break;
    }
done:
    close(upstream);
}

static void handle(int fd) {
    char request[16384];
    size_t used = 0;
    char *end = NULL;
    while (used < sizeof request - 1) {
        ssize_t n = read(fd, request + used, sizeof request - 1 - used);
        if (n <= 0) return;
        used += (size_t)n;
        request[used] = 0;
        end = strstr(request, "\r\n\r\n");
        if (end) break;
    }
    if (!end) { answer(fd, 431, "Headers Too Large"); return; }
    char method[8], path[2048];
    if (sscanf(request, "%7s %2047s", method, path) != 2 ||
        (strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST"))) {
        answer(fd, 400, "Bad Request"); return;
    }
    size_t length = 0;
    char *line = strstr(request, "\r\n");
    while (line && line < end) {
        line += 2;
        if (!strncasecmp(line, "Content-Length:", 15)) {
            length = (size_t)strtoul(line + 15, NULL, 10);
        }
        line = strstr(line, "\r\n");
    }
    if (length > 8192) { answer(fd, 413, "Body Too Large"); return; }
    char *body = end + 4;
    size_t have = used - (size_t)(body - request);
    if (!strncmp(path, "/api/", 5) || !strncmp(path, "/art/", 5) ||
        !strncmp(path, "/play/", 6)) {
        proxy_request(fd, method, path, body, have, length); return;
    }
    char *query = strchr(path, '?');
    if (query) *query = 0;
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        answer(fd, 405, "Method Not Allowed"); return;
    }
    serve_file(fd, method, path);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s DOCROOT BACKEND_IPV4 BACKEND_PORT LISTEN_PORT\n", argv[0]);
        return 2;
    }
    docroot = argv[1];
    memset(&backend, 0, sizeof backend);
    backend.sin_family = AF_INET;
    backend.sin_port = htons((unsigned short)atoi(argv[3]));
    if (inet_pton(AF_INET, argv[2], &backend.sin_addr) != 1) return 2;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons((unsigned short)atoi(argv[4]));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (struct sockaddr *)&local, sizeof local) || listen(listener, 12)) {
        perror("listen"); return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        pid_t child = fork();
        if (child == 0) {
            close(listener);
            alarm(90);
            handle(client);
            close(client);
            _exit(0);
        }
        close(client);
    }
}
