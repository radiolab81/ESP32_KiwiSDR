/**
 * @file kiwi_dirtool.c
 * @brief PC-Werkzeug fuer die Server-Datenbank (gleicher Code wie auf dem ESP32).
 *
 * Aufruf:
 *   kiwi_dirtool [-d db.bin] [-u URL] [-f liste.js] ["d Befehl" ...]
 *     -d DATEI   Datenbankdatei (Standard kiwidb.bin); simuliert die Flash-Partition
 *     -u URL     Verzeichnis-URL (Standard http://rx.linkfanel.net/kiwisdr_com.js)
 *     -f DATEI   "d update" liest die Liste aus dieser lokalen Datei statt aus dem Netz
 *     weitere Argumente: Befehle, z. B.  "d update" "d filter Germany" "d list"
 *   Ohne Befehle werden Zeilen von stdin gelesen (interaktiv, Ende mit q).
 *
 * "d go ..." zeigt nur, mit welchem Aufruf man den Server mit kiwi_pc benutzt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kiwi_dir_cmd.h"
#include "kiwi_dir_update.h"
#include "store_file.h"

typedef struct {
    kiwi_dir_db_t db;
    kiwi_dir_store_t *st;
    const char *url, *file;
    kiwi_dir_update_t upd;       /* ~9 KB: statisch */
} tool_t;

static void out(void *u, const char *line) { (void)u; puts(line); }

static void progress(void *u, uint32_t bytes, uint32_t n)
{
    (void)u;
    fprintf(stderr, "\r  %lu KB, %lu Server ...", (unsigned long)(bytes / 1024), (unsigned long)n);
}

static int do_update(void *user, int force)
{
    tool_t *t = user;
    (void)force;
    kiwi_dir_result_t r;
    int rc;
    if (t->file) {
        FILE *f = fopen(t->file, "rb");
        if (!f) { printf("Datei %s nicht lesbar\n", t->file); return -1; }
        rc = kiwi_dir_update_begin(&t->upd, t->st, &t->db);
        uint8_t buf[700];                       /* absichtlich ungerade Stuecke: testet die Streaming-Faehigkeit */
        size_t n;
        while (rc == 0 && (n = fread(buf, 1, sizeof buf, f)) > 0) rc = kiwi_dir_update_push(&t->upd, buf, n);
        fclose(f);
        if (rc) kiwi_dir_update_abort(&t->upd); else rc = kiwi_dir_update_end(&t->upd, &r);
    } else {
        printf("Lade %s ...\n", t->url);
        rc = kiwi_dir_update_http(&t->upd, t->st, &t->db, t->url, progress, NULL, &r);
        fprintf(stderr, "\n");
    }
    if (rc) { printf("Update fehlgeschlagen: %s\n", kiwi_dir_strerror(rc)); return rc; }
    printf("Update ok: %lu Server (vorher %lu), %lu Laender, %lu uebersprungen, %lu ueberzaehlig, %lu KB, Generation %lu\n",
           (unsigned long)r.entries, (unsigned long)r.old_entries, (unsigned long)r.countries,
           (unsigned long)r.skipped, (unsigned long)r.overflow, (unsigned long)(r.bytes / 1024),
           (unsigned long)r.generation);
    return 0;
}

static int do_select(void *user, const kiwi_dir_rec_t *rec)
{
    (void)user;
    printf("-> ./kiwi_pc -p %u -f 225 -t 0 -o - %s\n", (unsigned)rec->port, rec->host);
    return 0;
}

int main(int argc, char **argv)
{
    static tool_t t;
    const char *dbfile = "kiwidb.bin";
    t.url = "http://rx.linkfanel.net/kiwisdr_com.js";
    int opt;
    while ((opt = getopt(argc, argv, "d:u:f:h")) != -1) {
        switch (opt) {
        case 'd': dbfile = optarg; break;
        case 'u': t.url = optarg; break;
        case 'f': t.file = optarg; break;
        default: fprintf(stderr, "Aufruf: %s [-d db.bin] [-u URL] [-f liste.js] [\"d befehl\" ...]\n", argv[0]); return 2;
        }
    }
    t.st = store_file_open(dbfile, STORE_FILE_DEFAULT_SLOT);
    if (!t.st) { fprintf(stderr, "Kann %s nicht oeffnen\n", dbfile); return 1; }
    int rc = kiwi_dir_db_open(&t.db, t.st, 1);
    if (rc && rc != KIWI_DIR_ERR_EMPTY) { fprintf(stderr, "DB: %s\n", kiwi_dir_strerror(rc)); return 1; }

    kiwi_dir_cmd_t cmd;
    kiwi_dir_cmd_init(&cmd, &t.db, out, do_update, do_select, &t);

    if (optind < argc) {
        for (int i = optind; i < argc; i++) {
            printf("> %s\n", argv[i]);
            if (!kiwi_dir_command(&cmd, argv[i])) printf("(kein d-Befehl)\n");
        }
    } else {
        char line[120];
        while (fgets(line, sizeof line, stdin)) {
            if (line[0] == 'q') break;
            if (!kiwi_dir_command(&cmd, line)) puts("Befehle beginnen mit 'd' (q = Ende)");
        }
    }
    store_file_close(t.st);
    return 0;
}
