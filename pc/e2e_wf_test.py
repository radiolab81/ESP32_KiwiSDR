#!/usr/bin/env python3
"""
End-to-End-Test der Spektrumanzeige: echter kiwi_pc-Client (Audio + /W/F) gegen mock_kiwi_wf.py,
Anzeige auf einem simulierten ST7735/SSD1306 (sim_panel, dekodiert die echten Treiberbytes).

Geprueft wird:
  1. /W/F verwendet DENSELBEN Zeitstempel wie /SND (sonst weist ein echter Kiwi die Sitzung nicht zu)
  2. Zoom/Start (SET zoom=9 start=...), Anmeldung, wf_comp=1 (ADPCM), Origin
  3. der Traeger liegt auf dem Display an der richtigen Spalte (aus dem Bild ausgelesen), Markierung in der Mitte
  4. Umstimmen per 'f' verschiebt den Traeger, ohne neuen Server-Befehl-Sturm
  5. Abgelehnter /W/F (too_busy): Audio laeuft weiter, genau EIN Versuch, Anzeige "NO DATA"
  6. Ohne -D wird KEINE zweite Verbindung aufgebaut
"""
import os, re, subprocess, sys, time
from mock_kiwi_wf import MockKiwiWF, BW

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok: fails += 1

def read_ppm(path):
    d = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", d)
    w, h = int(m.group(1)), int(m.group(2)); px = d[m.end():]
    return w, h, lambda x, y: tuple(px[3 * (y * w + x):3 * (y * w + x) + 3])

def trace_peak_col(path):
    w, h, g = read_ppm(path)
    best, col = h, -1
    for x in range(w):
        for y in range(h):
            r, gr, b = g(x, y)
            if gr > 200 and b > 200 and r < 60:        # TRACE = Cyan
                if y < best: best, col = y, x
                break
    return col, w, h, g

