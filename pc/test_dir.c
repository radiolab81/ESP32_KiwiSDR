/**
 * @file test_dir.c
 * @brief Selbsttests fuer die Verzeichnisdienst-Komponente (PC, ohne Netz).
 *
 * Getestet wird: CRC-32, URL/Host/GPS-Parser, UTF-8-Kuerzung, Chunked-Decoder,
 * Streaming-Parser (Byte fuer Byte, Escapes, Unicode, Sonderfaelle), Laendererkennung,
 * Datenbank (Schreiben/Lesen/Filtern, A/B-Wechsel, Abbruch, Beschaedigung,
 * Kapazitaet), Update-Ablauf (Plausibilitaetsgrenze) und die d-Befehle.
 * Der "Flash" ist ein RAM-Store, der das Flash-Verhalten NACHBILDET: er meldet
 * einen Fehler, wenn ein Byte ohne vorheriges Loeschen ueberschrieben wird.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiwi_dir_cmd.h"
#include "kiwi_dir_geo.h"
#include "kiwi_dir_http.h"
#include "kiwi_dir_parse.h"
#include "kiwi_dir_update.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FEHLER Zeile %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

/* ---------- RAM-"Flash" ---------- */
#define SLOT 0x6000u   /* kleiner Slot: 0x3000 Kopf + 0x3000 Daten = 102 Saetze */
static uint8_t g_mem[KIWI_DIR_SLOTS][SLOT];
static int g_write_twice;
static int m_read(void *c, int s, size_t o, void *b, size_t n) { (void)c; if (o + n > SLOT) return -1; memcpy(b, g_mem[s] + o, n); return 0; }
static int m_erase(void *c, int s) { (void)c; memset(g_mem[s], 0xFF, SLOT); return 0; }
static int m_write(void *c, int s, size_t o, const void *b, size_t n)
{
    (void)c;
    if (o + n > SLOT) return -1;
    for (size_t i = 0; i < n; i++) if (g_mem[s][o + i] != 0xFF) g_write_twice++;   /* Flash: nur 1->0, einmal */
    memcpy(g_mem[s] + o, b, n);
    return 0;
}
static const kiwi_dir_store_t g_store = { SLOT, 4096, m_read, m_erase, m_write, NULL };
static void fresh(void) { m_erase(NULL, 0); m_erase(NULL, 1); g_write_twice = 0; }

/* ---------- Hilfen ---------- */
static kiwi_dir_rec_t mkrec(const char *host, int port, uint8_t country, uint8_t users, uint8_t umax, uint8_t snr)
{
    kiwi_dir_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.host, sizeof r.host, "%s", host);
    snprintf(r.name, sizeof r.name, "Name %s", host);
    r.port = (uint16_t)port; r.country = country; r.users = users; r.users_max = umax; r.snr_hf = snr;
    r.lat_cdeg = r.lon_cdeg = KIWI_DIR_NO_GEO;
    return r;
}

/* Schreibt n Dummy-Server (Land 1 = "Aland", 2 = "Bland") in den Slot gegenueber dem aktiven. */
static int write_db(kiwi_dir_db_t *db, int n, int commit)
{
    kiwi_dir_writer_t *w = malloc(sizeof *w);
    int act = kiwi_dir_db_active_slot(db), rc;
    rc = kiwi_dir_writer_begin(w, &g_store, act < 0 ? 0 : 1 - act, kiwi_dir_db_generation(db) + 1);
    if (rc) { free(w); return rc; }
    uint8_t a = kiwi_dir_writer_country(w, "Aland"), b = kiwi_dir_writer_country(w, "Bland");
    for (int i = 0; i < n; i++) {
        char h[24];
        snprintf(h, sizeof h, "h%d.example", i);
        kiwi_dir_rec_t r = mkrec(h, 8073 + i, (i % 2) ? b : a, (uint8_t)(i % 5), 4, (uint8_t)(20 + i));
        if (i == 3) r.flags = KIWI_DIR_F_OFFLINE;
        if (kiwi_dir_writer_add(w, &r) < 0) { free(w); return -1; }
    }
    rc = commit ? kiwi_dir_writer_finish(w) : 0;
    free(w);
    return rc;
}

