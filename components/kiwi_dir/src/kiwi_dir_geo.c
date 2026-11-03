/**
 * @file kiwi_dir_geo.c
 * @brief Land aus Text und Koordinaten. Konzept: siehe kiwi_dir_geo.h.
 */
#include "kiwi_dir_geo.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Koordinaten -> Land (Rasterkarte)                                  */
/* ------------------------------------------------------------------ */

/** Land-ID (1..n, 0 = Meer) der Rasterzelle (Zeile, Spalte). */
static unsigned cell_id(unsigned row, unsigned col)
{
    unsigned i = kiwi_dir_geo_row_start[row], end = kiwi_dir_geo_row_start[row + 1], pos = 0;
    for (; i < end; i++) {
        pos += kiwi_dir_geo_runs[2 * i + 1];
        if (col < pos) return kiwi_dir_geo_runs[2 * i];
    }
    return 0;
}

/** Zelle zu Koordinaten (0,01 Grad). Spalte bricht um (Datumsgrenze), Zeile wird begrenzt. */
static void to_cell(int lat_cdeg, int lon_cdeg, unsigned *row, unsigned *col)
{
    int step = (int)kiwi_dir_geo_step_milli;
    int r = (90000 - lat_cdeg * 10) / step;
    int c = (lon_cdeg * 10 + 180000) / step;
    int rows = (int)kiwi_dir_geo_rows_n, cols = (int)kiwi_dir_geo_cols_n;
    if (r < 0) r = 0;
    if (r >= rows) r = rows - 1;
    c %= cols;
    if (c < 0) c += cols;
    *row = (unsigned)r;
    *col = (unsigned)c;
}

const char *kiwi_dir_geo_lookup(int lat_cdeg, int lon_cdeg)
{
    unsigned r, c;
    to_cell(lat_cdeg, lon_cdeg, &r, &c);
    unsigned id = cell_id(r, c);
    return (id && id <= kiwi_dir_geo_name_count) ? kiwi_dir_geo_names[id - 1] : NULL;
}

/** 1, wenn das Land in der 5x5-Umgebung (+-1 Grad) der Position vorkommt. */
static int country_nearby(const char *country, int lat_cdeg, int lon_cdeg)
{
    unsigned r0, c0;
    to_cell(lat_cdeg, lon_cdeg, &r0, &c0);
    int rows = (int)kiwi_dir_geo_rows_n, cols = (int)kiwi_dir_geo_cols_n;
    for (int dr = -2; dr <= 2; dr++) {
        int r = (int)r0 + dr;
        if (r < 0 || r >= rows) continue;
        for (int dc = -2; dc <= 2; dc++) {
            int c = ((int)c0 + dc + cols) % cols;
            unsigned id = cell_id((unsigned)r, (unsigned)c);
            if (id && id <= kiwi_dir_geo_name_count && !strcmp(kiwi_dir_geo_names[id - 1], country)) return 1;
        }
    }
    return 0;
}

