# KiwiSDR-Client für ESP32 und PC

**Deutsch** · [English](README.en.md)

Portabler C-Code (C11), der sich mit einem KiwiSDR verbindet, Frequenz und
Demodulation (AM u. a.) einstellt, den Audiostrom empfängt und als PCM ausgibt:

* **ESP32 (klassisch, ESP-IDF 6.1):** über den internen 8-Bit-DAC (GPIO25/GPIO26)
* **PC (Linux/macOS):** in eine WAV-Datei (oder roh nach stdout, z. B. für `aplay`)

Der Protokoll-Kern ist **derselbe Quellcode** auf beiden Plattformen.

```
kiwi-esp32/
├── components/kiwi_core/        <- portabler Kern (PC und ESP-IDF-Komponente)
│   ├── include/                    kiwi_client.h  kiwi_ws.h  kiwi_adpcm.h
│   │                               kiwi_port.h   kiwi_audio_sink.h
│   └── src/
│       ├── kiwi_ws.c               WebSocket (RFC 6455), ohne I/O
│       ├── kiwi_adpcm.c            IMA-ADPCM-Dekoder
│       ├── kiwi_client.c           KiwiSDR-Protokoll (Zustandsautomat)
│       └── port_sockets.c          BSD-Sockets (Linux UND lwIP im ESP-IDF)
├── main/                        <- nur ESP32: WLAN, DAC-Senke, Konsole
├── pc/                          <- nur PC: main, WAV-Senke, Tests, Mock-Kiwi
├── CMakeLists.txt, sdkconfig.defaults
```

---

## 1 Das KiwiSDR-Audioprotokoll (aus `kiwiclient` abgeleitet)

Die Analyse stützt sich vor allem auf `kiwi/client.py` (`KiwiSDRStream`),
`kiwi/wsclient.py` (Handshake) und `kiwirecorder.py` (Startsequenz).

### 1.1 Transport: WebSocket auf dem HTTP-Port

1. TCP-Verbindung zu `host:8073`.
2. `GET /ws/kiwi/<zeitstempel>/SND HTTP/1.1` mit `Upgrade: websocket`,
   `Origin: http://host:port`, `Sec-WebSocket-Key`, `Sec-WebSocket-Version: 13`.
   **Der Pfadpräfix `/ws/kiwi` ist bei KiwiSDR v1.9xx Pflicht** (per Browser-Mitschnitt
   an v1.902 belegt). Mit dem alten Pfad `/<zeitstempel>/SND` (so im mitgelieferten
   `kiwiclient`) antwortet der Server zwar mit `101`, schickt danach aber **nie ein Byte** –
   das war die Ursache des anfänglichen Timeouts. Für ältere Kiwis: `kiwi_pc -l`
   bzw. `path_prefix = ""`.
   Der Zeitstempel ist eine beliebige Zahl, die sich pro Verbindung unterscheidet. `SND` wählt den
   Audiokanal (`W/F` wäre der Wasserfall).
3. Antwort `101 Switching Protocols`. (`Sec-WebSocket-Accept` wird – wie im
   Python-Client – nicht geprüft.)
4. Client→Server-Frames sind **maskiert** (Pflicht), Server→Client-Frames nicht.

### 1.2 Steuerkanal: Text-Befehle `SET …` und Server-`MSG`

| Richtung | Nachricht | Bedeutung |
|---|---|---|
| C→S | `SET auth t=kiwi p=<passwort>` (ohne Passwort: `p=#`) | Anmeldung (sofort nach dem Handshake) |
| C→S | `SERVER DE CLIENT kiwi_esp32 SND` | Selbstauskunft (der Browser sendet das ebenfalls) |
| S→C | `MSG sample_rate=11998.895009` | kommt bei v1.902 als **erste** Nachricht: reale Rate, Kanal bereit |
| S→C | `MSG badp=0`, `MSG version_maj=1 version_min=902 …` | Anmeldung OK, Version |
| S→C | `MSG audio_init=0 audio_rate=12000` | nominelle Audio-Abtastrate |
| C→S | `SET AR OK in=12000 out=44100` | **Pflicht-Quittung**, sonst startet kein Audio |
| C→S | `SET squelch=0 max=0`, `SET genattn=0`, `SET gen=0 mix=-1` | neutrale Grundwerte |
| C→S | `SET ident_user=<name>` | Anzeigename |
| C→S | **`SET mod=am low_cut=-4900 high_cut=4900 freq=225.000`** | **Abstimmen**: Modus, Durchlassbereich (Hz), Frequenz in **kHz** |
| C→S | `SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50` | AGC |
| C→S | `SET compression=0` | rohes PCM statt ADPCM (optional) |
| C→S | `SET keepalive` | mindestens 1×/s, sonst Trennung |
| S→C | `MSG too_busy=…`, `badp=1..7`, `down=…`, `redirect=…` | Fehler |

