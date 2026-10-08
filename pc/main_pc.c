/**
 * @file main_pc.c
 * @brief PC-Testprogramm: KiwiSDR-Audio empfangen und in WAV-Datei schreiben.
 *
 * Aufruf:
 *   kiwi_pc [Optionen] HOST
 *     -p PORT     Port (Standard 8073)
 *     -f KHZ      Frequenz in kHz (Standard 225)
 *     -m MODUS    am, amn, amw, sam, usb, lsb, cw, nbfm ... (Standard am)
 *     -t SEK      Laufzeit in Sekunden (0 = bis Strg-C; Standard 10)
 *     -o DATEI    Ausgabe WAV (Standard out.wav; "-" = roh S16LE nach stdout)
 *     -c          ADPCM-Kompression anfordern (Standard: rohes PCM)
 *     -w PASSWORT Kiwi-Passwort
 *     -l          alter Pfad "/<ts>/SND" statt "/ws/kiwi/<ts>/SND" (Kiwi < v1.9xx)
 *     -i          interaktiv: Befehle von stdin (f 1000 / m usb / s / q);
 *                 dabei keine automatische Statuszeile (Status mit 's')
 *
 * WICHTIG: Es wird immer genau EINE Verbindung aufgebaut und am Ende (auch
 * bei Strg-C oder Fehler) sauber geschlossen.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "kiwi_client.h"
#include "sink_wav.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static kiwi_client_t g_client;   /* ~5 KB: statisch statt auf dem Stack */

int main(int argc, char **argv)
{
    const char *host = NULL, *mode = "am", *out = "out.wav", *pw = "";
    unsigned port = 8073;
    uint32_t freq_hz = 225000;
    unsigned secs = 10;
    int compression = 0, interactive = 0, legacy = 0, opt;

    while ((opt = getopt(argc, argv, "p:f:m:t:o:cw:ilh")) != -1) {
        switch (opt) {
        case 'p': port = (unsigned)atoi(optarg); break;
        case 'f':
            if (kiwi_parse_khz(optarg, &freq_hz)) { fprintf(stderr, "Ungueltige Frequenz\n"); return 2; }
            break;
        case 'm': mode = optarg; break;
        case 't': secs = (unsigned)atoi(optarg); break;
        case 'o': out = optarg; break;
        case 'c': compression = 1; break;
        case 'w': pw = optarg; break;
        case 'i': interactive = 1; break;
        case 'l': legacy = 1; break;
        default:
            fprintf(stderr, "Aufruf: %s [-p port] [-f kHz] [-m modus] [-t sek] [-o datei.wav] [-c] [-w pw] [-l] [-i] HOST\n", argv[0]);
            return 2;
        }
    }
    if (optind >= argc) { fprintf(stderr, "HOST fehlt\n"); return 2; }
    host = argv[optind];

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    audio_sink_t *sink = sink_wav_create(out);
    if (!sink) return 1;

    kiwi_config_t cfg = {
        .host = host, .port = (uint16_t)port, .password = pw,
        .user_name = "kiwi_pc_test", .freq_hz = freq_hz, .mode = mode,
        .compression = compression, .sink = sink,
        .path_prefix = legacy ? "" : NULL,
    };

    int ret = 1;
    kiwi_err_t rc = kiwi_client_init(&g_client, &cfg);
    if (rc) { fprintf(stderr, "Konfiguration ungueltig: %s\n", kiwi_strerror(rc)); goto done; }

    fprintf(stderr, "Verbinde %s:%u, %lu.%03lu kHz, Modus %s ...\n", host, port,
            (unsigned long)(freq_hz / 1000), (unsigned long)(freq_hz % 1000), mode);
    rc = kiwi_client_connect(&g_client);
    if (rc) { fprintf(stderr, "Verbindung fehlgeschlagen: %s\n", kiwi_strerror(rc)); goto done; }

    uint32_t t_start = kiwi_time_ms(), t_stat = t_start;
    if (interactive) fprintf(stderr, "Befehle (mit Enter): f <kHz> | m <modus> | s (Status) | q (Ende)\n");
    while (!g_stop) {
        rc = kiwi_client_poll(&g_client, 100);
        if (rc) { fprintf(stderr, "\nBeendet: %s\n", kiwi_strerror(rc)); break; }

        uint32_t now = kiwi_time_ms();
        if (secs && now - t_start >= secs * 1000u) { ret = 0; break; }

        if (interactive) {                       /* stdin ohne Blockieren pruefen */
            fd_set rf; struct timeval tv = {0, 0};
            FD_ZERO(&rf); FD_SET(0, &rf);
            if (select(1, &rf, NULL, NULL, &tv) > 0) {
                char line[64], reply[200];
                if (!fgets(line, sizeof line, stdin)) { interactive = 0; }
                else {
                    int cr = kiwi_client_command(&g_client, line, reply, sizeof reply);
                    fprintf(stderr, "%s\n", reply);
                    if (cr == 1) { ret = 0; break; }
                }
            }
        }
        /* Im interaktiven Modus KEINE automatische Statuszeile: sie wuerde mit \r
         * die Zeile ueberschreiben, in der gerade getippt wird. Status per Befehl 's'. */
        if (!interactive && now - t_stat >= 1000u) {   /* 1x pro Sekunde Status */
            char st[200];
            kiwi_client_command(&g_client, "s", st, sizeof st);
            fprintf(stderr, "\r%s   ", st);
            t_stat = now;
        }
    }
    if (g_stop) ret = 0;
    fprintf(stderr, "\n");

done:
    kiwi_client_close(&g_client);   /* IMMER: Close-Frame + TCP schliessen + WAV finalisieren */
    sink_wav_destroy(sink);
    if (ret == 0) fprintf(stderr, "Fertig, Audio in %s\n", out);
    return ret;
}
