#!/usr/bin/env python3
"""
Mock-KiwiSDR mit /SND UND /W/F auf EINEM Port (mehrere Verbindungen, gleicher Zeitstempel).

/SND wird vom unveraenderten MockKiwi (mock_kiwi.py) bedient: die Verbindung wird per MSG_PEEK
inspiziert und dann an ihn uebergeben. /W/F erzeugt synthetische Spektren (Rauschteppich +
Traeger bei carrier_hz), roh oder IMA-ADPCM (mit 10 Vorlauf-Samples AM ANFANG der Zeile wie beim
echten Kiwi v1.902), und merkt sich alle Kommandos. Nur localhost.
"""
import random, socket, struct, threading, time, base64, hashlib
from mock_kiwi import MockKiwi, AdpcmEnc, ws_frame

BW = 30_000_000

class FakeSock:
    def __init__(self, conn): self.conn = conn
    def accept(self): return self.conn, ("127.0.0.1", 0)
    def close(self): pass

class MockKiwiWF(threading.Thread):
    def __init__(self, carrier_hz=1_010_000, reject_wf=None, wf_fft=1024):
        super().__init__(daemon=True)
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0)); self.srv.listen(4); self.srv.settimeout(0.2)
        self.port = self.srv.getsockname()[1]
        self.carrier, self.reject, self.wf_fft = carrier_hz, reject_wf, wf_fft
        self.paths, self.wf_cmds, self.wf_origin = [], [], ""
        self.wf_connects = 0
        self.snd = None
        self.stop = False
        self.wf_closed_cleanly = False

    def run(self):
        while not self.stop:
            try: conn, _ = self.srv.accept()
            except socket.timeout: continue
            except OSError: break
            req = b""
            conn.settimeout(2)
            while b"\r\n\r\n" not in req:
                req = conn.recv(4096, socket.MSG_PEEK)
            path = req.decode().split()[1]
            self.paths.append(path)
            if path.endswith("/SND"):
                m = MockKiwi(); m.sock.close(); m.sock = FakeSock(conn); self.snd = m; m.start()
            elif path.endswith("/W/F"):
                self.wf_connects += 1
                threading.Thread(target=self.wf, args=(conn, req.decode()), daemon=True).start()
            else:
                conn.close()
        self.srv.close()

    def _spectrum(self, zoom, counter):
        start = counter * BW / (1 << 24)
        vs = BW / (1 << zoom)
        out = bytearray()
        rnd = random.Random(1)
        for i in range(1024):
            f = start + (i + 0.5) * vs / 1024
            dbm = -110 + rnd.random() * 3
            d = abs(f - self.carrier)
            if d < 0.9 * vs / 1024 + 150: dbm = -50 - d / 400
            out.append(max(0, min(255, int(dbm + 255))))
        return bytes(out)

    def wf(self, conn, text):
        self.wf_origin = ([l.split(':', 1)[1].strip() for l in text.split('\r\n') if l.lower().startswith('origin')] or [''])[0]
        key = [l.split(":", 1)[1].strip() for l in text.split("\r\n") if l.lower().startswith("sec-websocket-key")][0]
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        # Der Handshake-Request wurde nur "angesehen": jetzt wirklich lesen.
        conn.recv(len(text.encode()))
        conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
        conn.settimeout(0.05)
        buf = bytearray(); zoom, counter, comp, seq = 0, 0, False, 0; sent_setup = False; next_t = time.time()
        try:
            while not self.stop:
                try:
                    d = conn.recv(4096)
                    if not d: break
                    buf += d
                except socket.timeout: pass
                for op, data in MockKiwi._read_frames(None, conn, buf):
                    if op == 8: self.wf_closed_cleanly = True; return
                    if op != 1 and op != 2: continue
                    cmd = data.decode(); self.wf_cmds.append(cmd)
                    if cmd.startswith("SET auth") and not sent_setup:
                        sent_setup = True
                        if self.reject:
                            conn.sendall(ws_frame(2, b"MSG " + self.reject.encode())); continue
                        for m in (b"MSG badp=0", b"MSG version_maj=1 version_min=902",
                                  ("MSG bandwidth=%d center_freq=15000000 wf_fft_size=%d zoom_max=14" % (BW, self.wf_fft)).encode(),
                                  b"MSG wf_setup"):
                            conn.sendall(ws_frame(2, m))
                    if cmd.startswith("SET zoom="):
                        kv = dict(p.split("=") for p in cmd[4:].split())
                        zoom, counter = int(kv["zoom"]), int(kv["start"])
                    if cmd.startswith("SET wf_comp="): comp = cmd.endswith("1")
                if sent_setup and not self.reject and time.time() >= next_t:
                    next_t += 0.1
                    z = zoom
                    if not any(c.startswith("SET zoom=") for c in self.wf_cmds): z = 0   # Voreinstellung des Servers
                    data = self._spectrum(z, counter)
                    if comp:
                        e = AdpcmEnc(); s = [data[0]] * 10 + list(data)
                        # ADPCM arbeitet auf 16-Bit-Samples: Bytewerte direkt als Samples uebertragen
                        body = e.encode(s); fz = z | 0x10000
                    else: body = data; fz = z
                    conn.sendall(ws_frame(2, b"W/F\x00" + struct.pack("<III", counter, fz, seq) + body)); seq += 1
        except (ConnectionError, OSError): pass
        finally: conn.close()

if __name__ == "__main__":
    m = MockKiwiWF(); print("Mock-Kiwi (SND + W/F) auf 127.0.0.1:%d" % m.port); m.run()