`MSG`-Nachrichten haben das Format `MSG<1 Trennbyte>name=wert name=wert …`.
Manche (`load_cfg`, `load_dxcfg`) sind zehntausende Bytes groß und für den
Audioempfang unnötig – der Client **verwirft** Nachrichten, die nicht in den
Puffer passen (wichtig für den RAM-armen ESP32).

Bei einem Kiwi mit Frequenzversatz (Aufwärtskonverter, `freq_offset=`) erwartet
das Protokoll die *Basisband*-Frequenz = Anzeigefrequenz − Versatz; das erledigt
der Client automatisch.

### 1.3 Audio-Frames: `SND`

```
Offset  Länge  Inhalt
0       3      "SND"
3       1      flags: 0x02 ADC-Überlauf, 0x08 Stereo (IQ/DRM), 0x10 ADPCM, 0x80 LE (nur "camp")
4       4      Sequenznummer, Little Endian
8       2      S-Meter, Big Endian:  RSSI[dBm] = 0,1 · Wert − 127
10      …      Audiodaten
```

* Bei v1.902 enthält ein Frame immer 1024 Nutzbytes: **ADPCM = 2048 Samples (≈ 171 ms)**,
  **rohes PCM = 512 Samples (≈ 43 ms)** (am ESP32 mit rohem PCM gemessen: ca. 23 Frames/s).
  `KIWI_RX_BUF_SIZE` = 6144 bietet reichlich Reserve.
* **Roh (`flags&0x10 == 0`):** `int16`, **Big Endian**, mono.
* **ADPCM (`flags&0x10`):** IMA-ADPCM, 1 Byte = 2 Samples (**erst Low-, dann
  High-Nibble**). Der Dekoder (Prädiktor + Schrittindex) läuft über alle Frames
  der Verbindung weiter.
* Der **erste** SND-Frame kann noch Audio des vorherigen Kanalnutzers enthalten. Er wird
  dekodiert (damit der ADPCM-Zustand stimmt), aber nicht ausgegeben.
* Zwischen den SND-Frames kommen laufend `MSG user_cb=…`, `stats_cb=…` u. Ä. (JSON,
  vom Client ignoriert) sowie große `load_cfg=…`-Nachrichten (werden übersprungen).
* Lücken in der Sequenznummer = verlorene Frames (werden gezählt).

### 1.4 Beenden

WebSocket-Close-Frame (Status 1001) senden, kurz auslaufen lassen, TCP schließen.
(Würde man den Socket mit ungelesenen Daten einfach schließen, sendet Linux ein
TCP-RST und der Server verliert evtl. das Close-Frame – der PC-Test hat genau das
aufgedeckt, deshalb liest `kiwi_client_close()` noch ≤ 300 ms weiter.)

---

## 2 Architektur und Portabilität

```
 Netzwerk ──► kiwi_port.h ──► kiwi_ws (Frames) ──► kiwi_client (MSG/SND) ──► audio_sink_t
 (Sockets)    (OS-Schicht)    (reine Bytes)         (Zustandsautomat)         (WAV | DAC)
```

* **`kiwi_port.h`**: 6 Funktionen (connect/send/recv/close/Zeit/Zufall). Nur
  hier steckt Betriebssystemwissen. `port_sockets.c` nutzt die BSD-Socket-API,
  die es auf Linux/macOS *und* in lwIP gibt → eine Datei für beide Welten.
* **`kiwi_ws.c`**: kennt keine Sockets; Bytes rein → Nachrichten raus. Deshalb
  ist er ohne Netzwerk testbar (`pc/test_core.c`).
* **`kiwi_client.c`**: kennt weder Plattform noch Audiohardware; liefert PCM an
  eine `audio_sink_t` (Funktionszeiger-Struktur = „Strategy“-Muster).
* Kein `malloc` im Datenpfad, keine Gleitkomma-Arithmetik (Frequenzen als
  Festkomma-Hz), alle Puffer haben feste Größe (`KIWI_RX_BUF_SIZE`, Standard 4 KB),
  Tabellen sind `const` (liegen im Flash).
