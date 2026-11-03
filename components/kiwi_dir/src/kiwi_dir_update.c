/**
 * @file kiwi_dir_update.c
 * @brief Update-Ablauf. Konzept und Sicherheitsregeln: kiwi_dir_update.h.
 */
#include "kiwi_dir_update.h"

#include <stdatomic.h>
#include <string.h>

/* Genau ein Update gleichzeitig (auch aus verschiedenen Tasks). */
static atomic_flag s_busy = ATOMIC_FLAG_INIT;

/** Der Parser hat einen Server fertig: in den Slot schreiben. */
static int on_item(void *user, const kiwi_dir_item_t *it)
{
    kiwi_dir_update_t *u = (kiwi_dir_update_t *)user;
    if (!u->writer_open) {
        /* Erster Server: JETZT erst den inaktiven Slot loeschen (siehe Kopfkommentar). */
        int act = kiwi_dir_db_active_slot(u->db);
        int slot = act < 0 ? 0 : 1 - act;
        int rc = kiwi_dir_writer_begin(&u->writer, u->st, slot, kiwi_dir_db_generation(u->db) + 1);
        if (rc) { u->err = rc; return 1; }
        u->writer_open = 1;
    }
    kiwi_dir_rec_t rec = it->rec;                 /* Kopie: Land-ID eintragen */
    rec.country = kiwi_dir_writer_country(&u->writer, it->country);
    int rc = kiwi_dir_writer_add(&u->writer, &rec);
    if (rc < 0) { u->err = rc; return 1; }        /* rc == 1: Slot voll -> nur gezaehlt */
    return 0;
}

int kiwi_dir_update_begin(kiwi_dir_update_t *u, const kiwi_dir_store_t *st, kiwi_dir_db_t *db)
{
    if (!u || !st || !db) return KIWI_DIR_ERR_ARG;
    if (atomic_flag_test_and_set(&s_busy)) return KIWI_DIR_ERR_BUSY;
    memset(u, 0, sizeof *u);
    u->st = st;
    u->db = db;
    u->locked = 1;
    kiwi_dir_parser_init(&u->parser, on_item, u);
    return KIWI_DIR_OK;
}

int kiwi_dir_update_push(kiwi_dir_update_t *u, const uint8_t *data, size_t n)
{
    if (!u || !u->locked) return KIWI_DIR_ERR_ARG;
    u->bytes += (uint32_t)n;
    if (kiwi_dir_parser_push(&u->parser, data, n) != 0) return u->err ? u->err : KIWI_DIR_ERR_PARSE;
    return KIWI_DIR_OK;
}

void kiwi_dir_update_abort(kiwi_dir_update_t *u)
{
    if (u && u->locked) {
        u->locked = 0;
        atomic_flag_clear(&s_busy);
    }
}

int kiwi_dir_update_end(kiwi_dir_update_t *u, kiwi_dir_result_t *res)
{
    if (!u || !u->locked) return KIWI_DIR_ERR_ARG;
    int rc = KIWI_DIR_OK;
    uint32_t old = kiwi_dir_db_count(u->db);

    if (u->err) rc = u->err;
    else if (!kiwi_dir_parser_finished(&u->parser) || !u->writer_open) rc = KIWI_DIR_ERR_PARSE;   /* abgeschnitten/leer */
    else if (old >= 40 && u->writer.count < old / 4) rc = KIWI_DIR_ERR_SUSPECT;

    if (rc == KIWI_DIR_OK) rc = kiwi_dir_writer_finish(&u->writer);             /* COMMIT */
    if (rc == KIWI_DIR_OK) {
        /* Neu einlesen (mit Pruefsumme): erst jetzt wird der neue Slot "aktiv". */
        int orc = kiwi_dir_db_open(u->db, u->st, 1);
        if (orc != KIWI_DIR_OK || kiwi_dir_db_active_slot(u->db) != u->writer.slot ||
            kiwi_dir_db_generation(u->db) != u->writer.generation) rc = KIWI_DIR_ERR_STORE;
    }
    if (res) {
        memset(res, 0, sizeof *res);
        res->entries = u->writer.count;
        res->skipped = u->parser.stats.skipped;
        res->overflow = u->writer.overflow;
        res->countries = u->writer.ncountry;
        res->bytes = u->bytes;
        res->generation = u->writer.generation;
        res->old_entries = old;
    }
    kiwi_dir_update_abort(u);                      /* Sperre freigeben */
    return rc;
}

typedef struct { kiwi_dir_update_t *u; kiwi_dir_progress_cb cb; void *user; uint32_t next; } feed_t;

static int feed(void *user, const uint8_t *d, size_t n)
{
    feed_t *f = (feed_t *)user;
    int rc = kiwi_dir_update_push(f->u, d, n);
    if (f->cb && f->u->bytes >= f->next) {
        f->cb(f->user, f->u->bytes, f->u->parser.stats.accepted);
        f->next = f->u->bytes + 32768u;
    }
    return rc;
}

int kiwi_dir_update_http(kiwi_dir_update_t *u, const kiwi_dir_store_t *st, kiwi_dir_db_t *db,
                         const char *url, kiwi_dir_progress_cb progress, void *user, kiwi_dir_result_t *res)
{
    int rc = kiwi_dir_update_begin(u, st, db);
    if (rc) return rc;
    feed_t f = { u, progress, user, 0 };
    kiwi_dir_http_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.url = url;
    rc = kiwi_dir_http_get(&cfg, feed, &f, NULL);
    if (rc == KIWI_DIR_ERR_PARSE && u->err) rc = u->err;      /* genauere Ursache (z. B. Speicherfehler) */
    if (rc != KIWI_DIR_OK) { kiwi_dir_update_abort(u); return rc; }
    return kiwi_dir_update_end(u, res);
}