/** 1, wenn das Land in der Rasterkarte ueberhaupt vorkommt (sonst nicht ueberpruefbar). */
static int in_grid(const char *country)
{
    for (unsigned i = 0; i < kiwi_dir_geo_name_count; i++)
        if (!strcmp(kiwi_dir_geo_names[i], country)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Text -> Land                                                       */
/* ------------------------------------------------------------------ */

/**
 * Zusaetzliche Suchbegriffe. Links: so wie im Text gesucht wird, rechts: der
 * kanonische Name (derselbe String, der in der Datenbank landet). Ergaenzt die
 * Namen aus der Rasterkarte um
 *   - Schreibvarianten/Kurzformen (UK, England, Holland, Deutschland ...),
 *   - Laender, die in der groben Rasterkarte fehlen (Singapore, Malta, Monaco ...).
 * Erweitern ist gefahrlos: einfach eine Zeile anhaengen.
 * NICHT aufgenommen: "US", "America", "Georgia" als US-Staat u. a. - zu mehrdeutig.
 */
static const struct { const char *pat, *country; } k_alias[] = {
    { "UK", "UK" }, { "U.K.", "UK" }, { "Great Britain", "UK" }, { "Britain", "UK" },
    { "England", "UK" }, { "Scotland", "UK" }, { "Wales", "UK" }, { "Northern Ireland", "UK" },
    { "USA", "USA" }, { "U.S.A.", "USA" }, { "United States", "USA" }, { "United States of America", "USA" },
    { "Holland", "Netherlands" }, { "Nederland", "Netherlands" },
    { "Deutschland", "Germany" }, { "Allemagne", "Germany" }, { "Germania", "Germany" },
    { "Italia", "Italy" }, { "Espana", "Spain" }, { "Polska", "Poland" }, { "Schweiz", "Switzerland" },
    { "Suisse", "Switzerland" }, { "Svizzera", "Switzerland" }, { "Osterreich", "Austria" },
    { "Belgique", "Belgium" }, { "Sverige", "Sweden" }, { "Norge", "Norway" }, { "Suomi", "Finland" },
    { "Danmark", "Denmark" }, { "Brasil", "Brazil" }, { "Nippon", "Japan" },
    { "Czechia", "Czech Republic" }, { "Russian Federation", "Russia" },
    { "Republic of Korea", "South Korea" }, { "Korea Republic", "South Korea" },
    { "Macedonia", "North Macedonia" }, { "Bosnia", "Bosnia and Herzegovina" },
    { "Burma", "Myanmar" }, { "Turkiye", "Turkey" }, { "Swaziland", "Eswatini" },
    { "Cote d'Ivoire", "Ivory Coast" }, { "Congo-Kinshasa", "DR Congo" },
    { "Aotearoa", "New Zealand" }, { "Azores", "Portugal" }, { "Madeira", "Portugal" },
    { "Canary Islands", "Spain" }, { "Canarias", "Spain" }, { "Mallorca", "Spain" }, { "Tenerife", "Spain" }, { "The Gambia", "Gambia" },
    /* in der Rasterkarte nicht enthalten (zu klein fuer 1:110m): nur ueber den Text */
    { "Singapore", "Singapore" }, { "Malta", "Malta" }, { "Hong Kong", "Hong Kong" },
    { "Macau", "Macau" }, { "Liechtenstein", "Liechtenstein" }, { "Monaco", "Monaco" },
    { "Andorra", "Andorra" }, { "San Marino", "San Marino" }, { "Bahrain", "Bahrain" },
    { "Mauritius", "Mauritius" }, { "Maldives", "Maldives" }, { "Seychelles", "Seychelles" },
    { "Cape Verde", "Cape Verde" }, { "Barbados", "Barbados" }, { "Bermuda", "Bermuda" },
    { "Cayman Islands", "Cayman Islands" }, { "Faroe Islands", "Faroe Islands" },
    { "Isle of Man", "Isle of Man" }, { "Gibraltar", "Gibraltar" }, { "French Polynesia", "French Polynesia" },
    { "Reunion", "Reunion" }, { "Guadeloupe", "Guadeloupe" }, { "Martinique", "Martinique" },
    { "Samoa", "Samoa" }, { "Tonga", "Tonga" }, { "Guam", "Guam" }, { "Svalbard", "Norway" },
};

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/** Buchstabe/Ziffer? Bytes >= 0x80 (UTF-8) zaehlen als Buchstaben -> kein Wortende. */
static int is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80;
}

/** Laenge von @p pat, wenn es als GANZES WORT in @p text steht, sonst 0. */
static size_t find_word(const char *text, const char *pat)
{
    size_t pl = strlen(pat);
    for (const char *t = text; *t; t++) {
        if (lc(*t) != lc(*pat)) continue;
        size_t i = 1;
        while (i < pl && t[i] && lc(t[i]) == lc(pat[i])) i++;
        if (i != pl) continue;
        if (t > text && is_word(t[-1])) continue;      /* davor Buchstabe: "Nigeria" != "Niger" */
        if (is_word(t[pl])) continue;                  /* dahinter Buchstabe */
        return pl;
    }
    return 0;
}

const char *kiwi_dir_geo_from_text(const char *text)
{
    const char *best = NULL;
    size_t blen = 0;
    if (!text || !*text) return NULL;
    for (unsigned i = 0; i < kiwi_dir_geo_name_count; i++) {
        size_t l = find_word(text, kiwi_dir_geo_names[i]);
        if (l > blen) { blen = l; best = kiwi_dir_geo_names[i]; }
    }
    for (unsigned i = 0; i < sizeof k_alias / sizeof k_alias[0]; i++) {
        size_t l = find_word(text, k_alias[i].pat);
        if (l > blen) { blen = l; best = k_alias[i].country; }
    }
    /* "New Mexico" ist kein Land: das Wort "Mexico" zaehlt nicht, wenn "New " davor steht. */
    if (best && !strcmp(best, "Mexico") && find_word(text, "New Mexico")) return NULL;
    return best;
}

/* ------------------------------------------------------------------ */
/* Kombination                                                        */
/* ------------------------------------------------------------------ */

const char *kiwi_dir_geo_resolve(const char *loc, const char *name, int has_geo, int lat_cdeg, int lon_cdeg)
{
    const char *txt = kiwi_dir_geo_from_text(loc);
    if (!txt) txt = kiwi_dir_geo_from_text(name);
    if (!has_geo) return txt;

    const char *grid = kiwi_dir_geo_lookup(lat_cdeg, lon_cdeg);
    if (!txt) return grid;                      /* nur Koordinaten */
    if (!grid || !strcmp(txt, grid)) return txt;
    if (!in_grid(txt)) return txt;              /* z. B. Singapore: Karte kennt es nicht -> nicht widerlegbar */
    return country_nearby(txt, lat_cdeg, lon_cdeg) ? txt : grid;
}