* Ein Thread/Task betreibt den Client (`poll`). Nur `kiwi_client_set_tune()`
  darf aus einem anderen Task aufgerufen werden (Übergabe per `atomic_int`).

---

## 3 PC: bauen und testen

Voraussetzung: `gcc`/`clang`, `make`, `python3`.

```sh
cd pc
make              # baut kiwi_pc und test_core
make test         # Kern-Selbsttests + End-to-End gegen lokalen Mock-Kiwi
```

**Gegen den echten KiwiSDR (immer genau eine Verbindung, wird sauber geschlossen):**

```sh
./kiwi_pc -f 225 -m am -t 20 -o pl225.wav url.kiwiserver.org
```

Optionen: `-p Port` · `-f kHz` · `-m Modus` · `-t Sekunden` (0 = bis Strg+C) ·
`-o datei.wav` (`-` = roh nach stdout) · `-c` ADPCM · `-w Passwort` ·
`-l` alter Pfad ohne `/ws/kiwi` (Kiwi < v1.9xx) ·
`-i` interaktiv (Befehle `f 1008`, `m usb`, `s`, `q`).

Live hören (Linux): `./kiwi_pc -f 225 -t 0 -o - HOST | aplay -r 12000 -f S16_LE -c 1`
(Die Statusanzeige geht nach stderr.)

Debug-Ausgabe aller Protokollnachrichten: `make kiwi_pc_debug`.

---

## 4 ESP32: bauen

```sh
. $IDF_PATH/export.sh            # ESP-IDF 6.1
idf.py set-target esp32
idf.py menuconfig                # Menü „KiwiSDR Client": WLAN, Host, Frequenz, Modus, Gain
idf.py build flash monitor
```

**Hardware:** Audio an **GPIO25** (DAC1) und **GPIO26** (DAC2, gleiches Signal).
Pegel 0…3,3 V um die Mittenspannung von 1,65 V. Für Verstärker/Kopfhörer:
10 µF in Reihe (entfernt den Offset) und ein RC-Tiefpass gegen die
Treppenstufen. Der DAC hat nur 8 Bit, daher ist das Quantisierungsrauschen leicht hörbar
(für AM-Sprache/Rundfunk brauchbar, aber kein HiFi).

**Bedienung über die serielle Konsole** (115200 Baud): `f 1008` · `m usb` · `s` · `q`.

**Technische Details:**
* Der Kiwi liefert 12 kHz; der DAC-DMA-Modus läuft erst ab ≈ 19,6 kHz. Daher
  wird automatisch um den Faktor 2 hochgerechnet (lineare Interpolation auf 24 kHz).
* Jitterpuffer (Standard 1 s) mit Vorpufferung (400 ms); bei Unterlauf gibt es Stille und
  erneutes Vorpuffern; die Zähler werden alle 10 s geloggt.
* Wiederverbinden mit Wartezeit 5 → 10 → 20 → 40 → 60 s, um den Server nicht zu
  „hämmern“. Zuletzt eingestellte Frequenz und Modus bleiben erhalten.

---

## 5 Was getestet wurde – und was nicht

**Gegen den echten Kiwi v1.902 (Browser-Mitschnitt `browser.pcap`):**
* Der Browser-Mitschnitt wurde Frame für Frame ausgewertet (Pfad, Header, alle
  `SET`-Befehle und `MSG`-Antworten). Daraus stammen Pfadpräfix, `p=#`, `Origin`
  und `SERVER DE CLIENT`.
* Die aufgezeichneten Server-Frames wurden an den C-Client zurückgespielt:
  keine Lücken, 12000 Hz erkannt, Ausgabe **identisch** zur
  Referenz-Dekodierung des Python-Dekoders.
* Der C-ADPCM-Dekoder und der Python-Dekoder liefern auf den Browser-Daten
  bitgleiche Ergebnisse.

**Getestet (Linux, `make test`):**
* ADPCM-Dekoder **bitgleich** gegen den Python-Dekoder aus `kiwiclient`
  (48 Referenz-Samples inkl. Sättigung).
* WebSocket-Parser: Byte-für-Byte-Fütterung, 16-Bit-Längen, Fragmentierung mit
  eingeschobenem Ping, Überlänge, Protokollfehler, Maskierung.
