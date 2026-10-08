#!/usr/bin/env python3
"""
Minimaler Mock-KiwiSDR zum Testen OHNE echten Server.

Er spricht genau das Protokoll, das aus kiwiclient abgeleitet wurde:
  WebSocket-Handshake auf /<ts>/SND -> MSG-Nachrichten -> auf "SET AR OK"
  und "SET mod=..." beginnt er, SND-Frames mit einem 1-kHz-Ton zu senden
  (roh = int16 big endian, oder IMA-ADPCM, falls der Client keine
  Kompression abgeschaltet hat). Alle empfangenen Befehle werden mitprotokolliert.
Es wird NIE ein Netzwerk ausserhalb von localhost benutzt.
"""
import base64, hashlib, math, socket, struct, threading, time

STEP = (7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
        130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
        1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
        5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
        27086,29794,32767)
ADJ = (-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8)

class AdpcmEnc:
    """Bruteforce-Encoder: probiert alle 16 Codes, nimmt den mit dem kleinsten Fehler.
    Rekonstruktion identisch zum Dekoder -> Encoder/Decoder bleiben synchron."""
    def __init__(self): self.idx = 0; self.prev = 0
    def _dec(self, idx, prev, code):
        step = STEP[idx]; idx = max(0, min(88, idx + ADJ[code]))
        d = step >> 3
        if code & 1: d += step >> 2
        if code & 2: d += step >> 1
        if code & 4: d += step
        if code & 8: d = -d
        return idx, max(-32768, min(32767, prev + d))
    def code(self, target):
        best = min(range(16), key=lambda c: abs(self._dec(self.idx, self.prev, c)[1] - target))
        self.idx, self.prev = self._dec(self.idx, self.prev, best)
        return best
    def encode(self, samples):
        out = bytearray()
        for i in range(0, len(samples), 2):
            lo = self.code(samples[i]); hi = self.code(samples[i + 1])
            out.append(lo | (hi << 4))
        return bytes(out)

def ws_frame(op, payload, fin=True):
    n = len(payload)
    h = bytes([(0x80 if fin else 0) | op])
    if n < 126: h += bytes([n])
    elif n < 65536: h += bytes([126]) + struct.pack(">H", n)
    else: h += bytes([127]) + struct.pack(">Q", n)
    return h + payload

class MockKiwi(threading.Thread):
    def __init__(self, tone_hz=1000.0, big_msg=False, ping=False, frames_per_sec_scale=1.0, fail_with=None, path_prefix="/ws/kiwi"):
        super().__init__(daemon=True)
        self.sock = socket.socket(); self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0)); self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.tone, self.big, self.ping, self.scale, self.fail_with = tone_hz, big_msg, ping, frames_per_sec_scale, fail_with
        self.origin = ""
        self.commands, self.path, self.got_close, self.closed_cleanly = [], "", False, False
        self.compressed_sent = False
        self.path_prefix = path_prefix   # wie der echte Kiwi v1.9xx: falscher Pfad -> 101, aber dann Schweigen

    def _read_frames(self, conn, buf):
        """Liefert (op, payload) fuer alle vollstaendigen maskierten Client-Frames."""
        out = []
        while len(buf) >= 2:
            op = buf[0] & 15; n = buf[1] & 127; i = 2
            if n == 126: n = struct.unpack(">H", buf[2:4])[0]; i = 4
            elif n == 127: n = struct.unpack(">Q", buf[2:10])[0]; i = 10
            assert buf[1] & 0x80, "Client-Frames muessen maskiert sein!"
            if len(buf) < i + 4 + n: break
            mask = buf[i:i+4]; data = bytes(b ^ mask[k & 3] for k, b in enumerate(buf[i+4:i+4+n]))
            out.append((op, data)); del buf[:i+4+n]
        return out

    def run(self):
        conn, _ = self.sock.accept(); conn.settimeout(0.05)
        req = b""
        while b"\r\n\r\n" not in req: req += conn.recv(1024) if True else b""
        text = req.decode(); self.path = text.split()[1]
        self.origin = ([l.split(':', 1)[1].strip() for l in text.split('\r\n') if l.lower().startswith('origin')] or [''])[0]
        key = [l.split(":", 1)[1].strip() for l in text.split("\r\n") if l.lower().startswith("sec-websocket-key")][0]
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
        buf = bytearray(); streaming = False; seq = 0; enc = AdpcmEnc(); phase = 0.0
        compression = True; got_ar = False; got_mod = False; next_t = time.time(); authed = False
        RATE, N = 12000, 2048     # Kiwi v1.9xx: 2048 Samples/Frame (roh = 4106 Byte!)
        path_ok = (self.path.startswith(self.path_prefix + "/") if self.path_prefix else not self.path.startswith("/ws/"))
        try:
            while True:
                try:
                    d = conn.recv(4096)
                    if not d: self.closed_cleanly = self.got_close; break
                    buf += d
                except socket.timeout: pass
                for op, data in self._read_frames(conn, buf):
                    if op == 8: self.got_close = True; continue
                    if op == 10: continue
                    cmd = data.decode(); self.commands.append(cmd)
                    if cmd.startswith("SET auth") and not authed:
                        authed = True
                        if not path_ok: continue          # falscher Pfad: nie antworten
                        if self.fail_with:
                            conn.sendall(ws_frame(2, b"MSG " + self.fail_with.encode())); continue
                        conn.sendall(ws_frame(2, b"MSG sample_rate=11998.895009"))   # kommt beim echten Kiwi zuerst
                        conn.sendall(ws_frame(2, b"MSG badp=0"))
                        conn.sendall(ws_frame(2, b"MSG version_maj=1 version_min=902"))
                        if self.big:   # riesige Konfigurationsnachricht (muss uebersprungen werden)
                            conn.sendall(ws_frame(2, b"MSG load_cfg=" + b"A" * 20000))
                        conn.sendall(ws_frame(2, b"MSG audio_init=0 audio_rate=12000"))
                    if cmd.startswith("SET AR OK"): got_ar = True
                    if cmd.startswith("SET mod="): got_mod = True
                    if cmd == "SET compression=0": compression = False
                    if got_ar and got_mod and not streaming:
                        streaming = True; next_t = time.time()
                        if self.ping: conn.sendall(ws_frame(9, b"hi"))
                if self.got_close: self.closed_cleanly = True; break
                while streaming and time.time() >= next_t:
                    # Ton; der erste Frame ist "Muell" (Rest des Vorgaengers), wird vom Client verworfen
                    s = []
                    for _ in range(N):
                        s.append(int(8000 * math.sin(phase))); phase += 2 * math.pi * self.tone / RATE
                    if seq == 0: s = [12345] * N
                    if compression: body = enc.encode(s); flags = 0x10; self.compressed_sent = True
                    else: body = b"".join(struct.pack(">h", v) for v in s); flags = 0
                    conn.sendall(ws_frame(2, b"SND" + bytes([flags]) + struct.pack("<I", seq) + struct.pack(">H", 1000) + body))
                    seq += 1; next_t += N / RATE / self.scale
        except (ConnectionError, OSError):
            pass
        finally:
            conn.close(); self.sock.close()

if __name__ == "__main__":
    import sys
    m = MockKiwi(); print("Mock-Kiwi lauscht auf 127.0.0.1:%d" % m.port); m.run()
