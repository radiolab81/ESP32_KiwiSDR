/**
 * @file kiwi_dir_cmd.c
 * @brief Befehlsinterpreter fuer die Serverauswahl. Syntax: kiwi_dir_cmd.h.
 */
#include "kiwi_dir_cmd.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiwi_client.h"      /* kiwi_parse_khz() */
#include "kiwi_dir_parse.h"   /* kiwi_dir_parse_hostport(), kiwi_dir_copy_utf8() */

#define PAGE_SIZE 10

static void say(kiwi_dir_cmd_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(kiwi_dir_cmd_t *c, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (c->out) c->out(c->user, line);
}

void kiwi_dir_cmd_init(kiwi_dir_cmd_t *c, kiwi_dir_db_t *db, void (*out)(void *, const char *),
                       int (*update)(void *, int), int (*select)(void *, const kiwi_dir_rec_t *), void *user)
{
    memset(c, 0, sizeof *c);
    c->db = db;
    c->out = out;
    c->update = update;
    c->select = select;
    c->user = user;
    kiwi_dir_query_init(&c->q);
}

static int ci_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    return 1;
}

/** Beschreibung des Filters als Text (fuer info/list). */
static void describe(kiwi_dir_cmd_t *c, char *buf, size_t cap)
{
    char cn[KIWI_DIR_COUNTRY_LEN] = "";
    size_t n = 0;
    buf[0] = '\0';
    if (c->q.country) kiwi_dir_db_country_name(c->db, c->q.country, cn);
    if (cn[0])            n += (size_t)snprintf(buf + n, cap - n, "Land=%s ", cn);
    if (c->q.text[0])     n += (size_t)snprintf(buf + n, cap - n, "Text='%s' ", c->q.text);
    if (c->q.only_free)   n += (size_t)snprintf(buf + n, cap - n, "nur-frei ");
    if (c->q.min_snr)     n += (size_t)snprintf(buf + n, cap - n, "SNR>=%u ", c->q.min_snr);
    if (c->q.freq_hz)     n += (size_t)snprintf(buf + n, cap - n, "Freq=%lu.%03lu kHz ",
                                                (unsigned long)(c->q.freq_hz / 1000), (unsigned long)(c->q.freq_hz % 1000));
    if (!n) snprintf(buf, cap, "(kein Filter)");
}

static void show_matches(kiwi_dir_cmd_t *c)
{
    char d[100];
    describe(c, d, sizeof d);
    say(c, "Filter: %s -> %lu Server", d, (unsigned long)kiwi_dir_db_count_matches(c->db, &c->q));
}

static int need_db(kiwi_dir_cmd_t *c)
{
    if (kiwi_dir_db_valid(c->db)) return 1;
    say(c, "Keine Serverdatenbank. Erst 'd update' ausfuehren.");
    return 0;
}

/** Teilstring ohne Beachtung der Gross-/Kleinschreibung. */
static int ci_has(const char *hay, const char *needle)
{
    for (; *hay; hay++) if (ci_prefix(hay, needle)) return 1;
    return 0;
}

static void print_rec(kiwi_dir_cmd_t *c, uint32_t idx, const kiwi_dir_rec_t *r)
{
    char cn[KIWI_DIR_COUNTRY_LEN] = "";
    if (r->country) kiwi_dir_db_country_name(c->db, r->country, cn);
    /* Das Land nur anhaengen, wenn es nicht schon im Ortstext steht ("Hamburg, Germany"). */
    int add = cn[0] && !ci_has(r->place, cn);
    say(c, "#%-4lu %s | %s%s%s | %s:%u | %u/%u Nutzer | SNR %u dB",
        (unsigned long)idx, r->name, r->place, add ? ", " : "", add ? cn : "", r->host, (unsigned)r->port,
        (unsigned)r->users, (unsigned)r->users_max, (unsigned)kiwi_dir_rec_snr(r));
}

/* ---- Unterbefehle ---- */

static void cmd_info(kiwi_dir_cmd_t *c)
{
    if (!need_db(c)) return;
    say(c, "Datenbank: %lu Server, %u Laender, Generation %lu, Slot %c, Kapazitaet %lu",
        (unsigned long)kiwi_dir_db_count(c->db), kiwi_dir_db_country_count(c->db),
        (unsigned long)kiwi_dir_db_generation(c->db), 'A' + kiwi_dir_db_active_slot(c->db),
        (unsigned long)kiwi_dir_capacity(c->db->st));
    show_matches(c);
}

