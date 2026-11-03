#!/usr/bin/env python3
"""
End-to-End-Test des Verzeichnisdienst-Clients: kiwi_dirtool (C) gegen einen lokalen
Mock-HTTP-Server. Geprueft werden Umleitung, "Transfer-Encoding: chunked",
Content-Length, Fehlerseiten, abgeschnittene Antworten, Groessenlimit-Verhalten,
die Datenbankdatei ueber mehrere Aufrufe (A/B-Wechsel) und die Kapazitaet.
Es wird nur localhost benutzt.
"""
import json, os, re, subprocess, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "kiwi_dirtool")
fails = 0
def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FEHLER ") + msg)
    if not cond: fails += 1

# Testdaten: echte Eintraege aus testdata/kiwisdr_sample.js, auf n Server vervielfacht
sample = open(os.path.join(HERE, "testdata", "kiwisdr_sample.js"), encoding="utf-8").read()
HEAD = sample[:sample.index("[") + 1]
OBJS = [m for m in re.findall(r"\t\{\n.*?\n\t\}", sample, re.S)]
def make_list(n):
    out = [HEAD, "\n"]
    for i in range(n):
        o = OBJS[i % len(OBJS)]
        o = re.sub(r'"url":"http://[^"]*"', '"url":"http://gen%d.example.org:%d"' % (i, 8000 + i % 100), o)
        out.append(o + ",\n")
    out.append("]\n;\n")
    return "".join(out).encode("utf-8")

LIST_800, LIST_2000 = make_list(800), make_list(2000)

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    hits = []
    def log_message(self, *a): pass
    def _send(self, code, body, extra=(), clen=True):
        self.send_response(code)
        for k, v in extra: self.send_header(k, v)
        if clen: self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        H.hits.append((self.path, self.headers.get("Host"), self.headers.get("User-Agent")))
        p = self.path
        if p == "/kiwisdr_com.js":   self._send(302, b"", [("Location", "/data/list.js")])        # relative Umleitung
        elif p == "/data/list.js":                                                               # chunked, krumme Stuecke
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close"); self.end_headers()
            b = LIST_800; i = 0; sizes = [1, 17, 4096, 333, 8192]; k = 0
            while i < len(b):
                n = min(sizes[k % len(sizes)], len(b) - i); k += 1
                self.wfile.write(b"%x\r\n" % n + b[i:i + n] + b"\r\n"); i += n
            self.wfile.write(b"0\r\n\r\n")
        elif p == "/plain.js":       self._send(200, LIST_800)
        elif p == "/big.js":         self._send(200, LIST_2000)
        elif p == "/small.js":       self._send(200, make_list(5))
        elif p == "/404":            self._send(404, b"nope")
        elif p == "/html":           self._send(200, b"<html><body>Wartung</body></html>")
        elif p == "/trunc.js":                                                                    # Content-Length lang, Verbindung bricht ab
            self.send_response(200); self.send_header("Content-Length", str(len(LIST_800))); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(LIST_800[:30000]); self.wfile.flush(); self.close_connection = True
        elif p == "/https":          self._send(302, b"", [("Location", "https://example.org/x")])
        elif p == "/loop":           self._send(302, b"", [("Location", "/loop")])
        elif p == "/gzip":           self._send(200, b"\x1f\x8b", [("Content-Encoding", "gzip")])
        else:                        self._send(404, b"")

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
PORT = srv.server_address[1]
DB = os.path.join(HERE, "e2e_dir_db.bin")

def run(*cmds, url="/kiwisdr_com.js", fresh=False):
    if fresh and os.path.exists(DB): os.remove(DB)
    p = subprocess.run([TOOL, "-d", DB, "-u", "http://127.0.0.1:%d%s" % (PORT, url)] + list(cmds),
                       capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL)
    return p.stdout

print("== Update: Umleitung + chunked, 800 Server")
o = run("d update", "d info", fresh=True)
check("Update ok: 800 Server (vorher 0)" in o, "800 Server uebernommen (Umleitung + chunked): %s" % [l for l in o.splitlines() if "Update" in l])
check("Generation 1" in o and "Slot A" in o, "erste Generation in Slot A")
check(H.hits[0][0] == "/kiwisdr_com.js" and H.hits[1][0] == "/data/list.js", "relative Umleitung befolgt")
check(H.hits[0][1] == "127.0.0.1:%d" % PORT and H.hits[0][2] == "kiwi_esp32", "Host-Header mit Port, User-Agent")

print("== zweiter Aufruf: DB aus der Datei, Content-Length, A/B-Wechsel")
o = run("d info", "d update", "d info", url="/plain.js")
check("800 Server, " in o.splitlines()[1] and "Slot A" in o.splitlines()[1], "DB wurde aus Datei geladen (Slot A)")
check("Update ok: 800 Server (vorher 800)" in o and "Generation 2" in o.split("Update ok")[1] and "Slot B" in o.split("Update ok")[1], "Update -> Slot B, Generation 2")

print("== Fehlerfaelle lassen die alte DB unberuehrt")
for url, expect in [("/404", "200 OK"), ("/html", "keine brauchbare"), ("/trunc.js", "Netzwerkfehler"),
                    ("/https", "https"), ("/loop", "200 OK"), ("/gzip", "200 OK"), ("/small.js", "auffaellig klein")]:
    o = run("d update", "d info", url=url)
    check(expect in o and "Update fehlgeschlagen" in o, "%s -> %s" % (url, [l for l in o.splitlines() if "fehlgeschlagen" in l]))
    check("800 Server, " in o.splitlines()[-2] and "Slot B" in o.splitlines()[-2], "  alte DB unveraendert (800, Slot B)")

print("== Kapazitaet: 2000 Server, Slot fasst 1536")
o = run("d update", "d info", url="/big.js")
check("Update ok: 1536 Server (vorher 800)" in o and "464 ueberzaehlig" in o, "Ueberzaehlige gezaehlt: %s" % [l for l in o.splitlines() if "Update ok" in l])

print("== Auswahl auf den echten Beispieldaten")
o = run("d update", "d filter Germany", "d list", "d go best", "d countries", url="/small.js")
# /small.js hat nur 5 Server -> abgelehnt (1536 -> 5); danach arbeiten wir mit der vorhandenen DB weiter
check("auffaellig klein" in o, "kleine Liste abgelehnt")
check("Filter: Land=Germany" in o, "Laenderfilter: %s" % [l for l in o.splitlines() if l.startswith("Filter")])
check("kiwi_pc -p" in o, "d go best liefert einen Server: %s" % [l for l in o.splitlines() if "kiwi_pc" in l])

try: os.remove(DB)
except OSError: pass
print("\n%s" % ("ALLE DIR-E2E-TESTS BESTANDEN" if not fails else "%d FEHLER" % fails))
sys.exit(1 if fails else 0)
