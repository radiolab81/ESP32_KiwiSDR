/**
 * @file lwipopts.h
 * @brief lwIP-Konfiguration: Sockets-API unter FreeRTOS (NO_SYS = 0).
 *
 * Die Sockets-API (lwip_socket/connect/send/recv/poll) braucht einen Betriebssystem-Rahmen
 * (tcpip_thread). Darum arbeitet dieser Port mit pico_cyw43_arch_lwip_sys_freertos statt mit
 * dem Polling-/Background-Betrieb ohne OS.
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ---- Betriebssystem ----------------------------------------------------- */
#define NO_SYS                          0
#define LWIP_SOCKET                     1
#define LWIP_NETCONN                    1
#define LWIP_COMPAT_SOCKETS             0      /* wir rufen lwip_* explizit auf (kein Namenskonflikt mit newlib) */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_TCPIP_CORE_LOCKING         1
#define LWIP_TCPIP_CORE_LOCKING_INPUT   1
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define SO_REUSE                        1
#define LWIP_SOCKET_POLL                1      /* lwip_poll() wird in port_lwip.c benutzt */
#define LWIP_SOCKET_SELECT              1
#define LWIP_TIMEVAL_PRIVATE            0      /* struct timeval kommt aus newlib <sys/time.h> */

/* Taskparameter fuer sys_arch.c (Stackgroesse in WORTEN = 4 Byte). */
#define TCPIP_THREAD_STACKSIZE          1024
#define TCPIP_THREAD_PRIO               5
#define DEFAULT_THREAD_STACKSIZE        1024
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       16     /* wie CONFIG_LWIP_TCP_RECVMBOX_SIZE im ESP32-Projekt */
#define DEFAULT_ACCEPTMBOX_SIZE         4
#define TCPIP_MBOX_SIZE                 16

/* ---- Speicher ----------------------------------------------------------- */
#define MEM_LIBC_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (24 * 1024)
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_ARP_QUEUE              10
#define MEMP_NUM_NETCONN                8
#define MEMP_NUM_NETBUF                 4
#define PBUF_POOL_SIZE                  24
#define PBUF_POOL_BUFSIZE               LWIP_MEM_ALIGN_SIZE(TCP_MSS + 40 + PBUF_LINK_ENCAPSULATION_HLEN + PBUF_LINK_HLEN)

/* ---- Protokolle --------------------------------------------------------- */
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_IGMP                       0
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_TX_SINGLE_PBUF       1
#define DHCP_DOES_ARP_CHECK             0
#define LWIP_DHCP_DOES_ACD_CHECK        0

/* ---- TCP: robust gegen WLAN-Jitter (wie CONFIG_LWIP_TCP_WND_DEFAULT beim ESP32) ---- */
#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)       /* 11680 */
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE              1

/* ---- Pruefsummen/Statistik --------------------------------------------- */
#define LWIP_CHKSUM_ALGORITHM           3
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
#define MEM_STATS                       0
#define SYS_STATS                       0
#define MEMP_STATS                      0
#define LINK_STATS                      0

#ifndef NDEBUG
#define LWIP_DEBUG                      0
#endif

#endif /* LWIPOPTS_H */