static void cmd_countries(kiwi_dir_cmd_t *c)
{
    uint16_t cnt[256];
    if (!need_db(c)) return;
    if (kiwi_dir_db_count_by_country(c->db, cnt)) { say(c, "Lesefehler"); return; }
    char line[120] = "";
    size_t n = 0;
    for (;;) {                                   /* absteigend nach Anzahl (einfache Auswahl, <= 255 Eintraege) */
        unsigned best = 0;
        for (unsigned i = 1; i < 256; i++) if (cnt[i] > cnt[best] || (best == 0 && cnt[i])) best = i;
        if (best == 0 || cnt[best] == 0) break;
        char nm[KIWI_DIR_COUNTRY_LEN];
        kiwi_dir_db_country_name(c->db, best, nm);
        char item[48];
        int il = snprintf(item, sizeof item, "%s(%u)  ", nm, (unsigned)cnt[best]);
        if (n + (size_t)il >= 95) { say(c, "%s", line); n = 0; line[0] = '\0'; }
        memcpy(line + n, item, (size_t)il + 1);
        n += (size_t)il;
        cnt[best] = 0;
    }
    if (n) say(c, "%s", line);
    if (cnt[0]) say(c, "ohne Land: %u", (unsigned)cnt[0]);
}

static void cmd_filter(kiwi_dir_cmd_t *c, const char *arg)
{
    if (!need_db(c)) return;
    if (!*arg) { kiwi_dir_query_init(&c->q); show_matches(c); return; }
    c->q.country = 0;
    c->q.text[0] = '\0';
    uint8_t id;
    if (kiwi_dir_db_country_find(c->db, arg, &id) == 0) c->q.country = id;
    else {
        /* Anfang eines Landesnamens ("Ger" -> Germany), aber nur wenn eindeutig. */
        unsigned hits = 0, last = 0;
        for (unsigned i = 1; i <= kiwi_dir_db_country_count(c->db); i++) {
            char nm[KIWI_DIR_COUNTRY_LEN];
            if (kiwi_dir_db_country_name(c->db, i, nm) == 0 && ci_prefix(nm, arg)) { hits++; last = i; }
        }
        if (hits == 1) c->q.country = (uint8_t)last;
        else snprintf(c->q.text, sizeof c->q.text, "%s", arg);
    }
    show_matches(c);
}

static void cmd_list(kiwi_dir_cmd_t *c, const char *arg)
{
    if (!need_db(c)) return;
    uint32_t total = kiwi_dir_db_count_matches(c->db, &c->q);
    uint32_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t page = *arg ? (uint32_t)strtoul(arg, NULL, 10) : 1;
    if (page < 1) page = 1;
    if (!total) { show_matches(c); return; }
    if (page > pages) page = pages;

    say(c, "Seite %lu/%lu (%lu Treffer)", (unsigned long)page, (unsigned long)pages, (unsigned long)total);
    kiwi_dir_rec_t r;
    uint32_t cur = 0, idx, seen = 0;
    while (kiwi_dir_db_find(c->db, &c->q, &cur, &r, &idx) == 1) {
        if (seen >= (page - 1) * PAGE_SIZE && seen < page * PAGE_SIZE) print_rec(c, idx, &r);
        if (++seen >= page * PAGE_SIZE) break;
    }
    if (page < pages) say(c, "weiter mit: d list %lu   |   verbinden: d go <Nr>", (unsigned long)(page + 1));
    else say(c, "verbinden: d go <Nr>");
}

