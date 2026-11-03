/**
 * @file kiwi_dir_db.c
 * @brief Datenbank: Speicherformat, Lesen, Filtern, Schreiben (A/B-Slots).
 * Dokumentation der Konzepte: kiwi_dir_db.h und kiwi_dir_store.h.
 */
#include "kiwi_dir_db.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

const char *kiwi_dir_strerror(int err)
{
    switch (err) {
    case KIWI_DIR_OK:          return "ok";
    case KIWI_DIR_ERR_ARG:     return "ungueltiger Parameter";
    case KIWI_DIR_ERR_NET:     return "Netzwerkfehler";
    case KIWI_DIR_ERR_HTTP:    return "Verzeichnisserver antwortet nicht mit 200 OK";
    case KIWI_DIR_ERR_TLS:     return "https:// wird nicht unterstuetzt (nur http://)";
    case KIWI_DIR_ERR_PARSE:   return "Antwort enthaelt keine brauchbare Serverliste";
    case KIWI_DIR_ERR_STORE:   return "Speicherfehler (Flash/Datei)";
    case KIWI_DIR_ERR_TOO_BIG: return "Antwort zu gross";
    case KIWI_DIR_ERR_TIMEOUT: return "Zeitueberschreitung";
    case KIWI_DIR_ERR_EMPTY:   return "keine Serverdatenbank vorhanden (erst 'd update')";
    case KIWI_DIR_ERR_BUSY:    return "Update laeuft bereits";
    case KIWI_DIR_ERR_RANGE:   return "Eintrag nicht vorhanden";
    case KIWI_DIR_ERR_SUSPECT: return "neue Liste auffaellig klein, alte Datenbank bleibt";
    default:                   return "unbekannter Fehler";
    }
}

/* ================================================================== */
/* CRC-32                                                             */
/* ================================================================== */

/* Halbbyte-Tabelle (16 Eintraege) statt 256: spart 960 Byte Flash, kostet
 * nur etwas Rechenzeit. Polynom 0xEDB88320, identisch zu zlib/PNG. */
uint32_t kiwi_dir_crc32(uint32_t crc, const void *data, size_t n)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15u];
        crc = (crc >> 4) ^ t[crc & 15u];
    }
    return ~crc;
}

/* ================================================================== */
/* Hilfsfunktionen                                                    */
/* ================================================================== */

/** ASCII-Kleinbuchstabe (locale-unabhaengig; UTF-8-Bytes bleiben unveraendert). */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ci_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (lc(*a) != lc(*b)) return 0;
    return *a == *b;
}

/** Teilstring-Suche ohne Beachtung der Gross-/Kleinschreibung. */
static int ci_contains(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && lc(*h) == lc(*n)) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

uint32_t kiwi_dir_capacity(const kiwi_dir_store_t *st)
{
    if (!st || st->slot_size <= KIWI_DIR_OFF_RECORDS) return 0;
    return (uint32_t)((st->slot_size - KIWI_DIR_OFF_RECORDS) / sizeof(kiwi_dir_rec_t));
}

static int st_read(const kiwi_dir_store_t *st, int slot, size_t off, void *buf, size_t len)
{
    return st->read(st->ctx, slot, off, buf, len) == 0 ? KIWI_DIR_OK : KIWI_DIR_ERR_STORE;
}

/* ================================================================== */
/* Lesen                                                              */
/* ================================================================== */

/** Liest und prueft den Header eines Slots. 1 = gueltig. */
static int read_hdr(const kiwi_dir_store_t *st, int slot, kiwi_dir_hdr_t *h)
{
    if (st_read(st, slot, KIWI_DIR_OFF_HDR, h, sizeof *h) != 0) return 0;
    if (h->magic != KIWI_DIR_MAGIC || h->version != KIWI_DIR_FMT_VERSION) return 0;
    if (h->rec_size != sizeof(kiwi_dir_rec_t) || h->country_len != KIWI_DIR_COUNTRY_LEN) return 0;
    if (h->hdr_crc != kiwi_dir_crc32(0, h, offsetof(kiwi_dir_hdr_t, hdr_crc))) return 0;
    if (h->count > kiwi_dir_capacity(st) || h->country_count > KIWI_DIR_MAX_COUNTRIES) return 0;
    return 1;
}