* Ende-zu-Ende gegen einen **lokalen Mock-Kiwi** (`pc/mock_kiwi.py`, implementiert
  das oben beschriebene Protokoll): Anmeldung, `AR OK`, Abstimmung
  `SET mod=am … freq=225.000`, rohes PCM und ADPCM (1-kHz-Ton wird korrekt
  erkannt), 20-KB-`MSG` wird übersprungen, Ping/Pong, Abstimmen zur Laufzeit,
  `too_busy`-Fehlerfall, sauberes Schließen in allen Fällen.

**Live-Verbindungen** des Clients zu mehreren echten KiwiSDR-Servern
(verschiedene Firmware-Versionen, Standorte, Auslastung) verliefen erfolgreich.

**Bekannte Grenzen:** nur Mono-Modi (Stereo/IQ/DRM werden abgelehnt), kein TLS
(`ws://` statt `wss://`), kein URL-Encoding des Passworts, Taktdrift Kiwi↔DAC wird nur
durch den Puffer (Überlaufzähler) abgefangen, nicht aktiv nachgeregelt.
Nur KiwiSDR wird unterstützt; WebSDR-Server verwenden ein anderes Protokoll.

---

## 6 Zielgruppe und Motivation

Für den PC gibt es bereits genügend Clients für KiwiSDR und WebSDR (Browser,
`kiwiclient`, diverse Desktop-Programme). Dieses Projekt richtet sich bewusst an
einen anderen Bereich: **Embedded-Geräte, die ohne Betriebssystem-Ballast einen
entfernten SDR als „Antenne und Empfänger aus dem Netz“ nutzen sollen.**

Typische Anwendungen:

* **Upcycling und Retrofit** alter Röhren-, Transistor- oder Weltempfänger-Gehäuse,
  die statt eines eigenen HF-Teils einen entfernten KiwiSDR anzapfen
  (siehe z. B. das
  [element14-UpCycleIT-Projekt „Embedded Web-SDR Client on Analog Radio Receiver“](https://community.element14.com/challenges-projects/design-challenges/upcycleit/b/blog/posts/upcycle-it-design-challenge-embedded-web-sdr-client-on-analog-radio-receiver-1-introduction)).
* **Stromsparende Dauerläufer**, z. B. Küchen-, Werkstatt- oder Nachttisch-Radio,
  Batterie- oder Solarbetrieb.
* **Lehr- und Bastelprojekte** rund um Funk, WebSockets und Embedded-Audio mit
  überschaubarem, gut lesbarem C-Code.
* **Empfang an Orten ohne eigene gute Antenne** (Mietwohnung, Funkloch-Lagen), mit
  ausgewähltem Kiwi in einer rauscharmen Gegend.

**Warum kein Raspberry Pi oder Jetson mit Headless-Browser?**
Der übliche Weg, einen Web-SDR in ein Gerät einzubauen, ist ein Einplatinenrechner
mit einem (Headless-)Browser. Für die Aufgabe „Audio-Strom empfangen und ausgeben“
ist das stark überdimensioniert: Linux-Boot, mehrere hundert MB RAM, hoher
Strombedarf und lange Startzeiten, dazu Wartungsaufwand (Updates, SD-Karten-Verschleiß).
Ein ESP32 (oder eine vergleichbare MCU mit WLAN) erledigt dieselbe Aufgabe mit
wenigen hundert KB RAM, startet in Sekunden und braucht typischerweise nur einen
Bruchteil der Leistung.

**Abgrenzung – was das Projekt nicht sein will:**
Kein Ersatz für die komplette Web-Oberfläche (kein Wasserfall, keine DX-Liste,
keine breitbandige IQ-Auswertung). Es geht um Audio eines einzelnen Kanals.

---

## 7 TODO

* [ ] **Automatischer Abgleich mit einem Verzeichnisdienst.**
  Der Client soll regelmäßig eine öffentliche KiwiSDR-Liste abrufen und daraus
  eine kleine interne Datenbank mit mehreren Kiwi-Servern pflegen
  (z. B. in NVS/SPIFFS auf dem ESP32, Datei auf dem PC). Denkbare Inhalte pro Eintrag:
  Host/Port, Standort, Frequenzbereich, freie Slots, Signalqualität, zuletzt erreichbar.
  Nutzen: automatischer Wechsel zu einem anderen Kiwi bei `too_busy`/`down`,
  Auswahl nach Standort oder Frequenzbereich, kein manuelles Pflegen der Hostliste.
  Zu beachten: feste Anzahl von Einträgen (kein `malloc`), sparsame Abruf-Intervalle,
  schlanker Parser statt vollständigem JSON-Baum, Fallback auf die zuletzt
  gespeicherte Liste bei fehlendem Verzeichnisdienst.
