#include "jpq_input.h"
#include <stdint.h>
#ifdef HR54_RECEIVER
#include "receiver-include/libc.h"
#else
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

static int fd = -1;
static uint32_t owner;
static unsigned char rx[256];
static size_t used;
static unsigned char held[10];
static unsigned long last_ms[10];

static unsigned word(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}
static int words(const uint32_t *p, size_t n) {
    unsigned char data[128];
    if (n > sizeof data / 4) return -1;
    for (size_t i = 0; i < n; i++) {
        data[4 * i] = (unsigned char)(p[i] >> 24);
        data[4 * i + 1] = (unsigned char)(p[i] >> 16);
        data[4 * i + 2] = (unsigned char)(p[i] >> 8);
        data[4 * i + 3] = (unsigned char)p[i];
    }
    return send(fd, data, n * 4, MSG_NOSIGNAL) == (ssize_t)(n * 4) ? 0 : -1;
}
static int full(void *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, (char *)p + off, n - off, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

void jpq_input_close(void) {
    if (fd < 0) return;
    uint32_t message[] = {12, 4, owner};
    words(message, 3);
    close(fd);
    fd = -1;
    used = 0;
}

int jpq_input_open(void) {
    static const uint32_t keys[] = {0xe100, 0xe101, 0xe102, 0xe103, 0xe001, 0xe505, 0xe002, 0xe200, 0xe403, 0xe00e, 0xe402};
    unsigned char greeting[4], ack[256];
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {3, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout)) goto fail;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    unsigned char *port = (unsigned char *)&address.sin_port;
    port[0] = 47953 >> 8;
    port[1] = 47953 & 255;
    unsigned char *ip = (unsigned char *)&address.sin_addr;
    ip[0] = 127; ip[3] = 1;
    if (connect(fd, (const struct sockaddr *)&address, sizeof address) || full(greeting, 4)) goto fail;
    owner = word(greeting);
    uint32_t session[] = {12, 16, 0}, map[6 + sizeof keys / sizeof *keys];
    map[0] = sizeof map; map[1] = 0; map[2] = owner; map[3] = 1; map[4] = 1;
    map[5] = sizeof keys / sizeof *keys + 1;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) map[6 + i] = keys[i];
    if (words(session, 3) || words(map, sizeof map / 4) || full(ack, 4)) goto fail;
    unsigned n = word(ack);
    if (n < 12 || n > sizeof ack || n % 4 || full(ack + 4, n - 4) || word(ack + 4) != 7 || n != 12 || word(ack + 8) != 1) goto fail;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
    return 0;
fail:
    jpq_input_close();
    return -1;
}

static unsigned long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 1000ul + (unsigned long)t.tv_nsec / 1000000ul;
}

JvKey jpq_key_translate(unsigned code) {
    switch (code & 65535) {
    case 0xe100: return JV_KEY_UP;
    case 0xe101: return JV_KEY_DOWN;
    case 0xe102: return JV_KEY_LEFT;
    case 0xe103: return JV_KEY_RIGHT;
    case 0xe001: case 0xe505: return JV_KEY_OK;
    case 0xe002: return JV_KEY_BACK;
    case 0xe200: case 0xe403: return JV_KEY_RED;
    case 0xe00e: return JV_KEY_INFO;
    case 0xe402: return JV_KEY_STOP;
    default: return JV_KEY_NONE;
    }
}

int jpq_input_pump(void (*sink)(JvKey)) {
    if (fd < 0) return -1;
    for (int budget = 0; budget < 32; budget++) {
        if (used >= 4) {
            unsigned n = word(rx);
            if (n < 8 || n > sizeof rx || n % 4) return -1;
            if (used >= n) {
                if (word(rx + 4) == 8 && (n == 12 || n == 24 || n == 28)) {
                    unsigned raw = word(rx + 8);
                    JvKey k = jpq_key_translate(raw);
                    if (k) {
                        int pressed = (raw & 0x10000) != 0;
                        unsigned long now = now_ms();
                        if (!pressed) held[k] = 0;
                        else if (k == JV_KEY_STOP || !held[k] ||
                                 ((k == JV_KEY_UP || k == JV_KEY_DOWN || k == JV_KEY_LEFT || k == JV_KEY_RIGHT) && now - last_ms[k] >= 135)) {
                            held[k] = 1; last_ms[k] = now; sink(k);
                        }
                    }
                }
                used -= n;
                for (size_t i = 0; i < used; i++) rx[i] = rx[n + i];
                continue;
            }
        }
        ssize_t r = recv(fd, rx + used, sizeof rx - used, 0);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if (r <= 0) return -1;
        used += (size_t)r;
    }
    return 0;
}