/** Prueft die Pruefsumme ueber Datensaetze + Laendertabelle (liest den Slot einmal). */
static int verify_body(const kiwi_dir_store_t *st, int slot, const kiwi_dir_hdr_t *h)
{
    uint8_t buf[256];
    uint32_t crc = 0;
    size_t len = (size_t)h->count * sizeof(kiwi_dir_rec_t), off = 0;
    while (off < len) {
        size_t k = len - off < sizeof buf ? len - off : sizeof buf;
        if (st_read(st, slot, KIWI_DIR_OFF_RECORDS + off, buf, k)) return 0;
        crc = kiwi_dir_crc32(crc, buf, k);
        off += k;
    }
    len = (size_t)h->country_count * KIWI_DIR_COUNTRY_LEN;
    for (off = 0; off < len; ) {
        size_t k = len - off < sizeof buf ? len - off : sizeof buf;
        if (st_read(st, slot, KIWI_DIR_OFF_COUNTRIES + off, buf, k)) return 0;
        crc = kiwi_dir_crc32(crc, buf, k);
        off += k;
    }
    return crc == h->body_crc;
}

int kiwi_dir_db_open(kiwi_dir_db_t *db, const kiwi_dir_store_t *st, int verify)
{
    if (!db || !st || !st->read || st->slot_size <= KIWI_DIR_OFF_RECORDS) return KIWI_DIR_ERR_ARG;

    kiwi_dir_hdr_t h[KIWI_DIR_SLOTS];
    int best = -1;
    for (int s = 0; s < KIWI_DIR_SLOTS; s++) {
        int ok = read_hdr(st, s, &h[s]);
        if (ok && verify) ok = verify_body(st, s, &h[s]);
        if (!ok) { memset(&h[s], 0, sizeof h[s]); continue; }
        if (best < 0 || h[s].generation > h[best].generation) best = s;
    }
    /* Erst alles fuellen, ZULETZT den aktiven Slot setzen: ein gleichzeitig
     * lesender Task sieht damit immer einen vollstaendigen Zustand. */
    db->st = st;
    for (int s = 0; s < KIWI_DIR_SLOTS; s++) db->hdr[s] = h[s];
    atomic_store(&db->active, best);
    return best >= 0 ? KIWI_DIR_OK : KIWI_DIR_ERR_EMPTY;
}

int kiwi_dir_db_active_slot(const kiwi_dir_db_t *db) { return db ? atomic_load(&db->active) : -1; }
int kiwi_dir_db_valid(const kiwi_dir_db_t *db) { return kiwi_dir_db_active_slot(db) >= 0; }

uint32_t kiwi_dir_db_count(const kiwi_dir_db_t *db)
{
    int s = kiwi_dir_db_active_slot(db);
    return s < 0 ? 0 : db->hdr[s].count;
}

uint32_t kiwi_dir_db_generation(const kiwi_dir_db_t *db)
{
    int s = kiwi_dir_db_active_slot(db);
    return s < 0 ? 0 : db->hdr[s].generation;
}

int kiwi_dir_db_get(const kiwi_dir_db_t *db, uint32_t idx, kiwi_dir_rec_t *out)
{
    int s = kiwi_dir_db_active_slot(db);
    if (s < 0) return KIWI_DIR_ERR_EMPTY;
    if (idx >= db->hdr[s].count) return KIWI_DIR_ERR_RANGE;
    return st_read(db->st, s, KIWI_DIR_OFF_RECORDS + (size_t)idx * sizeof *out, out, sizeof *out);
}

unsigned kiwi_dir_db_country_count(const kiwi_dir_db_t *db)
{
    int s = kiwi_dir_db_active_slot(db);
    return s < 0 ? 0 : db->hdr[s].country_count;
}

int kiwi_dir_db_country_name(const kiwi_dir_db_t *db, unsigned id, char out[KIWI_DIR_COUNTRY_LEN])
{
    int s = kiwi_dir_db_active_slot(db);
    out[0] = '\0';
    if (s < 0) return KIWI_DIR_ERR_EMPTY;
    if (id == 0 || id > db->hdr[s].country_count) return KIWI_DIR_ERR_RANGE;
    int rc = st_read(db->st, s, KIWI_DIR_OFF_COUNTRIES + (size_t)(id - 1) * KIWI_DIR_COUNTRY_LEN,
                     out, KIWI_DIR_COUNTRY_LEN);
    out[KIWI_DIR_COUNTRY_LEN - 1] = '\0';          /* Sicherheit gegen kaputte Daten */
    return rc;
}

