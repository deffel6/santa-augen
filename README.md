# 🎅 Santa-Augen

Freundliche Weihnachtsmann-Augen auf zwei runden GC9A01-Displays (240×240) mit einem ESP32-C3 Super Mini. Die Augen schauen umher, blinzeln, lächeln, funkeln und rollen alle 30 Sekunden kurz mit den Augen. Gesteuert wird per Handy im WLAN; optional folgen sie Personen mit einem HLK-LD2450-Radar.

**Web-Installer:** https://deffel6.github.io/santa-augen/ (Chrome oder Edge am Computer)

## Hardware

- ESP32-C3 Super Mini
- 2× GC9A01 rund 240×240
- optional HLK-LD2450 (24-GHz-Radar)

| GC9A01 | Linkes Auge | Rechtes Auge |
|---|---|---|
| VCC / GND | 3V3 / GND | 3V3 / GND |
| SCL | GPIO 6 | GPIO 6 |
| SDA | GPIO 7 | GPIO 7 |
| DC | GPIO 2 | GPIO 2 |
| RES | GPIO 1 | GPIO 1 |
| BLK | GPIO 3 | GPIO 3 |
| CS | **GPIO 4** | **GPIO 10** |

| LD2450 | ESP32-C3 |
|---|---|
| 5V / GND | 5V / GND |
| TX | GPIO 20 |
| RX | GPIO 21 |

Die Displays sind um 90° gedreht eingebaut (`ROT_LINKS 3`, `ROT_RECHTS 1`). Andere Einbaulage: diese Werte im Sketch ändern.

## Erster Start

1. Ohne gespeichertes WLAN öffnet der C3 das WLAN **„Santa-Augen“** (Passwort `hohohoho`).
2. Verbinden, `http://192.168.4.1` öffnen, „WLAN einrichten“.
3. Danach: `http://santa.local`

## Steuerseite

Blick per Touch-Feld, Blinzeln, Zwinkern, Lächeln, Funkeln, Herzchen, Ho Ho Ho, Schlafen/Aufwachen, Automatik, Helligkeit, Test (zeigt LINKS/RECHTS und Blickrichtungen) und eine Radar-Karte mit Erfassungszone, Einschlafzeit, „Links/rechts tauschen“ und „Mitte setzen“.

„Mitte setzen“ übernimmt nur, wenn genau eine Person stabil erkannt wird und die Korrektur höchstens ±20° beträgt – sonst würde eine Reflexion das Sichtfeld verdrehen.

## Updates ohne Kabel (OTA)

- Browser: `http://santa.local/update` → `santa_augen_c3.ino.bin` hochladen (Arduino IDE: Sketch → „Kompilierte Binärdatei exportieren“).
- Arduino IDE: Werkzeuge → Port → Netzwerk-Port „santa“.
- Kommandozeile: `curl -H 'Content-Type: application/octet-stream' --data-binary @santa_augen_c3.ino.bin 'http://santa.local/update?pw=santa-ota'`

Das Passwort steht in `OTA_PASS` (Standard `santa-ota`). Für ein eigenes Passwort den Sketch ändern und selbst bauen.

## Selbst bauen

Arduino IDE oder `arduino-cli` mit Arduino-ESP32 3.3.x und der Bibliothek **LovyanGFX**:

```
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=default src/santa_augen_c3
```

Wichtige Einstellungen oben im Sketch: Pins, Drehung, `SANTA_SKIN` / `SANTA_BROWS` (Haut, Brauen), Hautfarben in `santa_eye.h` (`SK_`, `CR_`, `LA_`, `CH_`), Blick-Kennlinie `FOLGE_KURVE`, Augenrollen `ROLL_ALLE_MS` / `ROLL_MS` / `ROLL_RUNDEN`.

Die Firmware belegt gut 90 % des App-Bereichs von „Default 4MB with spiffs“. Wird sie größer: „Minimal SPIFFS (1.9MB APP with OTA)“ wählen (einmal per USB flashen).

## Inhalt

- `src/santa_augen_c3/` – Sketch und Augen-Renderer
- `index.html`, `manifest.json`, `firmware/` – Web-Installer (ESP Web Tools), über GitHub Pages veröffentlicht
