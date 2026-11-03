#!/usr/bin/env python3
"""
Erzeugt src/kiwi_dir_geo_data.c: eine winzige "Koordinate -> Land"-Karte.

WARUM?  Im Feld "loc" des Verzeichnisdienstes steht FREIER TEXT ("Stevensville, MT",
"ALPA CORRAL ARGENTINA"), oft ohne Land. Die Koordinaten ("gps") sind dagegen
immer vorhanden. Der ESP32 hat keinen Platz fuer Laendergrenzen als Polygone,
daher wird einmalig (hier, auf dem PC) ein Raster berechnet:

    0,5-Grad-Gitter (720 x 360 Zellen)  ->  je Zelle eine Land-ID (0 = Meer)
    zeilenweise lauflaengenkodiert (RLE) ->  ca. 10 KB const-Daten im Flash

Genauigkeit: ca. +-50 km an Grenzen (Quelle ist die grobe 1:110m-Karte); fuer
"zeige mir Kiwis in Land X" reicht das, zumal der Text im Zweifel Vorrang hat
(siehe kiwi_dir_geo.c).

QUELLE: Natural Earth 1:110m Admin-0 Countries (gemeinfrei, public domain).
        Liegt z. B. im pip-Paket geopandas==0.14.4 (geopandas/datasets/naturalearth_lowres).
AUFRUF: pip install shapely pyshp numpy
        python3 gen_geo_table.py <pfad/zu/naturalearth_lowres(.shp)> ../src/kiwi_dir_geo_data.c
"""
import sys
import numpy as np
import shapefile
import shapely
from shapely.geometry import shape
from shapely.strtree import STRtree

STEP = 0.5                    # Rasterweite in Grad
NEAR = 0.75                   # Zellen im Meer bis zu dieser Entfernung (Grad) zur Kueste bekommen das Land
RENAME = {                    # NE-Name -> gebraeuchlicher Name (ASCII, <= 23 Zeichen)
    "United States of America": "USA", "United Kingdom": "UK", "Czechia": "Czech Republic",
    "Bosnia and Herz.": "Bosnia and Herzegovina", "Central African Rep.": "Central African Rep.",
    "Dem. Rep. Congo": "DR Congo", "Dominican Rep.": "Dominican Republic",
    "Eq. Guinea": "Equatorial Guinea", "Falkland Is.": "Falkland Islands", "N. Cyprus": "Cyprus",
    "S. Sudan": "South Sudan", "Solomon Is.": "Solomon Islands", "W. Sahara": "Western Sahara",
    "eSwatini": "Eswatini", "Côte d'Ivoire": "Ivory Coast", "Somaliland": "Somalia",
}
SKIP = {"Fr. S. Antarctic Lands"}

def main(src, dst):
    r = shapefile.Reader(src)
    geoms, names = [], []
    for sr in r.shapeRecords():
        n = sr.record["name"]
        if n in SKIP: continue
        n = RENAME.get(n, n)
        assert len(n) <= 23, n
        geoms.append(shape(sr.shape.__geo_interface__)); names.append(n)
    uniq = sorted(set(names))
    assert len(uniq) < 255
    ident = {n: i + 1 for i, n in enumerate(uniq)}
    tree = STRtree(geoms)

    ncol, nrow = int(360 / STEP), int(180 / STEP)
    lons = -180 + STEP * (np.arange(ncol) + 0.5)
    lats = 90 - STEP * (np.arange(nrow) + 0.5)
    pts = shapely.points(np.repeat(lons[None, :], nrow, 0).ravel(), np.repeat(lats[:, None], ncol, 1).ravel())
    idx, dist = tree.query_nearest(pts, return_distance=True, all_matches=False)
    grid = np.zeros(len(pts), dtype=np.uint8)
    for k, g, d in zip(idx[0], idx[1], dist):
        if d <= NEAR:
            grid[k] = ident[names[g]]
    grid = grid.reshape(nrow, ncol)

    runs, rows = [], []
    for y in range(nrow):
        rows.append(len(runs) // 2)
        x = 0
        while x < ncol:
            v = grid[y, x]; n = 1
            while x + n < ncol and grid[y, x + n] == v and n < 255: n += 1
            runs += [int(v), n]; x += n
    rows.append(len(runs) // 2)
    assert rows[-1] < 65535

    with open(dst, "w", encoding="utf-8") as f:
        f.write("/* GENERIERT von tools/gen_geo_table.py - nicht von Hand aendern.\n"
                " * Quelle: Natural Earth 1:110m Admin-0 (public domain). Raster %.1f Grad, RLE. */\n" % STEP)
        f.write('#include "kiwi_dir_geo.h"\n\n')
        f.write("const char *const kiwi_dir_geo_names[%d] = {\n" % len(uniq))
        for n in uniq: f.write('    "%s",\n' % n)
        f.write("};\nconst unsigned kiwi_dir_geo_name_count = %d;\n" % len(uniq))
        f.write("const unsigned kiwi_dir_geo_step_milli = %d;   /* Rasterweite in 0,001 Grad */\n" % int(STEP * 1000))
        f.write("const unsigned kiwi_dir_geo_rows_n = %d;\nconst unsigned kiwi_dir_geo_cols_n = %d;\n" % (nrow, ncol))
        f.write("const uint16_t kiwi_dir_geo_row_start[%d] = {" % len(rows))
        f.write(",".join(str(v) for v in rows)); f.write("};\n")
        f.write("/* Paare {Land-ID, Lauflaenge}; ID = Index in kiwi_dir_geo_names + 1, 0 = Meer */\n")
        f.write("const uint8_t kiwi_dir_geo_runs[%d] = {\n" % len(runs))
        for i in range(0, len(runs), 24): f.write(",".join(str(v) for v in runs[i:i + 24]) + ",\n")
        f.write("};\n")
    print("%d Laender, %d Laufpaare, %d Byte Daten" % (len(uniq), len(runs) // 2, len(runs) + 2 * len(rows)))

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