int kiwi_dir_db_country_find(const kiwi_dir_db_t *db, const char *name, uint8_t *id)
{
    char buf[KIWI_DIR_COUNTRY_LEN];
    unsigned n = kiwi_dir_db_country_count(db);
    for (unsigned i = 1; i <= n; i++) {
        if (kiwi_dir_db_country_name(db, i, buf) == 0 && ci_equal(buf, name)) {
            *id = (uint8_t)i;
            return KIWI_DIR_OK;
        }
    }
    return KIWI_DIR_ERR_RANGE;
}

int kiwi_dir_db_count_by_country(const kiwi_dir_db_t *db, uint16_t counts[256])
{
    kiwi_dir_rec_t r;
    memset(counts, 0, 256 * sizeof counts[0]);
    uint32_t n = kiwi_dir_db_count(db);
    for (uint32_t i = 0; i < n; i++) {
        int rc = kiwi_dir_db_get(db, i, &r);
        if (rc) return rc;
        if (r.flags & KIWI_DIR_F_UNUSABLE) continue;
        if (counts[r.country] < 0xFFFFu) counts[r.country]++;
    }
    return KIWI_DIR_OK;
}

int kiwi_dir_db_lookup(const kiwi_dir_db_t *db, const char *host, uint16_t port, uint32_t *idx)
{
    kiwi_dir_rec_t r;
    uint32_t n = kiwi_dir_db_count(db);
    for (uint32_t i = 0; i < n; i++) {
        if (kiwi_dir_db_get(db, i, &r)) break;
        if (r.port == port && ci_equal(r.host, host)) { *idx = i; return KIWI_DIR_OK; }
    }
    return KIWI_DIR_ERR_RANGE;
}

/* ================================================================== */
/* Filtern                                                            */
/* ================================================================== */

void kiwi_dir_query_init(kiwi_dir_query_t *q)
{
    memset(q, 0, sizeof *q);
    q->flags_excl = KIWI_DIR_F_UNUSABLE;
}

uint8_t kiwi_dir_rec_snr(const kiwi_dir_rec_t *r)
{
    return r->snr_hf ? r->snr_hf : r->snr_all;
}

int kiwi_dir_query_match(const kiwi_dir_db_t *db, const kiwi_dir_query_t *q, const kiwi_dir_rec_t *r)
{
    if (r->flags & q->flags_excl) return 0;
    if (q->country && r->country != q->country) return 0;
    /* users_max == 0 heisst "unbekannt" -> nicht als "voll" werten. */
    if (q->only_free && r->users_max && r->users >= r->users_max) return 0;
    if (q->min_snr && kiwi_dir_rec_snr(r) < q->min_snr) return 0;
    /* Unbekannter Bereich (0/0) wird nicht ausgeschlossen. */
    if (q->freq_hz && r->band_hi_hz &&
        (q->freq_hz < r->band_lo_hz || q->freq_hz > r->band_hi_hz)) return 0;
    if (q->text[0]) {
        if (ci_contains(r->name, q->text) || ci_contains(r->place, q->text) ||
            ci_contains(r->host, q->text)) return 1;
        char c[KIWI_DIR_COUNTRY_LEN];       /* zuletzt: Landesname (kostet einen Lesezugriff) */
        return r->country && kiwi_dir_db_country_name(db, r->country, c) == 0 && ci_contains(c, q->text);
    }
    return 1;
}

int kiwi_dir_db_find(const kiwi_dir_db_t *db, const kiwi_dir_query_t *q, uint32_t *cursor,
                     kiwi_dir_rec_t *out, uint32_t *idx)
{
    uint32_t n = kiwi_dir_db_count(db);
    for (uint32_t i = *cursor; i < n; i++) {
        int rc = kiwi_dir_db_get(db, i, out);
        if (rc) return rc;
        if (kiwi_dir_query_match(db, q, out)) {
            *cursor = i + 1;
            if (idx) *idx = i;
            return 1;
        }
    }
    *cursor = n;
    return 0;
}

