#!/usr/bin/env python3
"""
End-to-End-Test: kiwi_pc (C-Client) gegen den lokalen Mock-Kiwi.
Prueft Protokollablauf, Audio-Dekodierung (roh + ADPCM), Abstimmen zur
Laufzeit, Verwerfen riesiger Nachrichten, Ping/Pong und sauberes Schliessen.
"""
import math, os, struct, subprocess, sys, wave
from mock_kiwi import MockKiwi

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FEHLER ") + msg)
    if not cond: fails += 1

def goertzel(samples, rate, f):
    w = 2 * math.pi * f / rate; c = 2 * math.cos(w); s1 = s2 = 0.0
    for x in samples: s1, s2 = x + c * s1 - s2, s1
    return math.sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (len(samples) / 2)

def run(name, extra, secs, stdin_text=None, **mock_kw):
    print("== " + name)
    m = MockKiwi(**mock_kw); m.start()
    out = os.path.join(HERE, "e2e_%s.wav" % name)
    cmd = [os.path.join(HERE, "kiwi_pc"), "-p", str(m.port), "-f", "225", "-m", "am", "-t", str(secs), "-o", out] + extra + ["127.0.0.1"]
    p = subprocess.run(cmd, input=stdin_text, capture_output=True, text=True, timeout=30)
    m.join(timeout=5)
    return m, p, out

def analyse(out, expect_hz=1000.0, min_s=2.0):
    with wave.open(out) as w:
        check(w.getframerate() == 12000 and w.getnchannels() == 1 and w.getsampwidth() == 2, "WAV: 12 kHz / mono / 16 Bit")
        raw = w.readframes(w.getnframes())
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    check(len(s) / 12000 >= min_s, "Audiolaenge %.2f s" % (len(s) / 12000))
    check(12345 not in s[:600], "erster Frame (Muell) wurde verworfen")
    seg = s[2000:6000]
    amp = goertzel(seg, 12000, expect_hz); off = goertzel(seg, 12000, expect_hz * 1.5)
    peak = max(abs(x) for x in seg)
    dc = abs(sum(seg) / len(seg))
    check(dc < 300, "kein Gleichspannungsversatz (Mittelwert %.0f)" % dc)
    check(amp > 6000 and off < amp / 20, "Ton %.0f Hz erkannt (Amplitude %.0f, Nachbar %.0f, Spitze %d)" % (expect_hz, amp, off, peak))

def common(m, p):
    check(p.returncode == 0, "Exit-Code 0 (stderr: %s)" % p.stderr.strip().splitlines()[-1:] )
    check(m.path.endswith("/SND"), "Pfad %s" % m.path)
    c = m.commands
    check(c[0] == "SET auth t=kiwi p=#", "Anmeldung: %r" % c[0])
    check(c[1] == "SERVER DE CLIENT kiwi_esp32 SND", "Selbstauskunft: %r" % c[1])
    check(m.path.startswith("/ws/kiwi/"), "Pfad mit Praefix /ws/kiwi")
    check(m.origin.startswith("http://127.0.0.1:"), "Origin-Header: %r" % m.origin)
    check("SET AR OK in=12000 out=44100" in c, "AR OK quittiert")
    check("SET mod=am low_cut=-4900 high_cut=4900 freq=225.000" in c, "Abstimmung AM 225.000 kHz")
    check(any(x.startswith("SET agc=1") for x in c), "AGC gesetzt")
    check(sum(x == "SET keepalive" for x in c) >= 2, "Keepalives: %d" % sum(x == "SET keepalive" for x in c))
    check(m.got_close and m.closed_cleanly, "Close-Frame gesendet und Verbindung sauber geschlossen")

# 1) Rohes PCM + riesige Nachricht + Ping
m, p, out = run("raw", [], 3, big_msg=True, ping=True)
common(m, p); check("SET compression=0" in m.commands, "Kompression abgeschaltet"); check(not m.compressed_sent, "Server sendet rohes PCM")
analyse(out)

# 2) ADPCM
m, p, out = run("adpcm", ["-c"], 3)
common(m, p); check("SET compression=0" not in m.commands, "Kompression NICHT abgeschaltet"); check(m.compressed_sent, "Server sendet ADPCM")
analyse(out)

# 2b) Alter Pfad ohne Praefix (-l) gegen einen "alten" Mock
m, p, out = run("legacy", ["-l"], 3, path_prefix="")
print("== legacy")
check(p.returncode == 0 and not m.path.startswith("/ws/"), "Legacy-Pfad %s" % m.path)

# 2c) Regression des beobachteten Fehlers: falscher Pfad -> Kiwi schweigt -> Client meldet Timeout
m, p, out = run("silent", ["-l"], 20)   # Mock verlangt /ws/kiwi, Client nimmt alten Pfad (Timeout nach 10 s)
print("== silent (falscher Pfad)")
check(p.returncode != 0 and "Zeitueberschreitung" in p.stderr, "Timeout bei falschem Pfad: %s" % p.stderr.strip().splitlines()[-1:])

# 3) Abstimmen zur Laufzeit ueber stdin (f / m)
import time
m = MockKiwi(tone_hz=1000.0); m.start()
out = os.path.join(HERE, "e2e_tune.wav")
pr = subprocess.Popen([os.path.join(HERE, "kiwi_pc"), "-p", str(m.port), "-f", "225", "-m", "am", "-t", "0", "-i", "-o", out, "127.0.0.1"],
                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
print("== tune")
time.sleep(1.0); pr.stdin.write("f 7055.5\n"); pr.stdin.flush(); time.sleep(0.5); pr.stdin.write("m usb\n"); pr.stdin.flush(); time.sleep(0.5)
pr.stdin.write("q\n"); pr.stdin.flush(); pr.wait(timeout=10); m.join(timeout=5)
check(pr.returncode == 0, "Exit-Code 0")
check("SET mod=am low_cut=-4900 high_cut=4900 freq=7055.500" in m.commands, "f 7055.5 -> %s" % [c for c in m.commands if "7055" in c][:1])
check("SET mod=usb low_cut=300 high_cut=2700 freq=7055.500" in m.commands, "m usb -> Passband USB")
check(m.got_close, "sauber geschlossen")

# 4) Fehlerfall: Server meldet too_busy -> Client beendet sich mit Fehler und schliesst
m, p, out = run("busy", [], 3, fail_with="too_busy=4")
print("== busy")
check(p.returncode != 0 and "ausgelastet" in p.stderr, "too_busy erkannt: %s" % p.stderr.strip().splitlines()[-1:])
check(m.got_close, "auch im Fehlerfall sauber geschlossen")

print("\n%s" % ("ALLE E2E-TESTS BESTANDEN" if not fails else "%d FEHLER" % fails))
sys.exit(1 if fails else 0)