/** 1, wenn s nur aus Ziffern besteht ("21472.proxy..." ist ein Hostname, keine Nummer). */
static int all_digits(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static void cmd_go(kiwi_dir_cmd_t *c, const char *arg)
{
    kiwi_dir_rec_t r;
    uint32_t idx = 0;
    memset(&r, 0, sizeof r);

    if (!*arg) { say(c, "d go <Nr> | best | host[:port]"); return; }
    if (!strcmp(arg, "best")) {
        if (!need_db(c)) return;
        kiwi_dir_query_t q = c->q;
        q.only_free = 1;                          /* "best" soll auch wirklich verbinden koennen */
        if (kiwi_dir_db_best(c->db, &q, &r, &idx) != 1) { say(c, "Kein passender Server mit freiem Kanal."); return; }
    } else if (all_digits(arg + (arg[0] == '#'))) {
        if (!need_db(c)) return;
        idx = (uint32_t)strtoul(arg + (arg[0] == '#'), NULL, 10);
        if (kiwi_dir_db_get(c->db, idx, &r)) { say(c, "Nr. %lu gibt es nicht.", (unsigned long)idx); return; }
    } else {
        /* Direkteingabe host[:port]; ist er in der DB, nehmen wir deren Daten. */
        char host[KIWI_DIR_HOST_LEN];
        uint16_t port;
        uint8_t fl;
        if (kiwi_dir_parse_hostport(arg, host, &port, &fl) != 0 || (fl & KIWI_DIR_F_TLS)) {
            say(c, "Ungueltig. Format: host[:port] (ohne Port gilt 8073)");
            return;
        }
        if (kiwi_dir_db_valid(c->db) && kiwi_dir_db_lookup(c->db, host, port, &idx) == 0) kiwi_dir_db_get(c->db, idx, &r);
        else { snprintf(r.host, sizeof r.host, "%s", host); r.port = port; kiwi_dir_copy_utf8(r.name, sizeof r.name, host); }
    }
    if (r.flags & KIWI_DIR_F_TLS) { say(c, "Dieser Server verlangt https (TLS) - nicht unterstuetzt."); return; }
    if (r.flags & (KIWI_DIR_F_OFFLINE | KIWI_DIR_F_PRIVATE)) say(c, "Hinweis: Server ist als offline/privat markiert.");
    say(c, "Wechsle zu %s (%s:%u) ...", r.name, r.host, (unsigned)r.port);
    if (c->select && c->select(c->user, &r) != 0) say(c, "Wechsel nicht moeglich.");
}

/* ---- Einstieg ---- */

int kiwi_dir_command(kiwi_dir_cmd_t *c, const char *line)
{
    char buf[80];
    size_t n = 0;
    while (*line == ' ') line++;
    if (tolower((unsigned char)line[0]) != 'd' || (line[1] != '\0' && !isspace((unsigned char)line[1]))) return 0;
    for (line++; line[n] && n < sizeof buf - 1; n++) buf[n] = line[n];
    buf[n] = '\0';
    while (n && isspace((unsigned char)buf[n - 1])) buf[--n] = '\0';

    char *cmd = buf, *arg;
    while (*cmd == ' ') cmd++;
    arg = cmd;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = '\0';
    while (*arg == ' ') arg++;

#define IS(a, b) (!strcmp(cmd, a) || !strcmp(cmd, b))
    if (!*cmd || IS("help", "h") || !strcmp(cmd, "?")) {
        say(c, "Serverauswahl: d update | d info | d countries | d filter <Land|Text> | d free [on|off]");
        say(c, "               d snr <dB> | d freq <kHz|off> | d list [Seite] | d go <Nr|best|host[:port]>");
    } else if (IS("update", "u")) {
        int force = !strcmp(arg, "force");
        if (c->update) c->update(c->user, force);
    } else if (IS("info", "i")) cmd_info(c);
    else if (IS("countries", "c")) cmd_countries(c);
    else if (IS("filter", "f")) cmd_filter(c, arg);
    else if (!strcmp(cmd, "free")) {
        c->q.only_free = strcmp(arg, "off") ? 1 : 0;
        show_matches(c);
    } else if (!strcmp(cmd, "snr")) {
        c->q.min_snr = (uint8_t)strtoul(arg, NULL, 10);
        show_matches(c);
    } else if (!strcmp(cmd, "freq")) {
        uint32_t hz = 0;
        if (strcmp(arg, "off") && strcmp(arg, "0") && kiwi_parse_khz(arg, &hz)) say(c, "Frequenz in kHz, z. B. 'd freq 7055' oder 'd freq off'");
        else { c->q.freq_hz = hz; show_matches(c); }
    } else if (IS("list", "l")) cmd_list(c, arg);
    else if (IS("go", "g")) cmd_go(c, arg);
    else say(c, "Unbekannter Befehl. 'd' zeigt die Hilfe.");
#undef IS
    return 1;
}