def run(mock, args, secs, stdin_script=None):
    out = os.path.join(HERE, "e2e_wf.wav")
    cmd = [os.path.join(HERE, "kiwi_pc"), "-p", str(mock.port), "-f", "1000", "-m", "usb", "-t", str(secs), "-o", out] + args + ["127.0.0.1"]
    if stdin_script is None:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    p = subprocess.Popen(cmd + [], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    for delay, line in stdin_script:
        time.sleep(delay); p.stdin.write(line); p.stdin.flush()
    o, e = p.communicate(timeout=60)
    class R: pass
    r = R(); r.stdout, r.stderr, r.returncode = o, e, p.returncode
    return r

ppm = os.path.join(HERE, "e2e_wf.ppm")

print("1) Normalbetrieb ST7735 (128x160), Traeger bei +10 kHz")
m = MockKiwiWF(carrier_hz=1_010_000); m.start()
r = run(m, ["-D", "st7735", "-P", ppm, "-c"], 6)
time.sleep(0.3)
check(r.returncode == 0, "kiwi_pc beendet ohne Fehler (rc=%d)" % r.returncode)
check(len(m.paths) == 2, "genau zwei Verbindungen: %s" % m.paths)
ts = [re.search(r"/ws/kiwi/(\d+)/", p).group(1) for p in m.paths if re.search(r"/ws/kiwi/(\d+)/", p)]
check(len(ts) == 2 and ts[0] == ts[1], "gleicher Zeitstempel fuer SND und W/F (%s)" % ts)
check(any(c.startswith("SET auth t=kiwi") for c in m.wf_cmds), "W/F meldet sich an")
zc = [c for c in m.wf_cmds if c.startswith("SET zoom=")]
check(len(zc) >= 1 and zc[0].startswith("SET zoom=9 "), "Zoom 9 fuer 50 kHz bei 30 MHz: %s" % (zc[:1],))
if zc:
    cnt = int(zc[0].split("start=")[1]); st = cnt * BW / (1 << 24); vs = BW / 512
    check(st <= 1_000_000 <= st + vs, "angefordertes Fenster enthaelt die Abstimmfrequenz (%.0f..%.0f)" % (st, st + vs))
check("SET wf_comp=1" in m.wf_cmds and "SET send_dB=1" in m.wf_cmds, "ADPCM und dB-Bytes angefordert")
check(len(zc) <= 3, "kein Befehlssturm (%d x SET zoom)" % len(zc))
check(m.wf_closed_cleanly, "W/F mit Close-Frame beendet")
col, w, h, g = trace_peak_col(ppm)
exp = w / 2 + 10 / 50 * w
check(abs(col - exp) <= 3, "Traegerspitze bei Spalte %d (erwartet %.1f von %d)" % (col, exp, w))
mark = [x for x in range(w) if any(g(x, y)[0] > 200 and g(x, y)[1] < 60 and g(x, y)[2] < 60 for y in range(h))]
check(mark and abs(sum(mark) / len(mark) - w / 2) <= 2, "rote Abstimmmarkierung in der Mitte (%s)" % (mark[:3],))
m.stop = True

print("2) SSD1306 (I2C, 128x64), rohe Zeilen, Umstimmen auf 1004 kHz")
m = MockKiwiWF(carrier_hz=1_010_000); m.start()
r = run(m, ["-D", "ssd1306", "-P", ppm, "-i"], 0, [(2.5, "s\n"), (0.2, "f 1004\n"), (2.5, "q\n")])
time.sleep(0.3)
w, h, g = read_ppm(ppm)
check((w, h) == (128, 64), "Bildgroesse 128x64")
# Traegerspitze im 1-Bit-Bild: oberste weisse Pixel je Spalte im Plot-Bereich (y 9..H-12)
tops = {}
for x in range(w):
    for y in range(9, h - 12):
        if g(x, y)[0] > 128: tops[x] = y; break
for x in range(w // 2 - 2, w // 2 + 3): tops.pop(x, None)      # Abstimmmarkierung (senkrechte Linie) ausblenden
peak = min(tops, key=lambda x: tops[x]) if tops else -1
exp = w / 2 + 6 / 50 * w
check(abs(peak - exp) <= 4, "nach f 1004: Spitze bei Spalte %d (erwartet %.1f)" % (peak, exp))
check(len([c for c in m.wf_cmds if c.startswith("SET zoom=")]) <= 3, "Umstimmen innerhalb des Fensters erzeugt keinen neuen Zoom-Befehl")
m.stop = True

print("3) Server lehnt /W/F ab (too_busy): Audio laeuft weiter, ein Versuch")
m = MockKiwiWF(reject_wf="too_busy=1"); m.start()
r = run(m, ["-D", "st7735", "-P", ppm], 5)
time.sleep(0.3)
check(r.returncode == 0, "Audio-Sitzung endet normal (rc=%d)" % r.returncode)
check(m.wf_connects == 1, "genau EIN W/F-Verbindungsversuch (%d)" % m.wf_connects)
check("Spektrum" in r.stderr and "Audio" in r.stderr, "Hinweis auf Ablehnung ausgegeben")
check(m.snd is not None and m.snd.commands and os.path.getsize(os.path.join(HERE, "e2e_wf.wav")) > 44 + 12000, "Audio wurde trotzdem aufgezeichnet")
m.stop = True

print("4) Ohne -D: keine zweite Verbindung")
m = MockKiwiWF(); m.start()
r = run(m, [], 3)
time.sleep(0.2)
check(m.wf_connects == 0 and len(m.paths) == 1, "nur /SND: %s" % m.paths)
m.stop = True

for f in ("e2e_wf.wav", "e2e_wf.ppm"):
    try: os.remove(os.path.join(HERE, f))
    except OSError: pass
print("\n%s" % ("ALLE WF-E2E-TESTS BESTANDEN" if not fails else "%d FEHLER" % fails))
sys.exit(1 if fails else 0)
