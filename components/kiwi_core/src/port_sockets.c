/**
 * @file port_sockets.c
 * @brief Port auf BSD-Sockets - gleiche Datei fuer Linux/macOS UND ESP-IDF (lwIP).
 *
 * LEHRBUCH-HINTERGRUND: Nicht-blockierende Sockets + select()
 * -----------------------------------------------------------
 * Ein blockierendes connect()/recv() kann bei Netzproblemen minutenlang
 * haengen. Eingebettete Systeme brauchen aber verlaesslich ZEITLIMITS.
 * Daher: Socket auf "non-blocking" stellen und mit select() gezielt auf
 * "lesbar"/"schreibbar" warten - jeweils mit Timeout.
 */
#include "kiwi_port.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

/* MSG_NOSIGNAL verhindert unter Linux ein SIGPIPE beim Senden auf einen
 * bereits geschlossenen Socket. Nicht ueberall vorhanden -> optional. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct kiwi_net {
    int fd;
};

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/** Wartet bis fd lesbar (want_write=0) bzw. schreibbar (1). 1=bereit, 0=Timeout, -1=Fehler */
static int wait_fd(int fd, int want_write, uint32_t timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = (time_t)(timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
    int r = select(fd + 1, want_write ? NULL : &set, want_write ? &set : NULL, NULL, &tv);
    if (r < 0) return (errno == EINTR) ? 0 : -1;
    return r > 0 ? 1 : 0;
}

int kiwi_net_connect(kiwi_net_t **out, const char *host, uint16_t port, uint32_t timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;           /* IPv4 genuegt; vermeidet IPv6-Sonderfaelle */
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_nonblock(fd);

        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r == 0) break;                                 /* sofort verbunden */
        if (errno == EINPROGRESS && wait_fd(fd, 1, timeout_ms) == 1) {
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    kiwi_net_t *n = (kiwi_net_t *)malloc(sizeof *n);
    if (!n) { close(fd); return -1; }
    n->fd = fd;
    *out = n;
    return 0;
}

int kiwi_net_send_all(kiwi_net_t *n, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(n->fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (r > 0) { sent += (size_t)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (wait_fd(n->fd, 1, timeout_ms) != 1) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

int kiwi_net_recv(kiwi_net_t *n, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    int w = wait_fd(n->fd, 0, timeout_ms);
    if (w < 0) return KIWI_NET_ERR;
    if (w == 0) return KIWI_NET_TIMEOUT;

    ssize_t r = recv(n->fd, buf, cap, 0);
    if (r > 0) return (int)r;
    if (r == 0) return KIWI_NET_CLOSED;              /* orderly shutdown */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return KIWI_NET_TIMEOUT;
    return KIWI_NET_ERR;
}

void kiwi_net_close(kiwi_net_t *n)
{
    if (!n) return;
    shutdown(n->fd, SHUT_RDWR);
    close(n->fd);
    free(n);
}

uint32_t kiwi_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

uint32_t kiwi_random_u32(void)
{
#ifdef ESP_PLATFORM
    return esp_random();                 /* Hardware-Zufallsgenerator des ESP32 */
#else
    static uint32_t x = 0;
    if (!x) {                            /* einmalig aus Zeit/PID mischen */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        x = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec * 2654435761u ^ (uint32_t)getpid();
        if (!x) x = 0x9E3779B9u;
    }
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;   /* xorshift32 */
    return x;
#endif
}