/* ---------- Tests ---------- */
static void test_basics(void)
{
    CHECK(kiwi_dir_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(kiwi_dir_crc32(kiwi_dir_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);     /* verkettbar */

    char h[KIWI_DIR_HOST_LEN]; uint16_t p; uint8_t f;
    CHECK(kiwi_dir_parse_hostport("http://kiwi.example.org:8075", h, &p, &f) == 0 && !strcmp(h, "kiwi.example.org") && p == 8075 && f == 0);
    CHECK(kiwi_dir_parse_hostport("http://22204.proxy.kiwisdr.com", h, &p, &f) == 0 && p == 8073);   /* ohne Port: 8073 */
    CHECK(kiwi_dir_parse_hostport("http://h:8073/", h, &p, &f) == 0 && !strcmp(h, "h") && p == 8073);
    CHECK(kiwi_dir_parse_hostport("https://kiwi.770net.de:8073", h, &p, &f) == 0 && (f & KIWI_DIR_F_TLS));
    CHECK(kiwi_dir_parse_hostport("http://[::1]:8073", h, &p, &f) < 0);
    CHECK(kiwi_dir_parse_hostport("http://:8073", h, &p, &f) < 0);
    CHECK(kiwi_dir_parse_hostport("http://h:0", h, &p, &f) < 0 && kiwi_dir_parse_hostport("http://h:70000", h, &p, &f) < 0);
    CHECK(kiwi_dir_parse_hostport("http://a234567890123456789012345678901234567890123456:80", h, &p, &f) < 0);   /* Host zu lang */

    int16_t la, lo;
    CHECK(kiwi_dir_parse_gps("(-34.273700, 138.771000)", &la, &lo) == 0 && la == -3427 && lo == 13877);
    CHECK(kiwi_dir_parse_gps("(51.00, 13.65)", &la, &lo) == 0 && la == 5100 && lo == 1365);
    CHECK(kiwi_dir_parse_gps("(0.00, 0.00)", &la, &lo) < 0);          /* "unbekannt" */
    CHECK(kiwi_dir_parse_gps("(95.0, 10.0)", &la, &lo) < 0);          /* Breite ausserhalb */
    CHECK(kiwi_dir_parse_gps("Dortmund, Germany", &la, &lo) < 0);

    char u[8];
    kiwi_dir_copy_utf8(u, sizeof u, "abcdef\xC3\xA4g");               /* "abcdefä" + g: Schnitt mitten im ä */
    CHECK(!strcmp(u, "abcdef"));
    kiwi_dir_copy_utf8(u, sizeof u, "ab\xC3\xA4" "cd");
    CHECK(!strcmp(u, "ab\xC3\xA4" "cd"));

    kiwi_dir_url_t ur;
    CHECK(kiwi_dir_url_split("http://rx.linkfanel.net/kiwisdr_com.js", &ur) == 0 && !strcmp(ur.host, "rx.linkfanel.net") &&
          ur.port == 80 && !strcmp(ur.path, "/kiwisdr_com.js") && !ur.tls);
    CHECK(kiwi_dir_url_split("http://h:8080", &ur) == 0 && ur.port == 8080 && !strcmp(ur.path, "/"));
    CHECK(kiwi_dir_url_split("https://h/x", &ur) == 0 && ur.tls);
    CHECK(kiwi_dir_url_split("ftp://h/x", &ur) < 0);
}

typedef struct { uint8_t buf[256]; size_t n; } sink_t;
static int sink_cb(void *u, const uint8_t *d, size_t n) { sink_t *s = u; if (s->n + n > sizeof s->buf) return 1; memcpy(s->buf + s->n, d, n); s->n += n; return 0; }

static void test_chunk(void)
{
    const char *in = "5\r\nHello\r\n7;ext=1\r\n, World\r\nA\r\n0123456789\r\n0\r\nTrailer: x\r\n\r\n";
    for (int step = 1; step <= 5; step += 4) {                         /* 1 Byte und 5 Byte pro Aufruf */
        kiwi_dir_chunk_t c; sink_t s = { {0}, 0 }; int rc = 0;
        kiwi_dir_chunk_init(&c);
        for (size_t i = 0; i < strlen(in) && rc == 0; i += (size_t)step)
            rc = kiwi_dir_chunk_push(&c, (const uint8_t *)in + i, strlen(in) - i < (size_t)step ? strlen(in) - i : (size_t)step, sink_cb, &s);
        CHECK(rc == 1);
        CHECK(s.n == 22 && !memcmp(s.buf, "Hello, World0123456789", 22));
    }
    kiwi_dir_chunk_t c; sink_t s = { {0}, 0 };
    kiwi_dir_chunk_init(&c);
    CHECK(kiwi_dir_chunk_push(&c, (const uint8_t *)"zz\r\n", 4, sink_cb, &s) < 0);   /* kein Hex */
}

/* ---------- Parser ---------- */
typedef struct { kiwi_dir_item_t it[8]; int n; } coll_t;
static int coll_cb(void *u, const kiwi_dir_item_t *it) { coll_t *c = u; if (c->n < 8) c->it[c->n++] = *it; return 0; }

static void test_parser(void)
{
    static const char js[] =
        "// Kommentar mit [eckiger Klammer]\n"
        "var kiwisdr_com =\n[\n"
        "{ \"status\":\"active\", \"offline\":\"no\", \"name\":\"Caf\\u00e9 SDR \\ud83d\\ude00 | Dortmund, Germany\", \"bands\":\"0-30000000\",\n"
        "  \"users\":\"2\", \"users_max\":\"8\", \"gps\":\"(51.514400, 7.465300)\", \"loc\":\"Dortmund, Germany\",\n"
        "  \"antenna\":\"Vertical \\\"J-Pole\\\" \\\\ 5m\", \"snr\":\"40,41\", \"nested\":{\"a\":[1,2,{\"b\":\"}\"}],\"c\":\"]\"},\n"
        "  \"url\":\"http://dl.example.org:8074\" },\n"
        "{ \"name\":\"nur Zahl\", \"users\":3, \"users_max\":4, \"snr\":\"27\", \"offline\":\"yes\", \"status\":\"offline\",\n"
        "  \"gps\":\"(0.00, 0.00)\", \"loc\":\"Peru, IL\", \"url\":\"http://proxy.kiwisdr.com\", },\n"
        "{ \"name\":\"ohne URL\", \"users\":\"1\" },\n"
        "{ \"name\":\"https\", \"url\":\"https://secure.example.org:8073\", \"gps\":\"(40.0, -105.0)\", \"loc\":\"Boulder, CO\" , \"status\":\"private\"},\n"
        "{ \"name\":\"Mexico-Test\", \"url\":\"http://nm.example:8073\", \"gps\":\"(35.1, -106.6)\", \"loc\":\"Albuquerque, New Mexico\" },\n"
        "]\n;\n";
    for (int step = 1; step <= 3; step += 2) {                          /* Byte-fuer-Byte und 3er-Stuecke */
        kiwi_dir_parser_t *p = malloc(sizeof *p);
        coll_t c; memset(&c, 0, sizeof c);
        kiwi_dir_parser_init(p, coll_cb, &c);
        size_t n = strlen(js);
        for (size_t i = 0; i < n; i += (size_t)step)
            CHECK(kiwi_dir_parser_push(p, (const uint8_t *)js + i, n - i < (size_t)step ? n - i : (size_t)step) == 0);
        CHECK(kiwi_dir_parser_finished(p));
        CHECK(p->stats.objects == 5 && p->stats.accepted == 4 && p->stats.skipped == 1);
        CHECK(c.n == 4);
        if (c.n == 4) {
            const kiwi_dir_rec_t *r = &c.it[0].rec;
            CHECK(!strcmp(r->host, "dl.example.org") && r->port == 8074);
            CHECK(!strncmp(r->name, "Caf\xC3\xA9 SDR \xF0\x9F\x98\x80", 14));      /* é + Surrogatpaar */
            CHECK(strstr(r->name, "Dortmund") == NULL);                          /* Ort am Ende abgeschnitten */
            CHECK(!strcmp(r->place, "Dortmund, Germany") && !strcmp(c.it[0].country, "Germany"));
            CHECK(r->users == 2 && r->users_max == 8 && r->snr_all == 40 && r->snr_hf == 41);
            CHECK(r->band_lo_hz == 0 && r->band_hi_hz == 30000000u && r->lat_cdeg == 5151 && r->lon_cdeg == 747);
            CHECK(r->flags == 0);
            r = &c.it[1].rec;
            CHECK(!strcmp(r->host, "proxy.kiwisdr.com") && r->port == 8073);        /* Standardport */
            CHECK(r->users == 3 && r->users_max == 4 && r->snr_all == 27 && r->snr_hf == 0);
            CHECK(r->flags & KIWI_DIR_F_OFFLINE);
            CHECK(r->lat_cdeg == KIWI_DIR_NO_GEO);
            CHECK(!strcmp(c.it[1].country, "Peru"));                             /* ohne Koordinaten gewinnt der Text (nicht pruefbar) */
            CHECK((c.it[2].rec.flags & KIWI_DIR_F_TLS) && (c.it[2].rec.flags & KIWI_DIR_F_PRIVATE));
            CHECK(!strcmp(c.it[2].country, "USA"));                              /* nur ueber die Koordinaten */
            CHECK(!strcmp(c.it[3].country, "USA"));                              /* "New Mexico" ist nicht Mexico */
        }
        free(p);
    }
    /* Abgeschnittene Liste: nicht "finished" */
    kiwi_dir_parser_t *p = malloc(sizeof *p);
    coll_t c; memset(&c, 0, sizeof c);
    kiwi_dir_parser_init(p, coll_cb, &c);
    kiwi_dir_parser_push(p, (const uint8_t *)js, strlen(js) - 30);
    CHECK(!kiwi_dir_parser_finished(p));
    free(p);
}

static void test_geo(void)
{
    CHECK(!strcmp(kiwi_dir_geo_lookup(5252, 1340), "Germany"));                  /* Berlin */
    CHECK(!strcmp(kiwi_dir_geo_lookup(-3427, 13877), "Australia"));
    CHECK(!strcmp(kiwi_dir_geo_lookup(4000, -10500), "USA"));
    CHECK(kiwi_dir_geo_lookup(0, -3000) == NULL);                                /* Atlantik */
    CHECK(!strcmp(kiwi_dir_geo_from_text("Papua New Guinea"), "Papua New Guinea"));   /* laengster Treffer */
    CHECK(!strcmp(kiwi_dir_geo_from_text("Lagos, Nigeria"), "Nigeria"));         /* nicht "Niger" */
    CHECK(!strcmp(kiwi_dir_geo_from_text("FRANCE"), "France"));
    CHECK(!strcmp(kiwi_dir_geo_from_text("Oregon USA"), "USA"));
    CHECK(!strcmp(kiwi_dir_geo_from_text("Chichester UK"), "UK"));
    CHECK(!strcmp(kiwi_dir_geo_from_text("Baden, Deutschland"), "Germany"));
    CHECK(kiwi_dir_geo_from_text("Stevensville, MT") == NULL);
    /* Widerspruch Text/Koordinaten */
    CHECK(!strcmp(kiwi_dir_geo_resolve("Holland, MA", "", 1, 4203, -7214), "USA"));       /* Ortsname, Koordinaten gewinnen */
    CHECK(!strcmp(kiwi_dir_geo_resolve("Peru, IL", "", 1, 4133, -8913), "USA"));         /* Ortsname "Peru" in Illinois */
    CHECK(!strcmp(kiwi_dir_geo_resolve("Freital, Germany", "", 1, 5100, 1368), "Germany")); /* Raster sagt Tschechien (Grenznaehe) */
    CHECK(!strcmp(kiwi_dir_geo_resolve("Singapore", "", 1, 135, 10385), "Singapore"));     /* Land nicht im Raster */
    CHECK(!strcmp(kiwi_dir_geo_resolve("", "", 1, 4800, 1100), "Germany"));
    CHECK(!strcmp(kiwi_dir_geo_resolve("Madeira", "", 0, 0, 0), "Portugal"));
}

/* ---------- Datenbank ---------- */
static void test_db(void)
{
    static kiwi_dir_db_t db;
    kiwi_dir_rec_t r;
    uint32_t idx, cur;
    fresh();
    CHECK(kiwi_dir_db_open(&db, &g_store, 1) == KIWI_DIR_ERR_EMPTY && !kiwi_dir_db_valid(&db) && kiwi_dir_db_count(&db) == 0);
    CHECK(kiwi_dir_db_get(&db, 0, &r) == KIWI_DIR_ERR_EMPTY);
    CHECK(kiwi_dir_capacity(&g_store) == (SLOT - KIWI_DIR_OFF_RECORDS) / 120);

    /* Erstes Schreiben -> Slot 0, Generation 1 */
    CHECK(write_db(&db, 10, 1) == 0 && g_write_twice == 0);
    CHECK(kiwi_dir_db_open(&db, &g_store, 1) == 0);
    CHECK(kiwi_dir_db_active_slot(&db) == 0 && kiwi_dir_db_generation(&db) == 1 && kiwi_dir_db_count(&db) == 10);
    CHECK(kiwi_dir_db_get(&db, 9, &r) == 0 && !strcmp(r.host, "h9.example") && r.port == 8082);
    CHECK(kiwi_dir_db_get(&db, 10, &r) == KIWI_DIR_ERR_RANGE);
    char cn[KIWI_DIR_COUNTRY_LEN];
    CHECK(kiwi_dir_db_country_count(&db) == 2 && kiwi_dir_db_country_name(&db, 2, cn) == 0 && !strcmp(cn, "Bland"));
    uint8_t id;
    CHECK(kiwi_dir_db_country_find(&db, "bLAND", &id) == 0 && id == 2);                   /* Gross/Klein egal */
    CHECK(kiwi_dir_db_country_find(&db, "Cland", &id) != 0);
    CHECK(kiwi_dir_db_lookup(&db, "H5.EXAMPLE", 8078, &idx) == 0 && idx == 5);
    CHECK(kiwi_dir_db_lookup(&db, "h5.example", 1, &idx) != 0);

    /* Filter */
    kiwi_dir_query_t q;
    kiwi_dir_query_init(&q);
    CHECK(kiwi_dir_db_count_matches(&db, &q) == 9);                                         /* Server 3 ist offline */
    q.country = 2;                                                                          /* Bland: i = 1,3,5,7,9 -> ohne 3 */
    CHECK(kiwi_dir_db_count_matches(&db, &q) == 4);
    q.only_free = 1;                                                                        /* users = i % 5 von 4: i=9 (4/4) voll */
    CHECK(kiwi_dir_db_count_matches(&db, &q) == 3);
    q.min_snr = 27;                                                                         /* snr = 20 + i: i >= 7 */
    CHECK(kiwi_dir_db_count_matches(&db, &q) == 1);
    kiwi_dir_query_init(&q);
    strcpy(q.text, "H7.");
    cur = 0;
    CHECK(kiwi_dir_db_find(&db, &q, &cur, &r, &idx) == 1 && idx == 7 && cur == 8);
    CHECK(kiwi_dir_db_find(&db, &q, &cur, &r, &idx) == 0);
    kiwi_dir_query_init(&q);
    strcpy(q.text, "bland");                                                                /* trifft ueber den Landesnamen */
    CHECK(kiwi_dir_db_count_matches(&db, &q) == 4);
    kiwi_dir_query_init(&q);
    CHECK(kiwi_dir_db_best(&db, &q, &r, &idx) == 1 && idx == 9);                            /* hoechster SNR */
    uint16_t cnt[256];
    CHECK(kiwi_dir_db_count_by_country(&db, cnt) == 0 && cnt[1] == 5 && cnt[2] == 4);

    /* Abgebrochenes Update (kein Commit): alte DB bleibt aktiv */
    CHECK(write_db(&db, 50, 0) == 0);
    CHECK(kiwi_dir_db_open(&db, &g_store, 1) == 0 && kiwi_dir_db_active_slot(&db) == 0 && kiwi_dir_db_count(&db) == 10);

    /* Zweites Update -> Slot 1, Generation 2 */
    CHECK(write_db(&db, 20, 1) == 0 && g_write_twice == 0);
    CHECK(kiwi_dir_db_open(&db, &g_store, 1) == 0);
    CHECK(kiwi_dir_db_active_slot(&db) == 1 && kiwi_dir_db_generation(&db) == 2 && kiwi_dir_db_count(&db) == 20);

    /* Beschaedigung im aktiven Slot -> Pruefsumme schlaegt an -> Rueckfall auf Slot 0 (alt) */
    g_mem[1][KIWI_DIR_OFF_RECORDS + 5 * 120 + 3] ^= 0x01;
    CHECK(kiwi_dir_db_open(&db, &g_store, 1) == 0 && kiwi_dir_db_active_slot(&db) == 0 && kiwi_dir_db_count(&db) == 10);
    CHECK(kiwi_dir_db_open(&db, &g_store, 0) == 0 && kiwi_dir_db_active_slot(&db) == 1);   /* ohne verify faellt es nicht auf */
    /* Beschaedigter Header */
    g_mem[0][5] ^= 0xFF; g_mem[1][5] ^= 0xFF;
    CHECK(kiwi_dir_db_open(&db, &g_store, 0) == KIWI_DIR_ERR_EMPTY);

    /* Kapazitaet: mehr Saetze als Platz -> Rest wird gezaehlt, nicht gespeichert */
    fresh();
    kiwi_dir_db_open(&db, &g_store, 1);
    kiwi_dir_writer_t *w = malloc(sizeof *w);
    CHECK(kiwi_dir_writer_begin(w, &g_store, 0, 1) == 0);
    uint32_t cap = kiwi_dir_capacity(&g_store);
    for (uint32_t i = 0; i < cap + 7; i++) { kiwi_dir_rec_t x = mkrec("x", 1, 0, 0, 0, 0); kiwi_dir_writer_add(w, &x); }
    CHECK(w->count == cap && w->overflow == 7 && kiwi_dir_writer_finish(w) == 0);
    free(w);
    CHECK(kiwi_dir_db_open(&db, &g_store, 1) == 0 && kiwi_dir_db_count(&db) == cap);
}

/* ---------- Update ---------- */
static const char *mklist(int n, char *buf, size_t cap)
{
    size_t o = (size_t)snprintf(buf, cap, "var kiwisdr_com =\n[\n");
    for (int i = 0; i < n; i++)
        o += (size_t)snprintf(buf + o, cap - o, "{\"name\":\"S%d\",\"url\":\"http://u%d.example:8073\",\"gps\":\"(52.5, 13.4)\",\"users\":\"0\",\"users_max\":\"4\"},\n", i, i);
    snprintf(buf + o, cap - o, "]\n;\n");
    return buf;
}

static int update_with(kiwi_dir_db_t *db, const char *text, kiwi_dir_result_t *res)
{
    kiwi_dir_update_t *u = malloc(sizeof *u);
    int rc = kiwi_dir_update_begin(u, &g_store, db);
    if (rc) { free(u); return rc; }
    size_t n = strlen(text);
    for (size_t i = 0; i < n && rc == 0; i += 1000) rc = kiwi_dir_update_push(u, (const uint8_t *)text + i, n - i < 1000 ? n - i : 1000);
    if (rc) { kiwi_dir_update_abort(u); free(u); return rc; }
    rc = kiwi_dir_update_end(u, res);
    free(u);
    return rc;
}

static void test_update(void)
{
    static kiwi_dir_db_t db;
    static char buf[20000];
    kiwi_dir_result_t res;
    fresh();
    kiwi_dir_db_open(&db, &g_store, 1);

    CHECK(update_with(&db, mklist(60, buf, sizeof buf), &res) == 0);
    CHECK(res.entries == 60 && res.old_entries == 0 && res.generation == 1 && kiwi_dir_db_count(&db) == 60);
    CHECK(res.countries == 1 && g_write_twice == 0);

    /* Zu kleine neue Liste (< 1/4 der alten) wird abgelehnt, alte bleibt */
    CHECK(update_with(&db, mklist(5, buf, sizeof buf), &res) == KIWI_DIR_ERR_SUSPECT);
    CHECK(kiwi_dir_db_count(&db) == 60 && kiwi_dir_db_generation(&db) == 1);
    /* Abgeschnittene Liste */
    mklist(30, buf, sizeof buf);
    buf[strlen(buf) - 8] = '\0';
    CHECK(update_with(&db, buf, &res) == KIWI_DIR_ERR_PARSE && kiwi_dir_db_count(&db) == 60);
    /* Fehlerseite statt Liste: Flash bleibt unberuehrt (kein Loeschen) */
    uint8_t before[SLOT]; memcpy(before, g_mem[1], SLOT);
    CHECK(update_with(&db, "<html>404 Not Found</html>", &res) == KIWI_DIR_ERR_PARSE);
    CHECK(!memcmp(before, g_mem[1], SLOT) && kiwi_dir_db_count(&db) == 60);
    /* Gueltiges zweites Update */
    CHECK(update_with(&db, mklist(70, buf, sizeof buf), &res) == 0 && kiwi_dir_db_active_slot(&db) == 1 && res.generation == 2);

    /* Nur ein Update gleichzeitig */
    kiwi_dir_update_t *a = malloc(sizeof *a), *b = malloc(sizeof *b);
    CHECK(kiwi_dir_update_begin(a, &g_store, &db) == 0);
    CHECK(kiwi_dir_update_begin(b, &g_store, &db) == KIWI_DIR_ERR_BUSY);
    kiwi_dir_update_abort(a);
    CHECK(kiwi_dir_update_begin(b, &g_store, &db) == 0);
    kiwi_dir_update_abort(b);
    free(a); free(b);
}

/* ---------- Befehle ---------- */
typedef struct { char lines[40][200]; int n; int updates, force, selects; kiwi_dir_rec_t sel; } cmdenv_t;
static void c_out(void *u, const char *l) { cmdenv_t *e = u; if (e->n < 40) snprintf(e->lines[e->n++], 200, "%s", l); }
static int c_upd(void *u, int force) { cmdenv_t *e = u; e->updates++; e->force = force; return 0; }
static int c_sel(void *u, const kiwi_dir_rec_t *r) { cmdenv_t *e = u; e->selects++; e->sel = *r; return 0; }
static int has(cmdenv_t *e, const char *s) { for (int i = 0; i < e->n; i++) if (strstr(e->lines[i], s)) return 1; return 0; }

static void test_cmd(void)
{
    static kiwi_dir_db_t db;
    static cmdenv_t e;
    kiwi_dir_cmd_t c;
    fresh();
    kiwi_dir_db_open(&db, &g_store, 1);
    kiwi_dir_cmd_init(&c, &db, c_out, c_upd, c_sel, &e);

    CHECK(kiwi_dir_command(&c, "f 225") == 0 && kiwi_dir_command(&c, "dx") == 0 && kiwi_dir_command(&c, "q") == 0);   /* nicht unsere */
    CHECK(kiwi_dir_command(&c, "d list") == 1 && has(&e, "d update"));                    /* leere DB -> Hinweis */
    e.n = 0;
    CHECK(kiwi_dir_command(&c, "d update force") == 1 && e.updates == 1 && e.force == 1);
    CHECK(kiwi_dir_command(&c, "d") == 1 && has(&e, "Serverauswahl"));

    write_db(&db, 12, 1);
    kiwi_dir_db_open(&db, &g_store, 1);
    e.n = 0;
    kiwi_dir_command(&c, "d info");      CHECK(has(&e, "12 Server") && has(&e, "11 Server"));   /* 12, davon 11 nutzbar */
    e.n = 0;
    kiwi_dir_command(&c, "d countries"); CHECK(has(&e, "Aland(6)") && has(&e, "Bland(5)"));
    e.n = 0;
    kiwi_dir_command(&c, "d filter bla"); CHECK(has(&e, "Land=Bland") && has(&e, "5 Server"));    /* eindeutiger Anfang */
    e.n = 0;
    kiwi_dir_command(&c, "d filter land"); CHECK(has(&e, "Text='land'") && has(&e, "11 Server"));   /* mehrdeutig -> Text */
    e.n = 0;
    kiwi_dir_command(&c, "d filter Aland"); kiwi_dir_command(&c, "d list");
    CHECK(has(&e, "Seite 1/1") && has(&e, "h0.example:8073") && has(&e, "#10"));
    e.n = 0;
    kiwi_dir_command(&c, "d go 4");
    CHECK(e.selects == 1 && !strcmp(e.sel.host, "h4.example") && e.sel.port == 8077);
    e.n = 0;
    kiwi_dir_command(&c, "d go 999"); CHECK(e.selects == 1 && has(&e, "gibt es nicht"));
    kiwi_dir_command(&c, "d go best"); CHECK(e.selects == 2 && !strcmp(e.sel.host, "h10.example"));   /* bester SNR in Aland mit freiem Kanal */
    kiwi_dir_command(&c, "d go other.host:8080");
    CHECK(e.selects == 3 && !strcmp(e.sel.host, "other.host") && e.sel.port == 8080);
    kiwi_dir_command(&c, "d go other.host");
    CHECK(e.selects == 4 && e.sel.port == 8073);
    kiwi_dir_command(&c, "d go 3.example.net");                                        /* Hostname mit Ziffer am Anfang */
    CHECK(e.selects == 5 && !strcmp(e.sel.host, "3.example.net"));
    e.n = 0;
    kiwi_dir_command(&c, "d filter"); CHECK(c.q.country == 0 && !c.q.text[0]);
    kiwi_dir_command(&c, "d freq 7055"); CHECK(c.q.freq_hz == 7055000);
    kiwi_dir_command(&c, "d freq off");  CHECK(c.q.freq_hz == 0);
    kiwi_dir_command(&c, "d snr 25");    CHECK(c.q.min_snr == 25);
    kiwi_dir_command(&c, "d free");      CHECK(c.q.only_free == 1);
    kiwi_dir_command(&c, "d free off");  CHECK(c.q.only_free == 0);
}

int main(void)
{
    test_basics();
    test_chunk();
    test_parser();
    test_geo();
    test_db();
    test_update();
    test_cmd();
    printf(g_fail ? "\n%d FEHLER\n" : "\nALLE DIR-TESTS BESTANDEN\n", g_fail);
    return g_fail ? 1 : 0;
}