uint32_t kiwi_dir_db_count_matches(const kiwi_dir_db_t *db, const kiwi_dir_query_t *q)
{
    kiwi_dir_rec_t r;
    uint32_t cur = 0, n = 0;
    while (kiwi_dir_db_find(db, q, &cur, &r, NULL) == 1) n++;
    return n;
}

int kiwi_dir_db_best(const kiwi_dir_db_t *db, const kiwi_dir_query_t *q, kiwi_dir_rec_t *out, uint32_t *idx)
{
    kiwi_dir_rec_t r;
    uint32_t cur = 0, i;
    int found = 0, rc;
    while ((rc = kiwi_dir_db_find(db, q, &cur, &r, &i)) == 1) {
        int better = !found || kiwi_dir_rec_snr(&r) > kiwi_dir_rec_snr(out) ||
                     (kiwi_dir_rec_snr(&r) == kiwi_dir_rec_snr(out) && r.users < out->users);
        if (better) { *out = r; if (idx) *idx = i; found = 1; }
    }
    return rc < 0 ? rc : found;
}

/* ================================================================== */
/* Schreiben                                                          */
/* ================================================================== */

int kiwi_dir_writer_begin(kiwi_dir_writer_t *w, const kiwi_dir_store_t *st, int slot, uint32_t generation)
{
    if (!w || !st || !st->erase || !st->write || slot < 0 || slot >= KIWI_DIR_SLOTS) return KIWI_DIR_ERR_ARG;
    if (kiwi_dir_capacity(st) == 0) return KIWI_DIR_ERR_ARG;
    memset(w, 0, sizeof *w);
    w->st = st;
    w->slot = slot;
    w->generation = generation;
    w->capacity = kiwi_dir_capacity(st);
    if (st->erase(st->ctx, slot) != 0) return KIWI_DIR_ERR_STORE;
    return KIWI_DIR_OK;
}

uint8_t kiwi_dir_writer_country(kiwi_dir_writer_t *w, const char *name)
{
    if (!name || !*name) return 0;
    for (unsigned i = 0; i < w->ncountry; i++)
        if (ci_equal(w->country[i], name)) return (uint8_t)(i + 1);
    if (w->ncountry >= KIWI_DIR_MAX_COUNTRIES) return 0;
    strncpy(w->country[w->ncountry], name, KIWI_DIR_COUNTRY_LEN - 1);   /* Rest ist durch memset 0 */
    return (uint8_t)(++w->ncountry);
}

int kiwi_dir_writer_add(kiwi_dir_writer_t *w, const kiwi_dir_rec_t *rec)
{
    if (w->count >= w->capacity) { w->overflow++; return 1; }
    if (w->st->write(w->st->ctx, w->slot, KIWI_DIR_OFF_RECORDS + (size_t)w->count * sizeof *rec,
                     rec, sizeof *rec) != 0) return KIWI_DIR_ERR_STORE;
    w->crc = kiwi_dir_crc32(w->crc, rec, sizeof *rec);     /* Pruefsumme laeuft mit */
    w->count++;
    return 0;
}

int kiwi_dir_writer_finish(kiwi_dir_writer_t *w)
{
    size_t tab = (size_t)w->ncountry * KIWI_DIR_COUNTRY_LEN;
    if (tab && w->st->write(w->st->ctx, w->slot, KIWI_DIR_OFF_COUNTRIES, w->country, tab) != 0)
        return KIWI_DIR_ERR_STORE;
    w->crc = kiwi_dir_crc32(w->crc, w->country, tab);

    kiwi_dir_hdr_t h;
    memset(&h, 0, sizeof h);
    h.magic = KIWI_DIR_MAGIC;
    h.version = KIWI_DIR_FMT_VERSION;
    h.rec_size = (uint16_t)sizeof(kiwi_dir_rec_t);
    h.generation = w->generation;
    h.count = w->count;
    h.country_count = w->ncountry;
    h.country_len = KIWI_DIR_COUNTRY_LEN;
    h.body_crc = w->crc;
    h.hdr_crc = kiwi_dir_crc32(0, &h, offsetof(kiwi_dir_hdr_t, hdr_crc));

    /* COMMIT: erst dieser Schreibvorgang macht den Slot gueltig. */
    if (w->st->write(w->st->ctx, w->slot, KIWI_DIR_OFF_HDR, &h, sizeof h) != 0) return KIWI_DIR_ERR_STORE;
    return KIWI_DIR_OK;
}
