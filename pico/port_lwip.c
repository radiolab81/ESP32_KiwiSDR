/**
 * @file port_lwip.c
 * @brief kiwi_port.h auf lwIP-Sockets (FreeRTOS) - Gegenstueck zu components/kiwi_core/src/port_sockets.c.
 *
 * WARUM EINE EIGENE DATEI?
 * port_sockets.c bleibt unveraendert (PC + ESP-IDF). Auf dem Pico fehlen in newlib <netdb.h>,
 * und lwIP liefert seine Socket-Funktionen mit dem Praefix lwip_ (LWIP_COMPAT_SOCKETS=0, damit
 * es keine Namenskollisionen mit newlib gibt). Genau dafuer ist die Portschicht kiwi_port.h da:
 * Nur diese 6 Funktionen kennen das Betriebssystem.
 *
 * ZUR ERRNO-FRAGE
 * errno ist unter FreeRTOS ohne Newlib-Reentrancy NICHT pro Task getrennt (der Kiwi-Task und der
 * Verzeichnis-Update-Task laufen gleichzeitig). Deshalb liest dieser Port errno nie. Stattdessen
 * wird vor jedem send()/recv() mit lwip_poll() gewartet und ein Fehlschlag ueber SO_ERROR
 * (liefert den Fehler direkt, ohne errno) von "kein Fehler / nur zu frueh" unterschieden.
 */
#include "kiwi_port.h"

#include <string.h>

#include "FreeRTOS.h"
#include "pico/rand.h"
#include "pico/time.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

struct kiwi_net {
    int fd;
};

/** Offener Socketfehler (SO_ERROR); 0 = keiner. Setzt den Fehler zurueck (lwIP-Semantik). */
static int sock_error(int fd)
{
    int err = 0;
    socklen_t len = sizeof err;
    if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return -1;
    return err;
}

/** Wartet bis fd lesbar (want_write=0) bzw. schreibbar (1). 1=bereit, 0=Timeout, -1=Fehler/Abbruch. */
static int wait_fd(int fd, int want_write, uint32_t timeout_ms)
{
    struct pollfd p;
    p.fd = fd;
    p.events = want_write ? POLLOUT : POLLIN;
    p.revents = 0;
    int r = lwip_poll(&p, 1, (int)timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return -1;
    /* POLLHUP bei lesen: recv() liefert gleich 0 (= geschlossen) - das soll der Aufrufer sehen. */
    return 1;
}

int kiwi_net_connect(kiwi_net_t **out, const char *host, uint16_t port, uint32_t timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;           /* IPv4 genuegt (LWIP_IPV6 = 0) */
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    if (lwip_getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

    for (ai = res; ai; ai = ai->ai_next) {
        fd = lwip_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int on = 1;
        lwip_ioctl(fd, FIONBIO, &on);    /* nicht blockierend -> Zeitlimit per poll */

        int r = lwip_connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r == 0) break;               /* sofort verbunden */
        /* Fehlschlag ohne errno: bei nicht blockierendem connect heisst r<0 normalerweise
         * "laeuft" (EINPROGRESS); ein echter Fehler zeigt sich gleich ueber SO_ERROR. */
        if (wait_fd(fd, 1, timeout_ms) == 1 && sock_error(fd) == 0) break;
        lwip_close(fd);
        fd = -1;
    }
    lwip_freeaddrinfo(res);
    if (fd < 0) return -1;

    kiwi_net_t *n = (kiwi_net_t *)pvPortMalloc(sizeof *n);
    if (!n) { lwip_close(fd); return -1; }
    n->fd = fd;
    *out = n;
    return 0;
}

int kiwi_net_send_all(kiwi_net_t *n, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    size_t sent = 0;
    while (sent < len) {
        if (wait_fd(n->fd, 1, timeout_ms) != 1) return -1;
        ssize_t r = lwip_send(n->fd, data + sent, len - sent, 0);
        if (r > 0) { sent += (size_t)r; continue; }
        if (r < 0 && sock_error(n->fd) == 0) continue;   /* nur "noch nicht" (EAGAIN): erneut warten */
        return -1;
    }
    return 0;
}

int kiwi_net_recv(kiwi_net_t *n, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    int w = wait_fd(n->fd, 0, timeout_ms);
    if (w < 0) return KIWI_NET_ERR;
    if (w == 0) return KIWI_NET_TIMEOUT;

    ssize_t r = lwip_recv(n->fd, buf, cap, 0);
    if (r > 0) return (int)r;
    if (r == 0) return KIWI_NET_CLOSED;              /* geordnetes Ende (FIN) */
    return sock_error(n->fd) == 0 ? KIWI_NET_TIMEOUT : KIWI_NET_ERR;
}

void kiwi_net_close(kiwi_net_t *n)
{
    if (!n) return;
    lwip_shutdown(n->fd, SHUT_RDWR);
    lwip_close(n->fd);
    vPortFree(n);
}

uint32_t kiwi_time_ms(void)
{
    return (uint32_t)(time_us_64() / 1000u);         /* monoton, darf ueberlaufen */
}

uint32_t kiwi_random_u32(void)
{
    return get_rand_32();                            /* RP2350: TRNG, RP2040: Ringoszillator/Bus-Rauschen */
}
