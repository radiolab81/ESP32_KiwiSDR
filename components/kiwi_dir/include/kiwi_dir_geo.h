/**
 * @file kiwi_dir_geo.h
 * @brief Land eines Servers bestimmen (Text + Koordinaten, ohne Netz, ohne RAM).
 *
 * PROBLEM
 * -------
 * Der Verzeichnisdienst liefert KEIN Landesfeld. "loc" ist freier Text, den der
 * Betreiber selbst eintippt: "Hattingen NRW, Germany", "Stevensville, MT",
 * "ALPA CORRAL ARGENTINA", "Nailsea, Bristol". Ein Filter "nach Land" braucht
 * aber eindeutige Laender. Zwei Quellen werden deshalb kombiniert:
 *
 *   1. TEXT:        Ein bekannter Landesname (oder Alias wie "UK", "Deutschland")
 *                   irgendwo in "loc" bzw. "name" (ganzes Wort, Gross-/Klein egal,
 *                   bei mehreren Treffern gewinnt der laengste: "Papua New Guinea"
 *                   schlaegt "Guinea").
 *   2. KOORDINATEN: "gps" ist bei (fast) jedem Eintrag vorhanden. Eine
 *                   eingebaute Rasterkarte (0,5 Grad, ~13 KB im Flash, erzeugt von
 *                   tools/gen_geo_table.py) liefert das Land zur Position.
 *
 * Bei Widerspruch gewinnt der Text, ABER nur, wenn das Land auch in der Naehe der
 * Koordinaten liegt (+-1 Grad): das faengt Ortsnamen wie "Peru, IL", "Lebanon, PA"
 * oder "New Mexico" ab, ohne Grenzorte (Text "Germany", Raster sagt "Netherlands")
 * falsch umzudeuten.
 */
#ifndef KIWI_DIR_GEO_H
#define KIWI_DIR_GEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Von tools/gen_geo_table.py erzeugte Daten (kiwi_dir_geo_data.c), im Flash. */
extern const char *const kiwi_dir_geo_names[];
extern const unsigned    kiwi_dir_geo_name_count;
extern const unsigned    kiwi_dir_geo_step_milli, kiwi_dir_geo_rows_n, kiwi_dir_geo_cols_n;
extern const uint16_t    kiwi_dir_geo_row_start[];
extern const uint8_t     kiwi_dir_geo_runs[];

/** Land zu einer Position (Koordinaten in 0,01 Grad). NULL = Meer/unbekannt. */
const char *kiwi_dir_geo_lookup(int lat_cdeg, int lon_cdeg);

/** Land aus Freitext. NULL = kein Landesname gefunden. */
const char *kiwi_dir_geo_from_text(const char *text);

/**
 * Kombination beider Quellen (Regeln siehe oben).
 * @param loc,name  Texte (beide duerfen NULL/leer sein); "loc" wird zuerst durchsucht
 * @param has_geo   0, wenn keine Koordinaten bekannt sind
 * @return Landesname (statischer String) oder NULL
 */
const char *kiwi_dir_geo_resolve(const char *loc, const char *name, int has_geo, int lat_cdeg, int lon_cdeg);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_DIR_GEO_H */
