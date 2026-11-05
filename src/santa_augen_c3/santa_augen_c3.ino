// =====================================================================
//  santa_augen_c3 – freundliche Weihnachtsmann-Augen auf 2x GC9A01
// =====================================================================
//  Hardware : ESP32-C3 Super Mini + 2x GC9A01 240x240 rund
//  Board    : "ESP32C3 Dev Module" (Arduino IDE)
//  Bibliothek: LovyanGFX
//
//  Zwei Augen, die gemeinsam umherschauen, blinzeln, ab und zu
//  laecheln (Lachfalten, Brauen gehen hoch) und dabei funkeln.
//  Haut und rote Baeckchen (SANTA_SKIN), Brauen abschaltbar (SANTA_BROWS).
//
//  Steuerung per Handy im WLAN:  http://santa.local
//    - Touch-Feld: Augen schauen dorthin, wo du tippst
//    - Blinzeln, Zwinkern links/rechts, Laecheln, Funkeln, Ho Ho Ho,
//      Schlafen/Aufwachen, Automatik an/aus, Helligkeit
//  Erster Start (noch kein WLAN gespeichert): Der C3 macht ein eigenes
//  WLAN "Santa-Augen" (Passwort: hohohoho) auf. Damit verbinden,
//  http://192.168.4.1 oeffnen -> "WLAN einrichten".
//
//  Update ohne Kabel (OTA), Passwort siehe OTA_PASS:
//    - Arduino IDE: Werkzeuge -> Port -> Netzwerk-Port "santa" waehlen,
//      dann wie gewohnt hochladen (Mac im selben WLAN).
//    - Browser: http://santa.local/update (bzw. 192.168.4.1/update) ->
//      .bin-Datei waehlen (IDE: Sketch -> "Kompilierte Binaerdatei exportieren").
//  Partitionsschema mit OTA noetig, z. B. "Default 4MB with spiffs".
//
//  Radar HLK-LD2450 (optional): die Augen folgen der naechsten Person,
//  er schlaeft ein, wenn lange niemand da ist, und laechelt, wenn man
//  nah herankommt. 5V -> 5V, GND -> GND, TX -> GPIO 20, RX -> GPIO 21.
//
//  Verdrahtung (an BEIDEN Displays parallel):
//    VCC -> 3V3   GND -> GND   SCL -> GPIO 6   SDA -> GPIO 7
//    DC  -> GPIO 2             RST -> GPIO 1   BLK -> GPIO 3 (falls vorhanden)
//  Getrennt:
//    CS Display 1 (links)  -> GPIO 4
//    CS Display 2 (rechts) -> GPIO 10
// =====================================================================

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>

// Status-Ausgaben nur ueber den USB-Anschluss des C3 (Werkzeuge ->
// "USB CDC On Boot: Enabled"). Ist das aus, gibt es keine Ausgaben –
// GPIO 20/21 (sonst die serielle Schnittstelle) bleiben so immer frei
// fuer das Radar.
#if ARDUINO_USB_CDC_ON_BOOT
  #define LOG Serial
  #define LOG_NONBLOCKING() Serial.setTxTimeoutMs(0)   // nie warten, wenn am USB keiner mitliest
#else
  #define LOG_NONBLOCKING()
  struct NullLog { void begin(unsigned long) {} int printf(const char*, ...) { return 0; } } nullLog;
  #define LOG nullLog
#endif

#define SANTA_SKIN 1          // 1 = Haut um das Auge, 0 = schwarzer Hintergrund
#define SANTA_BROWS 0         // 1 = weisse buschige Augenbrauen, 0 = keine
#include "santa_eye.h"

// ---------------- Einstellungen ----------------
#define PIN_SCLK  6
#define PIN_MOSI  7
#define PIN_DC    2
#define PIN_RST   1
#define PIN_BL    3   // Display ohne BLK-Pin: egal, dann leuchtet es immer
#define PIN_CS1   4   // Display 1 (links)  – beim Einbau getauscht, war 10
#define PIN_CS2  10   // Display 2 (rechts) – beim Einbau getauscht, war 4

#define NUM_EYES    2
#define SPI_MHZ     80    // Display-Takt: 80 = schnell; bei Bildfehlern/Streifen 40 nehmen
// Bilddrehung je Display (LovyanGFX setRotation): 0 = normal,
// 1 = 90 Grad nach rechts, 2 = 180 Grad, 3 = 90 Grad nach links
#define ROT_LINKS   3     // Display 1 (links):  90 Grad nach links
#define ROT_RECHTS  1     // Display 2 (rechts): 90 Grad nach rechts
#define HELLIGKEIT  255
#define PUPIL_R     24    // Grundgroesse Pupille (gross = freundlich)
#define LID_OFFEN   0.80f // Oberlid: 1.0 = ganz offen; 0.76 = deckt die Iris oben etwas ab (natuerlicher)
#define GRUND_LACHEN 0.12f// leichtes Dauerlaecheln (0 = keins)
#define LOOK_X      30    // max. Blickauslenkung horizontal (Automatik)
#define LOOK_Y      12    // max. Blickauslenkung vertikal (Automatik)
#define MANUELL_MS  5000  // so lange pausiert die Automatik nach einer Eingabe

// Radar HLK-LD2450
#define RADAR_RX      20     // LD2450 TX -> GPIO 20
#define RADAR_TX      21     // LD2450 RX -> GPIO 21
#define RADAR_BAUD    256000
#define RADAR_WINKEL  25.0f  // ab diesem Winkel (Grad) schauen die Augen ganz zur Seite
#define RADAR_MAX_MM  6000   // weiter entfernte Ziele ignorieren
#define RADAR_NAH_MM  1200   // naeher als das -> Laecheln + Funkeln
#define RADAR_SCHLAF_S 30    // Standard: so viele Sekunden niemand da -> einschlafen (0 = nie)
#define SCHLAF_HELL   0.06f  // Bildhelligkeit im Schlaf (0 = schwarz, 1 = normal)
#define AUFWACH_MS    250    // so schnell schlagen die Augen auf
#define EINSCHLAF_MS  1200   // so langsam fallen sie zu und das Bild wird dunkel
#define RADAR_STABIL  3      // so viele Messungen in Folge, bis ein Ziel zaehlt (~0,1 s je Messung)
#define RADAR_GLAETTUNG 0.7f // 0..1: kleiner = ruhiger, groesser = direkter
#define FOLGE_MS      35     // Reaktionszeit der Augen beim Folgen (kleiner = schneller)
#define FOLGE_PX      50     // so weit (Pixel) gehen die Augen beim Folgen maximal zur Seite
#define FOLGE_KURVE   2.5f   // Blick-Kennlinie: 1 = linear, groesser = Mitte ruhiger, Seite staerker
                             // (2.5: 10 Grad -> 10 %, 15 Grad -> 28 %, 20 Grad -> 57 %, 25 Grad -> 100 %)
#define FOLGE_NAH_PX  10     // so weit schauen sie nach unten, wenn jemand ganz nah ist
#define BLICK_MS      90     // Reaktionszeit bei normalen Blicksprüngen

#define HOSTNAME    "santa"        // -> http://santa.local
// Viele ESP32-C3 Super Mini haben eine schwache Antennenanpassung und
// funken bei voller Leistung schlechter; 8,5 dBm ist dort zuverlaessiger.
#define WIFI_TX     WIFI_POWER_8_5dBm
#define AP_SSID     "Santa-Augen"
#define AP_PASS     "hohohoho"     // mind. 8 Zeichen
#define OTA_PASS    "santa-ota"    // Passwort fuer Updates per IDE und /update – fuer den eigenen Build aendern

// ---------------- Display-Konfiguration ----------------
// CS wird von Hand geschaltet (pin_cs = -1), damit ein Display-Objekt
// beide Displays nacheinander bedienen kann.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX() {
    { auto c = _bus.config(); c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = SPI_MHZ * 1000000; c.freq_read = 16000000;
      c.spi_3wire = true; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = PIN_SCLK; c.pin_mosi = PIN_MOSI; c.pin_miso = -1; c.pin_dc = PIN_DC;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config(); c.pin_cs = -1; c.pin_rst = PIN_RST; c.pin_busy = -1;
      c.panel_width = 240; c.panel_height = 240; c.readable = false;
      c.invert = true; c.rgb_order = false;
      _panel.config(c); }
    { auto c = _light.config(); c.pin_bl = PIN_BL; c.invert = false;
      c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};

LGFX tft;

#define STRIP_H 60     // kleine Streifen sparen RAM fuers WLAN
uint16_t* stripBuf[2];

const uint8_t csPin[2] = {PIN_CS1, PIN_CS2};
const uint8_t eyeRot[2] = {ROT_LINKS, ROT_RECHTS};

void selectEye(int e) {   // e = -1: keins, 99: beide
  for (int i = 0; i < 2; i++)
    digitalWrite(csPin[i], (e == 99 || e == i) ? LOW : HIGH);
}

// ---------------- Animationszustand ----------------
float lookX = 0, lookY = 0, targetX = 0, targetY = 0;
float pupil = PUPIL_R, pupilTarget = PUPIL_R;
uint32_t nextSaccade = 0, nextPupil = 0;

uint32_t blinkStart = 0, nextBlink = 2500;
uint16_t blinkDur = 180;
bool     doubleBlink = false;

int8_t   winkEye = -1;        // Zwinkern: 0 = links, 1 = rechts
uint32_t winkStart = 0;
#define  WINK_MS 450

// Laecheln: 0 = aus, 1 = wird mehr, 2 = halten, 3 = wird weniger
uint8_t  smileState = 0;
uint32_t smileT0 = 0, smileHold = 0, nextSmile = 5000;
float    smilePeak = 0.4f, smile = GRUND_LACHEN;

uint32_t twinkleT0 = 0, nextTwinkle = 9000;
#define TWINKLE_MS 700

bool     autoMode = true;
bool     sleeping = false;
float    lidOpen = LID_OFFEN;
uint32_t manualUntil = 0;
uint32_t hohoUntil = 0;

// Augenrollen: alle ROLL_ALLE_MS einmal kurz im Kreis (beide Augen gleich)
#define  ROLL_ALLE_MS 30000
#define  ROLL_MS      3000    // Dauer
#define  ROLL_RUNDEN  2.0f    // so viele Kreise in dieser Zeit
uint32_t rollStart = 0, nextRoll = 30000;
uint8_t  brightness = HELLIGKEIT;
uint32_t msgUntil = 0;        // solange wird eine Textmeldung gezeigt

// ---------------- Test beim Start ----------------
// Zeigt auf jedem Display, ob es das linke oder rechte Auge ist, und laesst
// die Augen dann nach links, rechts, oben und unten schauen (mit Beschriftung).
// Ueber die Webseite ("Test") jederzeit wiederholbar.
bool     testPending = true;  // startet, sobald die WLAN-Meldung weg ist
uint32_t testStart = 0;       // 0 = kein Test aktiv
struct TestStep { uint16_t until; int8_t x, y; const char* text; };
const TestStep TEST_STEPS[] = {
  {2500,   0,  0, nullptr},     // nur Beschriftung LINKS / RECHTS
  {4000,  -1,  0, "schaut LINKS"},
  {5500,   1,  0, "schaut RECHTS"},
  {6700,   0, -1, "schaut OBEN"},
  {7900,   0,  1, "schaut UNTEN"},
  {9000,   0,  0, "Mitte - fertig"},
};
const int TEST_N = sizeof(TEST_STEPS) / sizeof(TEST_STEPS[0]);
int      testStep = -1;       // aktueller Schritt, -1 = kein Test

LGFX_Sprite overlay;          // zeichnet Text direkt in den Streifenpuffer

// Nur die Zeilen um die Augenoeffnung aendern sich (Lider, Iris, Laecheln);
// Haut darueber und darunter bleibt stehen -> deutlich mehr Bilder/s.
#if SANTA_BROWS
const int DIRTY_Y0 = 0, DIRTY_Y1 = 240;   // Brauen bewegen sich -> alles zeichnen
#else
const int DIRTY_Y0 = santa::CY - (int)santa::UP_H - 8;
const int DIRTY_Y1 = santa::CY + (int)santa::LO_H + 8;
#endif
bool fullRedraw[2] = {true, true};

// ---------------- Herzchen beim Aufwachen ----------------
// Kleine Herzen tauchen nacheinander auf, wachsen, steigen etwas hoch und
// verschwinden wieder; das Ganze dauert HERZ_MS.
#define HERZ_MS     9000
#define HERZ_ANZAHL 16
#define HERZ_LEBEN  3400   // so lange lebt ein einzelnes Herz (ms)
struct Heart { int16_t x, y; uint16_t t0; uint8_t size; uint16_t col; };
Heart    hearts[HERZ_ANZAHL];
uint32_t heartsStart = 0;     // 0 = keine Herzen
// Aufwachen: erst Herzchen im dunklen Bild, danach gehen die Augen auf
#define WECK_NACH_MS (HERZ_MS - 500)
uint32_t wakeAt = 0;          // 0 = kein Aufwachen geplant
void drawHeart(LGFX_Sprite& g, int cx, int cy, int size, uint16_t col);

void startHearts(uint32_t now) {
  static const uint8_t COLS[][3] = {{235, 40, 70}, {255, 90, 130}, {220, 20, 50}, {255, 140, 170}};
  for (int i = 0; i < HERZ_ANZAHL; i++) {
    Heart& h = hearts[i];
    h.x = random(45, 196);
    h.y = random(60, 215);
    h.t0 = i * (HERZ_MS - HERZ_LEBEN) / HERZ_ANZAHL + random(0, 120);
    h.size = random(40, 80);
    const uint8_t* c = COLS[random(0, 4)];
    h.col = lgfx::color565(c[0], c[1], c[2]);
  }
  heartsStart = now ? now : 1;
}

static float ease(float t) { t = constrain(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); }

void doBlink(uint32_t now) {
  if (!blinkStart) { blinkStart = now; blinkDur = random(150, 230); }
}
void doSmile(uint32_t now, float peak, uint32_t hold) {
  smileState = 1; smileT0 = now; smilePeak = peak; smileHold = hold;
}
void startTwinkle(uint32_t now) {
  twinkleT0 = now;
  nextTwinkle = now + random(8000, 20000);
}

// ---------------- Radar HLK-LD2450 ----------------
// Datenrahmen (30 Byte, ca. 10x pro Sekunde):
//   AA FF 03 00 | 3x Ziel je 8 Byte (X, Y, Geschwindigkeit, Aufloesung) | 55 CC
// Werte: int16 little endian, oberstes Bit 1 = positiv, 0 = negativ; X/Y in mm.
struct RadarTarget { int16_t x, y; bool valid; };
RadarTarget radarT[3];
uint8_t  rbuf[30];
uint8_t  rlen = 0;
uint32_t radarMaxGap = 0, radarBadTail = 0, radarLoopMax = 0;
uint32_t radarLastFrame = 0, radarLastSeen = 0, radarFrameCount = 0;
int      radarBest = -1;
int      radarCount = 0;
float    radarAngle = 0, radarDist = 0;   // Grad (+ = rechts vom Sensor), mm
bool     followMode = true, radarFlip = false;
float    radarOffset = 0;      // Grad; per Webseite "Mitte setzen" kalibriert
#define  RADAR_KALIB_MAX 20.0f // groessere Korrektur wird abgelehnt (Reflexion statt Person)
String   calMsg;               // Rueckmeldung von "Mitte setzen" fuer die Webseite
int      schlafS = RADAR_SCHLAF_S;   // per Webseite einstellbar
float    dimLevel = 1.0f;      // Bildhelligkeit 0..1 (Schlaf = dunkel)
// Filter gegen Stoerungen (per Webseite einstellbar, gespeichert)
bool     radarSingle = true;   // Einzelziel-Modus des LD2450
int      zoneMin = 500, zoneMax = 4000, zoneW = 2000;   // mm: Abstand von/bis, seitlich +/-
uint8_t  radarStab[3];         // Messungen in Folge je Ziel
bool     radarOk[3];           // Ziel gueltig, in der Zone und stabil
bool     radarTracking = false, autoSleeping = false, wasNear = false;
uint32_t nearCooldown = 0;

static int16_t ldVal(uint8_t lo, uint8_t hi) {
  uint16_t r = lo | (hi << 8);
  int16_t v = r & 0x7FFF;
  return (r & 0x8000) ? v : -v;
}

void radarParse() {
  if (rbuf[28] != 0x55 || rbuf[29] != 0xCC) { radarBadTail++; return; }
  radarFrameCount++;
  uint32_t nowF = millis();
  if (radarLastFrame && nowF - radarLastFrame > radarMaxGap) radarMaxGap = nowF - radarLastFrame;
  radarLastFrame = nowF;
  for (int i = 0; i < 3; i++) {
    const uint8_t* t = &rbuf[4 + i * 8];
    bool any = false;
    for (int k = 0; k < 8; k++) if (t[k]) any = true;
    int16_t x = ldVal(t[0], t[1]), y = ldVal(t[2], t[3]);
    // Kalibrierung: Sensor-Schraeglage herausdrehen (Mitte = 0 Grad)
    float c = cosf(radarOffset * DEG_TO_RAD), sn = sinf(radarOffset * DEG_TO_RAD);
    int16_t nx = (int16_t)(x * c - y * sn), ny = (int16_t)(x * sn + y * c);
    bool valid = any && ny > 0 && ny <= RADAR_MAX_MM;
    // Stabilitaet: zaehlt erst nach mehreren Messungen in Folge; ein Sprung
    // um mehr als 60 cm (typisch fuer Geisterziele) faengt wieder bei 1 an
    if (!valid) radarStab[i] = 0;
    else if (radarStab[i] && (abs(nx - radarT[i].x) > 600 || abs(ny - radarT[i].y) > 600)) radarStab[i] = 1;
    else if (radarStab[i] < 250) radarStab[i]++;
    radarT[i].x = nx; radarT[i].y = ny;
    radarT[i].valid = valid;
    bool inZone = ny >= zoneMin && ny <= zoneMax && abs(nx) <= zoneW;
    radarOk[i] = valid && inZone && radarStab[i] >= RADAR_STABIL;
  }
}

// LD2450-Befehl senden: FD FC FB FA | Laenge | Befehl (+Wert) | 04 03 02 01
void radarCmd(uint16_t cmd, const uint8_t* val, uint8_t vlen) {
  const uint8_t head[4] = {0xFD, 0xFC, 0xFB, 0xFA}, tail[4] = {0x04, 0x03, 0x02, 0x01};
  uint16_t len = 2 + vlen;
  Serial1.write(head, 4);
  Serial1.write((uint8_t)(len & 0xFF)); Serial1.write((uint8_t)(len >> 8));
  Serial1.write((uint8_t)(cmd & 0xFF)); Serial1.write((uint8_t)(cmd >> 8));
  if (vlen) Serial1.write(val, vlen);
  Serial1.write(tail, 4);
  Serial1.flush();
  delay(60);
}

// Einzel- oder Mehrziel-Modus einstellen (Konfiguration oeffnen, setzen, schliessen)
void radarSetMode(bool single) {
  const uint8_t one[2] = {0x01, 0x00};
  radarCmd(0x00FF, one, 2);              // Konfigurationsmodus an
  radarCmd(single ? 0x0080 : 0x0090, nullptr, 0);
  radarCmd(0x00FE, nullptr, 0);          // Konfigurationsmodus aus
  LOG.printf("[RADAR] %s-Modus gesetzt\n", single ? "Einzelziel" : "Mehrziel");
}

void radarRead() {
  static const uint8_t HDR[4] = {0xAA, 0xFF, 0x03, 0x00};
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    if (rlen < 4) {                       // auf Rahmenanfang synchronisieren
      if (b == HDR[rlen]) rbuf[rlen++] = b;
      else { rlen = (b == 0xAA) ? 1 : 0; rbuf[0] = 0xAA; }
      continue;
    }
    rbuf[rlen++] = b;
    if (rlen == 30) { radarParse(); rlen = 0; }
  }
}

void radarUpdate(uint32_t now) {
  radarRead();

  // naechste Person waehlen; die bisherige behalten, solange sie nicht
  // deutlich weiter weg ist als eine andere (sonst springen die Augen)
  int best = -1; float bestD = 1e9;
  radarCount = 0;
  for (int i = 0; i < 3; i++) {
    if (!radarOk[i]) continue;
    radarCount++;
    float d = sqrtf((float)radarT[i].x * radarT[i].x + (float)radarT[i].y * radarT[i].y);
    if (i == radarBest) d *= 0.75f;
    if (d < bestD) { bestD = d; best = i; }
  }
  static uint32_t seenFrames = 0;
  static int lastBest = -1;
  if (best >= 0 && radarFrameCount != seenFrames) {   // neuer Messwert
    const bool lost = !radarLastSeen || now - radarLastSeen > 1500;
    radarLastSeen = now;
    float a = atan2f(radarT[best].x, radarT[best].y) * RAD_TO_DEG;
    float d = sqrtf((float)radarT[best].x * radarT[best].x + (float)radarT[best].y * radarT[best].y);
    if (best != lastBest || lost) { radarAngle = a; radarDist = d; }
    else {                                 // Messrauschen glaetten, Bewegung bleibt flott
      radarAngle += (a - radarAngle) * RADAR_GLAETTUNG;
      radarDist  += (d - radarDist)  * RADAR_GLAETTUNG;
    }
  }
  seenFrames = radarFrameCount;
  lastBest = best;
  radarBest = best;
  const bool present = radarLastSeen && now - radarLastSeen < 1500;
  radarTracking = followMode && present && !sleeping;

  if (!followMode) { wasNear = false; return; }

  if (present) {
    if (autoSleeping && !wakeAt) {        // jemand kommt -> erst Herzchen, dann aufwachen
      startHearts(now);
      wakeAt = now + WECK_NACH_MS;
    }
    if (radarTracking && now >= manualUntil) {
      float a = radarFlip ? -radarAngle : radarAngle;
      float n = constrain(a / RADAR_WINKEL, -1.0f, 1.0f);
      // progressiv: um die Mitte kaum Bewegung, erst deutlich seitlich weit zur Seite
      n = copysignf(powf(fabsf(n), FOLGE_KURVE), n);
      targetX = n * FOLGE_PX;
      // nah = etwas nach unten schauen (auf die Person), weit weg = geradeaus
      targetY = constrain((2000.0f - radarDist) / 1500.0f, 0.0f, 1.0f) * FOLGE_NAH_PX - 2;
    }
    // nah dran -> laecheln und funkeln (mit Pause dazwischen)
    bool near = radarDist < (wasNear ? RADAR_NAH_MM + 200 : RADAR_NAH_MM);
    if (near && !wasNear && now >= nearCooldown && !sleeping) {
      doSmile(now, 0.42f, 2200);
      twinkleT0 = now + 250;
      nearCooldown = now + 10000;
    }
    wasNear = near;
  } else {
    wasNear = false;
    if (schlafS && !sleeping && radarFrameCount &&
        now - radarLastSeen > schlafS * 1000UL) {
      sleeping = true; autoSleeping = true; smileState = 0;
    }
  }
}

// geplantes Aufwachen ausfuehren: Augen auf, kurz freuen
void serviceWake(uint32_t now) {
  if (!wakeAt || now < wakeAt) return;
  wakeAt = 0;
  sleeping = false; autoSleeping = false;
  radarLastSeen = now;
  nextBlink = now + 900;
  doSmile(now, 0.35f, 1500);
}

santa::Params updateAnimation(uint32_t now) {
  const bool autoActive = autoMode && now >= manualUntil && !sleeping;

  // ruhige Blickwechsel, meistens ungefaehr geradeaus
  if (autoActive && !radarTracking && now >= nextSaccade) {
    if (random(100) < 55) {
      targetX = random(-6, 7);
      targetY = random(-3, 4);
    } else {
      float a = random(0, 360) * DEG_TO_RAD;
      float r = random(40, 101) / 100.0f;
      targetX = cosf(a) * r * LOOK_X;
      targetY = sinf(a) * r * LOOK_Y;
    }
    nextSaccade = now + random(800, 3500);
  }
  // gleich schnell, egal wie viele Bilder pro Sekunde gerade gezeichnet werden
  static uint32_t lastAnim = 0;
  float dt = lastAnim ? (now - lastAnim) : 16;
  lastAnim = now;
  float k = 1.0f - expf(-dt / (radarTracking ? FOLGE_MS : BLICK_MS));
  lookX += (targetX - lookX) * k;
  lookY += (targetY - lookY) * k;

  // Augenrollen: nicht im Schlaf, beim Test oder waehrend jemand per Handy steuert
  float eyeX = lookX, eyeY = lookY;
  if (!rollStart && now >= nextRoll) {
    if (!sleeping && !testStart && now >= manualUntil) { rollStart = now; nextBlink = max(nextBlink, now + ROLL_MS); }
    else nextRoll = now + 2000;             // gleich nochmal versuchen
  }
  if (rollStart) {
    float t = (now - rollStart) / (float)ROLL_MS;
    if (t >= 1.0f) { rollStart = 0; nextRoll = now + ROLL_ALLE_MS; }
    else {
      float w = ease(min(t, 1.0f - t) / 0.15f);          // weich hinein und wieder heraus
      float a = -HALF_PI + t * ROLL_RUNDEN * TWO_PI;     // oben beginnen
      eyeX += (cosf(a) * LOOK_X * 1.2f - eyeX) * w;
      eyeY += (sinf(a) * LOOK_Y * 1.6f - eyeY) * w;
    }
  }

  // Pupille atmet leicht
  if (now >= nextPupil) {
    pupilTarget = PUPIL_R + random(-3, 5);
    nextPupil = now + random(400, 1500);
  }
  pupil += (pupilTarget - pupil) * 0.08f;

  // Blinzeln (auch im Handbetrieb, nur nicht im Schlaf)
  float blink = 1.0f;
  if (!sleeping && now >= nextBlink) doBlink(now);
  if (blinkStart) {
    float t = (now - blinkStart) / (float)blinkDur;
    if (t >= 1.0f) {
      blinkStart = 0;
      if (!doubleBlink && random(100) < 20) { doubleBlink = true; nextBlink = now + 120; }
      else { doubleBlink = false; nextBlink = now + random(2000, 6000); }
    } else {
      blink = (t < 0.4f) ? 1.0f - t / 0.4f : (t - 0.4f) / 0.6f;
    }
  }

  // Schlafen: Lider langsam zu / wieder auf
  {
    float tau = sleeping ? EINSCHLAF_MS : AUFWACH_MS;
    float k = 1.0f - expf(-dt / tau);
    lidOpen  += ((sleeping ? 0.0f : LID_OFFEN) - lidOpen) * k;
    // Bild wird erst dunkel, wenn die Lider fast zu sind; beim Aufwachen sofort hell
    float dimTarget = sleeping ? (lidOpen < 0.15f ? SCHLAF_HELL : 0.6f) : 1.0f;
    dimLevel += (dimTarget - dimLevel) * (1.0f - expf(-dt / (sleeping ? 500.0f : 150.0f)));
  }

  // Laecheln mit Lachfalten
  switch (smileState) {
    case 0:
      smile = GRUND_LACHEN;
      if (autoActive && now >= nextSmile) {
        doSmile(now, random(30, 46) / 100.0f, random(1500, 3500));
        targetX = random(-4, 5); targetY = random(-4, 1);  // dabei freundlich geradeaus
        twinkleT0 = now + 250;
      }
      break;
    case 1:
      smile = GRUND_LACHEN + (smilePeak - GRUND_LACHEN) * ease((now - smileT0) / 500.0f);
      if (now - smileT0 >= 500) { smileState = 2; smileT0 = now; }
      break;
    case 2:
      smile = smilePeak;
      if (now - smileT0 >= smileHold) { smileState = 3; smileT0 = now; }
      break;
    case 3:
      smile = smilePeak - (smilePeak - GRUND_LACHEN) * ease((now - smileT0) / 700.0f);
      if (now - smileT0 >= 700) { smileState = 0; nextSmile = now + random(6000, 15000); }
      break;
  }

  // Funkeln
  if (autoActive && now >= nextTwinkle && now > twinkleT0 + TWINKLE_MS) startTwinkle(now);
  int tw = 0;
  if (twinkleT0 && now >= twinkleT0 && now < twinkleT0 + TWINKLE_MS) {
    float t = (now - twinkleT0) / (float)TWINKLE_MS;
    tw = (int)(256 * (t < 0.3f ? t / 0.3f : 1.0f - (t - 0.3f) / 0.7f));
  }

  // Ho Ho Ho: Augen huepfen beim Lachen
  float jiggle = 0;
  if (now < hohoUntil) jiggle = sinf(now * 0.025f) * 4.0f;

  santa::Params p;
  p.lookX   = (int)eyeX;
  p.lookY   = (int)(eyeY + jiggle);
  p.pupil   = (int)(pupil + 0.5f);
  p.open    = constrain(lidOpen - eyeY * 0.009f, 0.0f, 1.0f) * blink;
  p.smile   = smile;
  p.browDY  = (int)(eyeY * 0.3f - (smile - GRUND_LACHEN) * 22 + (sleeping ? 4 : 0) + jiggle * 0.5f);
  p.twinkle = tw;
  p.dim     = dimLevel > 0.995f ? 256 : (int)(dimLevel * 256);
  return p;
}

// Zwinkern: Faktor fuer das Oberlid eines Auges
float winkFactor(int e, uint32_t now) {
  if (winkEye != e) return 1.0f;
  float t = (now - winkStart) / (float)WINK_MS;
  if (t >= 1.0f) { winkEye = -1; return 1.0f; }
  return (t < 0.35f) ? 1.0f - t / 0.35f : (t < 0.6f ? 0.0f : (t - 0.6f) / 0.4f);
}

// ---------------- Textmeldung auf beiden Displays ----------------
void showMessage(const String& l1, const String& l2, const String& l3, uint32_t ms) {
  for (int e = 0; e < NUM_EYES; e++) {   // je Display mit eigener Drehung
    selectEye(e);
    tft.setRotation(eyeRot[e]);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setFont(&fonts::FreeSansBold9pt7b);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(l1, 120, 90);
    tft.setTextColor(TFT_YELLOW);
    tft.drawString(l2, 120, 120);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(l3, 120, 150);
  }
  selectEye(-1);
  msgUntil = millis() + ms;
  fullRedraw[0] = fullRedraw[1] = true;
}

// ---------------- WLAN + Webserver ----------------
Preferences prefs;
// Kleiner, nicht blockierender Webserver. Der Standard-WebServer wartet bis
// zu 5 s auf eine Verbindung, die (wie bei Handy-Browsern ueblich) vorsorglich
// geoeffnet, aber nie benutzt wird – dann haengen alle anderen Anfragen.
// Dieser hier bedient mehrere Verbindungen gleichzeitig und schliesst jede
// Antwort sofort.
class MiniServer {
public:
  typedef std::function<void()> Fn;
  void on(const char* path, Fn f) { if (nRoutes < 12) routes[nRoutes++] = {path, f}; }
  void on(const char* path, int /*Methode*/, Fn f) { on(path, f); }
  void onNotFound(Fn f) { notFound = f; }
  // POST auf diesen Pfad: der Rumpf (z. B. Firmware) wird nicht gepuffert,
  // sondern bleibt in der Verbindung und wird vom Handler selbst gelesen.
  typedef std::function<void(NetworkClient&, int)> UploadFn;
  void onUpload(const char* path, UploadFn f) { upPrefix = String("POST ") + path; upload = f; }
  void begin() { srv.begin(); srv.setNoDelay(true); }

  String arg(const char* name) const {
    for (int i = 0; i < nArgs; i++) if (argK[i] == name) return argV[i];
    return String();
  }
  void sendHeader(const String& k, const String& v, bool = false) { extraHdr += k + ": " + v + "\r\n"; }
  void send(int code, const char* type = nullptr, const String& body = String()) {
    sendRaw(code, type, (const uint8_t*)body.c_str(), body.length());
  }
  void send_P(int code, const char* type, const char* body) {
    sendRaw(code, type, (const uint8_t*)body, strlen(body));
  }

  void handleClient() {
    // neue Verbindungen annehmen
    while (true) {
      NetworkClient nc = srv.accept();
      if (!nc) break;
      int slot = -1;
      for (int i = 0; i < MAXC; i++) if (!conn[i].used) { slot = i; break; }
      if (slot < 0) { nc.stop(); continue; }
      conn[slot].c = nc; conn[slot].buf = ""; conn[slot].t = millis(); conn[slot].used = true;
    }
    // vorhandene Verbindungen bearbeiten (ohne zu warten)
    for (int i = 0; i < MAXC; i++) {
      Conn& k = conn[i];
      if (!k.used) continue;
      bool isUpload = false;
      while (k.c.available() && k.buf.length() < 3000) {
        k.buf += (char)k.c.read();
        if (upload && k.buf.endsWith("\r\n\r\n") && k.buf.startsWith(upPrefix)) { isUpload = true; break; }
      }
      int he = k.buf.indexOf("\r\n\r\n");
      bool done = false;
      if (he >= 0) {
        int cl = 0, p = k.buf.indexOf("Content-Length:");
        if (p < 0) p = k.buf.indexOf("content-length:");
        if (p >= 0 && p < he) cl = k.buf.substring(p + 15, k.buf.indexOf("\r\n", p)).toInt();
        if (isUpload) { parseRequest(k, he, 0); upload(k.c, cl); cur = nullptr; done = true; }
        else if ((int)k.buf.length() >= he + 4 + cl) { dispatch(k, he, cl); done = true; }
      }
      if (done || millis() - k.t > 3000 || (!k.c.connected() && !k.c.available())) {
        k.c.stop(); k.used = false; k.buf = "";
      }
    }
  }

private:
  static const int MAXC = 4;
  struct Conn { NetworkClient c; String buf; uint32_t t; bool used = false; };
  struct Route { const char* path; Fn fn; };
  NetworkServer srv{80};
  Conn   conn[MAXC];
  Route  routes[12];
  int    nRoutes = 0;
  Fn     notFound;
  UploadFn upload;
  String upPrefix;
  NetworkClient* cur = nullptr;
  String argK[8], argV[8];
  int    nArgs = 0;
  String extraHdr;

  static String urlDecode(const String& in) {
    String out; out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
      char ch = in[i];
      if (ch == '+') out += ' ';
      else if (ch == '%' && i + 2 < in.length()) {
        char h[3] = {in[i + 1], in[i + 2], 0};
        out += (char)strtol(h, nullptr, 16); i += 2;
      } else out += ch;
    }
    return out;
  }
  void parseArgs(const String& q) {
    int pos = 0;
    while (pos < (int)q.length() && nArgs < 8) {
      int amp = q.indexOf('&', pos); if (amp < 0) amp = q.length();
      String kv = q.substring(pos, amp);
      int eq = kv.indexOf('=');
      argK[nArgs] = urlDecode(eq < 0 ? kv : kv.substring(0, eq));
      argV[nArgs] = eq < 0 ? String() : urlDecode(kv.substring(eq + 1));
      nArgs++;
      pos = amp + 1;
    }
  }
  // Pfad und Parameter lesen, Antworten gehen ab jetzt an diese Verbindung
  String parseRequest(Conn& k, int he, int cl) {
    int sp1 = k.buf.indexOf(' '), sp2 = k.buf.indexOf(' ', sp1 + 1);
    String url = k.buf.substring(sp1 + 1, sp2);
    int qm = url.indexOf('?');
    nArgs = 0; extraHdr = "";
    if (qm >= 0) parseArgs(url.substring(qm + 1));
    if (cl > 0) parseArgs(k.buf.substring(he + 4, he + 4 + cl));   // Formular (POST)
    cur = &k.c;
    return qm < 0 ? url : url.substring(0, qm);
  }
  void dispatch(Conn& k, int he, int cl) {
    String path = parseRequest(k, he, cl);
    for (int i = 0; i < nRoutes; i++)
      if (path == routes[i].path) { routes[i].fn(); cur = nullptr; return; }
    if (notFound) notFound(); else send(404, "text/plain", "nicht gefunden");
    cur = nullptr;
  }
  void sendRaw(int code, const char* type, const uint8_t* body, size_t len) {
    if (!cur) return;
    const char* txt = code == 200 ? "OK" : code == 204 ? "No Content" : code == 302 ? "Found" :
                      code == 403 ? "Forbidden" : code == 500 ? "Error" : "Not Found";
    String h = "HTTP/1.1 " + String(code) + " " + txt + "\r\n";
    if (type) h += String("Content-Type: ") + type + "\r\n";
    h += "Content-Length: " + String(len) + "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extraHdr + "\r\n";
    cur->write((const uint8_t*)h.c_str(), h.length());
    size_t off = 0;
    while (off < len) {                       // grosse Seiten in Stuecken senden
      size_t n = cur->write(body + off, min((size_t)1400, len - off));
      if (n == 0) break;
      off += n;
    }
  }
};
MiniServer  server;
DNSServer   dns;
String      wifiSsid, wifiPass;
bool        apMode = false, staConnected = false;
uint32_t    wifiStart = 0;

const char PAGE_MAIN[] PROGMEM = R"HTML(<!doctype html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Santa-Augen</title><style>
:root{--bg:#7a0f12;--card:#a3161b;--fg:#fff8ee;--gold:#f6c453;--btn:#fff8ee;--btnfg:#7a0f12}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;font-family:system-ui,sans-serif;background:var(--bg);color:var(--fg);
 display:flex;flex-direction:column;align-items:center;padding:16px;gap:14px;user-select:none}
h1{margin:4px 0 0;font-size:22px}
#pad{width:min(86vw,320px);aspect-ratio:1;border-radius:50%;background:radial-gradient(circle,#fff 0 38%,#f3e3d3 39% 100%);
 position:relative;touch-action:none;box-shadow:inset 0 0 0 6px var(--gold)}
#dot{position:absolute;width:30%;height:30%;border-radius:50%;left:35%;top:35%;
 background:radial-gradient(circle at 35% 35%,#fff 0 10%,#5a8fd6 12% 55%,#1b2a4a 57% 100%);pointer-events:none;transition:left .06s,top .06s}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;width:min(92vw,360px)}
button{border:0;border-radius:14px;padding:14px 6px;font-size:15px;font-weight:600;background:var(--btn);color:var(--btnfg)}
button:active{transform:scale(.96)}
button.big{grid-column:span 3;background:var(--gold)}
.row{display:flex;align-items:center;gap:10px;width:min(92vw,360px);background:var(--card);padding:10px 14px;border-radius:14px}
.row label{flex:1}
input[type=range]{flex:2}
a{color:var(--gold);font-size:14px}
</style></head><body>
<h1>🎅 Santa-Augen</h1>
<div id="pad"><div id="dot"></div></div>
<div class="grid">
 <button onclick="c('winkL')">😉 Zwinkern L</button>
 <button onclick="c('blink')">👁 Blinzeln</button>
 <button onclick="c('winkR')">Zwinkern R 😉</button>
 <button onclick="c('smile')">😊 Lächeln</button>
 <button onclick="c('twinkle')">✨ Funkeln</button>
 <button onclick="c('hearts')">💕 Herzchen</button>
 <button onclick="c('center')">🎯 Geradeaus</button>
 <button class="big" style="background:#fff8ee" onclick="c('test')">🧪 Test: Links / Rechts / Blickrichtung</button>
 <button class="big" onclick="c('hoho')">🎅 Ho Ho Ho!</button>
 <button id="sl" class="big" onclick="c(sleep?'wake':'sleep')">😴 Schlafen</button>
</div>
<div class="row"><label for="au">Automatik</label><input type="checkbox" id="au" onchange="c(this.checked?'auto1':'auto0')"></div>
<div class="row"><label for="fo">Personen folgen (Radar)</label><input type="checkbox" id="fo" onchange="c(this.checked?'follow1':'follow0')"></div>
<div class="row"><label for="sg">Nur eine Person verfolgen (ruhiger)</label><input type="checkbox" id="sg" onchange="c(this.checked?'single1':'single0')"></div>
<div class="row" style="flex-wrap:wrap;gap:8px"><span style="flex-basis:100%">Erfassungszone (cm)</span>
 <label>ab <input id="zmin" type="number" min="0" max="550" step="10" style="width:64px"></label>
 <label>bis <input id="zmax" type="number" min="50" max="600" step="10" style="width:64px"></label>
 <label>seitlich ± <input id="zw" type="number" min="20" max="400" step="10" style="width:64px"></label></div>
<div class="row"><label for="ss">Augen zu &amp; dunkel nach … s ohne Bewegung (0 = nie)</label><input id="ss" type="number" min="0" max="3600" step="5" style="width:70px" onchange="c('sleeps&v='+this.value)"></div>
<div class="row"><label for="fl">Links/rechts tauschen</label><input type="checkbox" id="fl" onchange="c(this.checked?'flip1':'flip0')"></div>
<div class="grid"><button class="big" style="background:#fff8ee" onclick="c('calib')">📐 Ich stehe mittig – Mitte setzen</button></div>
<div style="font-size:13px;opacity:.85;width:min(92vw,360px)">Stell dich 1–2 m mittig vor den Weihnachtsmann und tippe auf „Mitte setzen“. Korrektur: <span id="off">0</span>° · <a href="#" onclick="c('calib0');return false">zurücksetzen</a></div>
<div class="row" id="rs">Radar: …</div>
<canvas id="map" width="720" height="440" style="width:min(92vw,360px);border-radius:14px;background:#5e0b0e"></canvas>
<div style="font-size:13px;opacity:.85;width:min(92vw,360px)">So wie du davor stehst: der Weihnachtsmann ist oben und schaut zu dir, links/rechts aus deiner Sicht. Gold = Person, der er folgt · weiß = weitere Person · leerer Kreis = ignoriert (außerhalb der Zone oder noch nicht stabil) · helles Feld = Erfassungszone · rote Linie = Blickrichtung. Läuft dein Punkt falsch herum mit: „Links/rechts tauschen“.</div>
<div class="row"><label for="br">Helligkeit</label><input type="range" id="br" min="5" max="255" onchange="c('bright&v='+this.value)"></div>
<div style="display:flex;gap:18px"><a href="/wifi">WLAN einrichten</a><a href="/update">Firmware-Update</a></div>
<div style="font-size:12px;opacity:.6">läuft seit <span id="upt">–</span></div>
<script>
let sleep=false,last=0,pend=null;
function c(x){fetch('/cmd?c='+x).then(r=>r.json()).then(st)}
function st(s){if(s.msg)alert(s.msg);sleep=s.sleep;au.checked=s.auto;br.value=s.bright;fo.checked=s.follow;fl.checked=s.flip;off.textContent=s.off;sg.checked=s.single;
 if(document.activeElement.type!='number'){zmin.value=s.zmin/10;zmax.value=s.zmax/10;zw.value=s.zw/10;ss.value=s.sleeps}
 sl.textContent=sleep?'☀️ Aufwachen':'😴 Schlafen';
 upt.textContent=s.up<120?s.up+' s':Math.round(s.up/60)+' min';
 rs.textContent=s.up<5&&!s.radar?'Radar: startet …':s.rage>3000?'Radar: keine Daten – Verkabelung prüfen (TX→20, RX→21, 5V)':
  s.n==0?'Radar: niemand da':
  'Radar: '+s.n+(s.n>1?' Personen':' Person')+' · nächste '+(s.dist/1000).toFixed(1).replace('.',',')+' m, '+
  Math.abs(Math.round(s.ang))+'° '+(s.ang<0?'links':'rechts')+' (von dir aus gesehen)'}
function look(e){const r=pad.getBoundingClientRect();
 let x=(e.clientX-r.left)/r.width*2-1,y=(e.clientY-r.top)/r.height*2-1;
 const d=Math.hypot(x,y);if(d>1){x/=d;y/=d}
 dot.style.left=(35+x*35)+'%';dot.style.top=(35+y*35)+'%';
 const q='/look?x='+x.toFixed(2)+'&y='+y.toFixed(2),n=Date.now();
 if(n-last>70){last=n;fetch(q)}else{clearTimeout(pend);pend=setTimeout(()=>{last=Date.now();fetch(q)},75)}}
pad.addEventListener('pointerdown',e=>{pad.setPointerCapture(e.pointerId);look(e)});
pad.addEventListener('pointermove',e=>{if(e.buttons||e.pointerType=='touch')look(e)});
fetch('/state').then(r=>r.json()).then(st);
function draw(s){const g=map.getContext('2d'),W=map.width,H=map.height,cx=W/2,cy=20,R=H-40,M=6000,k=R/M;
 g.clearRect(0,0,W,H);g.lineWidth=2;g.font='22px system-ui';
 if(s.zmax){g.fillStyle='rgba(246,196,83,.10)';g.fillRect(cx-s.zw*k,cy+s.zmin*k,2*s.zw*k,(s.zmax-s.zmin)*k)}
 g.fillStyle='rgba(255,248,238,.6)';g.strokeStyle='rgba(255,248,238,.18)';
 for(let m=1;m<=6;m++){g.beginPath();g.arc(cx,cy,m*1000*k,Math.PI*(0.5-1/3),Math.PI*(0.5+1/3));g.stroke();
  if(m%2==0)g.fillText(m+' m',cx+4,cy+m*1000*k-6)}
 for(const a of [-60,-30,0,30,60]){const r=a*Math.PI/180;g.beginPath();g.moveTo(cx,cy);g.lineTo(cx+Math.sin(r)*R,cy+Math.cos(r)*R);g.stroke()}
 if(s.radar){const ea=s.look*s.wink*Math.PI/180,L=R*0.95;
  g.strokeStyle=s.track?'#ff5a4e':'rgba(255,90,78,.45)';g.lineWidth=5;g.beginPath();g.moveTo(cx,cy);
  g.lineTo(cx+Math.sin(ea)*L,cy+Math.cos(ea)*L);g.stroke();g.lineWidth=3;
  (s.t||[]).forEach((t,i)=>{if(!t)return;const x=cx+t[0]*k,y=cy+t[1]*k;g.beginPath();
   if(t[2]){g.arc(x,y,i==s.best?16:11,0,7);g.fillStyle=i==s.best?'#f6c453':'#fff8ee';g.fill()}
   else{g.arc(x,y,10,0,7);g.strokeStyle='rgba(255,248,238,.45)';g.stroke()}})}
 g.fillStyle='#fff8ee';g.beginPath();g.arc(cx,cy,12,0,7);g.fill();g.fillText('🎅',cx+16,cy+8)}
for(const el of [zmin,zmax,zw])el.addEventListener('change',()=>c('zone&min='+zmin.value+'&max='+zmax.value+'&w='+zw.value));
let busy=false;
setInterval(()=>{if(document.hidden||busy)return;busy=true;
 fetch('/state').then(r=>r.json()).then(s=>{st(s);draw(s)}).catch(()=>{}).finally(()=>busy=false)},300);
</script></body></html>)HTML";

const char PAGE_WIFI[] PROGMEM = R"HTML(<!doctype html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WLAN einrichten</title><style>
body{margin:0;font-family:system-ui,sans-serif;background:#7a0f12;color:#fff8ee;padding:20px;display:flex;flex-direction:column;gap:12px;max-width:380px}
input,button{font-size:16px;padding:12px;border-radius:10px;border:0}
button{background:#f6c453;color:#7a0f12;font-weight:700}
a{color:#f6c453}
</style></head><body>
<h2>🎅 WLAN einrichten</h2>
<form method="post" action="/savewifi" style="display:flex;flex-direction:column;gap:12px">
<input name="s" placeholder="WLAN-Name (SSID)" required>
<input name="p" type="password" placeholder="Passwort">
<button>Speichern &amp; neu starten</button></form>
<p>Danach erreichst du die Augen unter <b>http://santa.local</b></p>
<a href="/">zurück</a></body></html>)HTML";

const char PAGE_UPDATE[] PROGMEM = R"HTML(<!doctype html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware-Update</title><style>
body{margin:0;font-family:system-ui,sans-serif;background:#7a0f12;color:#fff8ee;padding:20px;display:flex;flex-direction:column;gap:12px;max-width:380px}
input,button{font-size:16px;padding:12px;border-radius:10px;border:0;background:#fff8ee;color:#7a0f12}
button{background:#f6c453;font-weight:700}
progress{width:100%;height:18px}
a{color:#f6c453}
</style></head><body>
<h2>🎅 Firmware-Update</h2>
<p style="margin:0;font-size:14px;opacity:.85">Arduino IDE: Sketch → „Kompilierte Binärdatei exportieren“, dann die Datei <b>santa_augen_c3.ino.bin</b> wählen (nicht die mit „bootloader“ oder „merged“).</p>
<input id="f" type="file" accept=".bin">
<input id="pw" type="password" placeholder="Update-Passwort">
<button id="go" onclick="up()">Hochladen</button>
<progress id="pr" max="100" value="0" hidden></progress>
<div id="msg"></div>
<a href="/">zurück</a>
<script>
function up(){const file=f.files[0];if(!file){msg.textContent='Bitte eine .bin-Datei wählen.';return}
 const x=new XMLHttpRequest();x.open('POST','/update?pw='+encodeURIComponent(pw.value));
 x.setRequestHeader('Content-Type','application/octet-stream');
 x.upload.onprogress=e=>{if(e.lengthComputable){pr.value=e.loaded/e.total*100;msg.textContent=Math.round(pr.value)+' %'}};
 x.onload=()=>{if(x.status==200){msg.textContent='Fertig – Santa startet neu …';setTimeout(()=>location.href='/',9000)}
  else{msg.textContent='Fehler: '+x.responseText;go.disabled=false}};
 x.onerror=()=>{msg.textContent='Verbindung abgebrochen.';go.disabled=false};
 go.disabled=true;pr.hidden=false;msg.textContent='lade hoch …';x.send(file)}
</script></body></html>)HTML";

void sendState() {
  String j = "{\"auto\":";
  j += autoMode ? "true" : "false";
  j += ",\"sleep\":";
  j += sleeping ? "true" : "false";
  j += ",\"bright\":" + String(brightness);
  j += ",\"follow\":";
  j += followMode ? "true" : "false";
  j += ",\"flip\":";
  j += radarFlip ? "true" : "false";
  j += ",\"radar\":";
  j += (radarLastFrame && millis() - radarLastFrame < 2000) ? "true" : "false";
  j += ",\"n\":" + String(radarCount);
  j += ",\"dist\":" + String((int)radarDist);
  j += ",\"ang\":" + String(radarFlip ? -radarAngle : radarAngle, 1);
  j += ",\"off\":" + String(radarOffset, 1);
  j += ",\"single\":";
  j += radarSingle ? "true" : "false";
  j += ",\"sleeps\":" + String(schlafS);
  j += ",\"up\":" + String(millis() / 1000) + ",\"rst\":" + String((int)esp_reset_reason());
  j += ",\"rage\":" + String(radarLastFrame ? (millis() - radarLastFrame) : 99999);
  j += ",\"zmin\":" + String(zoneMin) + ",\"zmax\":" + String(zoneMax) + ",\"zw\":" + String(zoneW);
  j += ",\"track\":";
  j += radarTracking ? "true" : "false";
  j += ",\"look\":" + String(constrain(lookX / FOLGE_PX, -1.0f, 1.0f), 2);   // -1..1
  j += ",\"wink\":" + String(RADAR_WINKEL, 0);
  j += ",\"best\":" + String(radarBest);
  j += ",\"t\":[";
  for (int i = 0; i < 3; i++) {                               // alle 3 Ziele (ungueltige als null)
    if (i) j += ",";
    if (radarT[i].valid) j += "[" + String(radarFlip ? -radarT[i].x : radarT[i].x) + "," + String(radarT[i].y) + "," + (radarOk[i] ? "1" : "0") + "]";
    else j += "null";
  }
  j += "]";
  if (calMsg.length()) { j += ",\"msg\":\"" + calMsg + "\""; calMsg = ""; }
  j += "}";
  server.send(200, "application/json", j);
}

void handleCmd() {
  uint32_t now = millis();
  String c = server.arg("c");
  if (c == "blink")        { doBlink(now); }
  else if (c == "winkL")   { winkEye = 0; winkStart = now; }
  else if (c == "winkR")   { winkEye = 1; winkStart = now; }
  else if (c == "smile")   { doSmile(now, 0.42f, 2500); twinkleT0 = now + 250; manualUntil = now + MANUELL_MS; }
  else if (c == "twinkle") { startTwinkle(now); }
  else if (c == "test")    { testStart = now; sleeping = false; autoSleeping = false; radarLastSeen = now; }
  else if (c == "center")  { targetX = 0; targetY = 0; manualUntil = now + MANUELL_MS; }
  else if (c == "hoho")    { doSmile(now, 0.5f, 2600); twinkleT0 = now + 200; hohoUntil = now + 2600;
                             targetX = 0; targetY = -4; manualUntil = now + 4000; }
  else if (c == "sleep")   { sleeping = true; autoSleeping = false; wakeAt = 0; smileState = 0; }
  else if (c == "wake")    { if (sleeping && !wakeAt) { startHearts(now); wakeAt = now + WECK_NACH_MS; }
                             radarLastSeen = now; }
  else if (c == "hearts")  { startHearts(now); }
  else if (c == "follow1") { followMode = true;  prefs.putBool("follow", true); }
  else if (c == "follow0") { followMode = false; prefs.putBool("follow", false); }
  else if (c == "flip1")   { radarFlip = true;   prefs.putBool("flip", true); }
  else if (c == "flip0")   { radarFlip = false;  prefs.putBool("flip", false); }
  else if (c == "calib") {                            // aktuelle Position = Mitte
    // nur mit genau einer stabil erkannten Person und plausibler Schraeglage,
    // sonst dreht eine Reflexion das ganze Sichtfeld weg
    float neu = radarOffset + radarAngle;
    if (radarCount != 1 || !radarTracking) calMsg = "Nicht gesetzt: es muss genau eine Person erkannt sein.";
    else if (fabsf(neu) > RADAR_KALIB_MAX) calMsg = "Nicht gesetzt: " + String(neu, 0) + " Grad ist zu schief – stehst du wirklich mittig?";
    else {
      radarOffset = neu; radarAngle = 0;
      prefs.putFloat("roff", radarOffset);
      calMsg = "Mitte gesetzt, Korrektur " + String(radarOffset, 1) + " Grad.";
    }
    LOG.printf("[RADAR] %s\n", calMsg.c_str());
  }
  else if (c == "calib0")  { radarOffset = 0; prefs.putFloat("roff", 0); }
  else if (c == "single1" || c == "single0") {
    radarSingle = (c == "single1");
    prefs.putBool("single", radarSingle);
    radarSetMode(radarSingle);
  }
  else if (c == "sleeps")  { schlafS = constrain(server.arg("v").toInt(), 0, 3600); prefs.putInt("sleeps", schlafS); }
  else if (c == "zone") {
    zoneMin = constrain(server.arg("min").toInt() * 10, 0, 5500);
    zoneMax = constrain(server.arg("max").toInt() * 10, zoneMin + 300, 6000);
    zoneW   = constrain(server.arg("w").toInt() * 10, 200, 4000);
    prefs.putInt("zmin", zoneMin); prefs.putInt("zmax", zoneMax); prefs.putInt("zw", zoneW);
  }
  else if (c == "auto1")   { autoMode = true; manualUntil = 0; }
  else if (c == "auto0")   { autoMode = false; }
  else if (c == "bright")  { brightness = constrain(server.arg("v").toInt(), 5, 255);
                             tft.setBrightness(brightness); prefs.putUChar("bright", brightness); }
  sendState();
}

void handleLook() {
  float x = constrain(server.arg("x").toFloat(), -1.0f, 1.0f);
  float y = constrain(server.arg("y").toFloat(), -1.0f, 1.0f);
  targetX = x * LOOK_X * 1.3f;
  targetY = y * LOOK_Y * 1.6f;
  if (sleeping) { sleeping = false; nextBlink = millis() + 900; }
  manualUntil = millis() + MANUELL_MS;
  server.send(204);
}

void handleSaveWifi() {
  prefs.putString("ssid", server.arg("s"));
  prefs.putString("pass", server.arg("p"));
  server.send(200, "text/html; charset=utf-8",
    "<meta charset=utf-8><body style='font-family:sans-serif;background:#7a0f12;color:#fff;padding:20px'>"
    "<h2>Gespeichert – starte neu …</h2><p>Gleich unter <b>http://santa.local</b> erreichbar.</p>");
  delay(800);
  ESP.restart();
}

// Firmware per Browser: Rumpf = rohe .bin-Datei (kein Formular), Passwort im Parameter pw
void handleUpload(NetworkClient& c, int len) {
  uint8_t buf[1024];
  int got = 0;
  uint32_t t = millis();
  const bool ok = server.arg("pw") == OTA_PASS && len > 0 && Update.begin(len);
  if (ok) LOG.printf("[OTA] Browser-Update, %d Byte\n", len);
  int lastPct = -1;
  // auch bei falschem Passwort alles abholen, sonst sieht der Browser nur einen Abbruch
  while (got < len && millis() - t < 10000) {
    int n = c.read(buf, min((int)sizeof(buf), len - got));
    if (n <= 0) { delay(1); continue; }
    got += n; t = millis();
    if (ok && !Update.hasError()) Update.write(buf, n);
    int pct = (int)(got * 10LL / len) * 10;
    if (ok && pct != lastPct) { lastPct = pct; showMessage("Firmware-Update", String(pct) + " %", "nicht ausschalten", 60000); }
  }
  if (!ok) {
    server.send(403, "text/plain; charset=utf-8",
                server.arg("pw") != OTA_PASS ? "falsches Passwort" : "Update nicht möglich (Partitionsschema mit OTA?)");
    return;
  }
  if (got == len && Update.end(true)) {
    LOG.printf("[OTA] fertig, Neustart\n");
    server.send(200, "text/plain", "OK");
    showMessage("Update fertig", "Neustart ...", "", 5000);
    delay(500);
    ESP.restart();
  }
  String err = got < len ? String("Übertragung abgebrochen") : String(Update.errorString());
  Update.abort();
  LOG.printf("[OTA] Fehler: %s\n", err.c_str());
  server.send(500, "text/plain; charset=utf-8", err);
  showMessage("Update fehlgeschlagen", err, "", 4000);
}

// Firmware per Arduino IDE (Netzwerk-Port "santa")
void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.setMdnsEnabled(false);      // mDNS startet serviceWifi() selbst
  ArduinoOTA.onStart([] { LOG.printf("[OTA] IDE-Update startet\n"); showMessage("Firmware-Update", "0 %", "nicht ausschalten", 60000); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int lastPct = -1;
    int pct = (int)(done * 10ULL / total) * 10;
    if (pct != lastPct) { lastPct = pct; showMessage("Firmware-Update", String(pct) + " %", "nicht ausschalten", 60000); }
  });
  ArduinoOTA.onEnd([] { showMessage("Update fertig", "Neustart ...", "", 5000); });
  ArduinoOTA.onError([](ota_error_t e) {
    LOG.printf("[OTA] Fehler %d\n", (int)e);
    showMessage("Update fehlgeschlagen", "Fehler " + String((int)e), "", 4000);
  });
  ArduinoOTA.begin();
}

void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setTxPower(WIFI_TX);
  WiFi.setSleep(false);            // kein WLAN-Stromsparen -> Webseite reagiert sofort
  LOG.printf("[WLAN] eigenes WLAN '%s' an, IP %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  dns.start(53, "*", WiFi.softAPIP());   // jede Adresse -> Steuerseite
  showMessage("WLAN: " AP_SSID, "Passwort: " AP_PASS, WiFi.softAPIP().toString(), 6000);
}

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html; charset=utf-8", PAGE_MAIN); });
  server.on("/wifi", [] { server.send_P(200, "text/html; charset=utf-8", PAGE_WIFI); });
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.on("/cmd", handleCmd);
  server.on("/look", handleLook);
  server.on("/state", sendState);
  server.on("/update", [] { server.send_P(200, "text/html; charset=utf-8", PAGE_UPDATE); });
  server.onUpload("/update", handleUpload);
  server.onNotFound([] {                  // Captive Portal im eigenen WLAN
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  setupOTA();
}

void setupWifi() {
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  WiFi.setHostname(HOSTNAME);
  if (wifiSsid.length() == 0) {
    startAP();
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    WiFi.setTxPower(WIFI_TX);
  WiFi.setSleep(false);            // kein WLAN-Stromsparen -> Webseite reagiert sofort
    wifiStart = millis();
    LOG.printf("[WLAN] verbinde mit '%s' ...\n", wifiSsid.c_str());
  }
}

void serviceWifi() {
  static bool     everConnected = false;
  static uint32_t lastRetry = 0;
  const uint32_t  now = millis();
  if (!staConnected && wifiSsid.length() && WiFi.status() == WL_CONNECTED) {
    staConnected = true;
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    MDNS.enableArduino(3232, true);      // Netzwerk-Port in der Arduino IDE
    LOG.printf("[WLAN] verbunden: http://%s.local  (%s)\n", HOSTNAME, WiFi.localIP().toString().c_str());
    if (!everConnected) showMessage("Santa-Augen", "http://" HOSTNAME ".local", WiFi.localIP().toString(), 4000);
    everConnected = true;
    if (apMode) { dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); apMode = false; }
  }
  // Verbindung verloren (Router-Neustart, Funkloch) -> neu verbinden
  if (staConnected && WiFi.status() != WL_CONNECTED) {
    staConnected = false;
    MDNS.end();
    wifiStart = lastRetry = now;           // eigenes WLAN erst nach 20 s ohne Verbindung
    LOG.printf("[WLAN] Verbindung verloren, verbinde neu ...\n");
    WiFi.reconnect();
  }
  // nicht verbunden -> regelmaessig neu versuchen (im eigenen WLAN seltener und nur,
  // solange kein Handy damit verbunden ist, weil die Suche das eigene WLAN kurz stoert)
  if (!staConnected && wifiSsid.length() && now - lastRetry > (apMode ? 30000u : 10000u)
      && (!apMode || WiFi.softAPgetStationNum() == 0)) {
    lastRetry = now;
    LOG.printf("[WLAN] neuer Verbindungsversuch mit '%s'\n", wifiSsid.c_str());
    WiFi.disconnect(false);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
  // gespeichertes WLAN nicht erreichbar -> zusaetzlich eigenes WLAN aufmachen
  if (!staConnected && !apMode && wifiSsid.length() && now - wifiStart > 20000) startAP();
  if (apMode) dns.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();
}

// ---------------------------------------------------------------------
void setup() {
  LOG.begin(115200);
  LOG_NONBLOCKING();
  delay(1500);                  // Zeit fuer den USB-Monitor
  LOG.printf("\n[START] Reset-Grund: %d, freier Speicher: %u\n",
                (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());
  pinMode(PIN_CS1, OUTPUT);
  pinMode(PIN_CS2, OUTPUT);
  prefs.begin("santa");
  brightness = prefs.getUChar("bright", HELLIGKEIT);
  followMode = prefs.getBool("follow", true);
  radarFlip  = prefs.getBool("flip", false);
  radarOffset = prefs.getFloat("roff", 0);
  if (fabsf(radarOffset) > RADAR_KALIB_MAX) radarOffset = 0;   // alte, unplausible Kalibrierung verwerfen

  Serial1.setRxBufferSize(512);
  Serial1.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX, RADAR_TX);
  radarSingle = prefs.getBool("single", true);
  zoneMin = prefs.getInt("zmin", 500);
  zoneMax = prefs.getInt("zmax", 4000);
  zoneW   = prefs.getInt("zw", 2000);
  schlafS = prefs.getInt("sleeps", RADAR_SCHLAF_S);
  delay(300);                    // Radar hochfahren lassen
  radarSetMode(radarSingle);

  selectEye(99);                // beide gleichzeitig initialisieren
  tft.init();
  tft.setBrightness(brightness);
  tft.fillScreen(TFT_BLACK);
  selectEye(-1);

  // Puffer vor dem WLAN anlegen, solange der Speicher noch frei ist
  for (int i = 0; i < 2; i++) {
    stripBuf[i] = (uint16_t*)heap_caps_malloc(santa::W * STRIP_H * 2, MALLOC_CAP_DMA);
    if (!stripBuf[i]) {
      selectEye(99);
      tft.setTextColor(TFT_RED);
      tft.drawString("kein RAM", 90, 110);
      while (true) delay(1000);
    }
  }
  randomSeed(esp_random());
  santa::init();

  LOG.printf("[START] Augen bereit, freier Speicher: %u\n", (unsigned)ESP.getFreeHeap());
  setupWifi();                  // zuerst WLAN: startet den Netzwerk-Stack
  setupWeb();
  LOG.printf("[START] WLAN gestartet (AP=%d), freier Speicher: %u\n", apMode, (unsigned)ESP.getFreeHeap());
}

// Beschriftung fuer den Test in den gerade gerenderten Streifen zeichnen
// ein Herz: zwei Kreise oben, Dreieck nach unten, kleiner Glanzpunkt
void drawHeart(LGFX_Sprite& g, int cx, int cy, int size, uint16_t col) {
  if (size < 3) return;
  int r = size / 4 + 1;
  g.fillCircle(cx - r, cy - r / 2, r, col);
  g.fillCircle(cx + r, cy - r / 2, r, col);
  g.fillTriangle(cx - 2 * r + 1, cy, cx + 2 * r - 1, cy, cx, cy + 2 * r + r / 2, col);
  if (size > 10) g.fillCircle(cx - r - r / 3, cy - r + 1, r / 3, TFT_WHITE);
}

// Herzchen in den gerade gerenderten Streifen zeichnen
void drawHearts(uint16_t* buf, int y0, int h, uint32_t now) {
  if (!heartsStart || now < heartsStart) return;
  overlay.setBuffer(buf, santa::W, h, 16);
  uint32_t t = now - heartsStart;
  for (int i = 0; i < HERZ_ANZAHL; i++) {
    const Heart& hz = hearts[i];
    if (t < hz.t0 || t > hz.t0 + HERZ_LEBEN) continue;
    float p = (t - hz.t0) / (float)HERZ_LEBEN;          // 0..1 Lebenszeit
    float grow = p < 0.85f ? p / 0.85f : (1.0f - p) / 0.15f;  // wachsen, dann schnell weg
    grow = grow * grow * (3 - 2 * grow);
    int size = (int)(hz.size * grow);
    int y = hz.y - (int)(p * 30) - y0;                   // steigt langsam nach oben
    drawHeart(overlay, hz.x, y, size, hz.col);
  }
}

void drawTestOverlay(uint16_t* buf, int y0, int h, int e) {
  if (testStep < 0) return;
  overlay.setBuffer(buf, santa::W, h, 16);
  overlay.setTextDatum(textdatum_t::middle_center);
  const uint16_t BOX = overlay.color565(30, 20, 20);
  if (testStep == 0) {
    // grosses Schild LINKS / RECHTS
    overlay.fillRoundRect(40, 30 - y0, 160, 54, 12, BOX);
    overlay.setFont(&fonts::FreeSansBold18pt7b);
    overlay.setTextColor(e == 0 ? TFT_YELLOW : TFT_CYAN);
    overlay.drawString(e == 0 ? "LINKS" : "RECHTS", 120, 58 - y0);
    overlay.fillRoundRect(55, 190 - y0, 130, 30, 8, BOX);
    overlay.setFont(&fonts::FreeSans9pt7b);
    overlay.setTextColor(TFT_WHITE);
    overlay.drawString(e == 0 ? "Display 1 - CS 4" : "Display 2 - CS 10", 120, 206 - y0);
  } else {
    const TestStep& st = TEST_STEPS[testStep];
    overlay.fillRoundRect(45, 192 - y0, 150, 30, 8, BOX);
    overlay.setFont(&fonts::FreeSansBold9pt7b);
    overlay.setTextColor(TFT_YELLOW);
    overlay.drawString(st.text, 120, 208 - y0);
    // Pfeil in Blickrichtung
    const int cx = 120, cy = 40 - y0;
    const uint16_t A = TFT_YELLOW;
    if (st.x < 0) overlay.fillTriangle(cx - 30, cy, cx - 6, cy - 14, cx - 6, cy + 14, A);
    if (st.x > 0) overlay.fillTriangle(cx + 30, cy, cx + 6, cy - 14, cx + 6, cy + 14, A);
    if (st.y < 0) overlay.fillTriangle(cx, cy - 16, cx - 14, cy + 8, cx + 14, cy + 8, A);
    if (st.y > 0) overlay.fillTriangle(cx, cy + 16, cx - 14, cy - 8, cx + 14, cy - 8, A);
  }
}

void loop() {
  static uint32_t loopStart = 0;
  if (loopStart && millis() - loopStart > radarLoopMax) radarLoopMax = millis() - loopStart;
  loopStart = millis();
  serviceWifi();
  uint32_t now = millis();
  radarUpdate(now);
  serviceWake(now);

  static uint32_t lastLog = 0, frames = 0;    // alle 5 s Status auf USB-Seriell
  frames++;
  if (now - lastLog >= 5000) {
    LOG.printf("[STATUS] %.1f fps, frei %u Byte, WLAN %s, AP-Clients %d | Radar: %u Rahmen, %d Pers., %.0f mm, %.0f Grad\n",
                  frames * 1000.0f / (now - lastLog), (unsigned)ESP.getFreeHeap(),
                  staConnected ? WiFi.localIP().toString().c_str() : "-", WiFi.softAPgetStationNum(),
                  (unsigned)radarFrameCount, radarCount, radarDist, radarAngle);
    LOG.printf("[RADAR] max. Luecke %u ms, kaputte Rahmen %u, laengste Schleife %u ms\n",
               (unsigned)radarMaxGap, (unsigned)radarBadTail, (unsigned)radarLoopMax);
    radarMaxGap = 0; radarLoopMax = 0;
    lastLog = now; frames = 0;
  }
  // Test-Ablauf: Blickrichtung vorgeben, Radar/Automatik solange pausieren
  if (testPending && now >= msgUntil) { testPending = false; testStart = now; }
  testStep = -1;
  if (testStart) {
    uint32_t t = now - testStart;
    for (int i = 0; i < TEST_N; i++) if (t < TEST_STEPS[i].until) { testStep = i; break; }
    if (testStep < 0) {
      testStart = 0;
      LOG.printf("[TEST] fertig\n");
      fullRedraw[0] = fullRedraw[1] = true;   // Beschriftung wegwischen
    } else {
      targetX = TEST_STEPS[testStep].x * LOOK_X * 1.3f;
      targetY = TEST_STEPS[testStep].y * LOOK_Y * 1.6f;
      manualUntil = now + 1500;
      nextBlink = now + 1500;
      radarTracking = false;
    }
  }

  santa::Params base = updateAnimation(now);
  if (now < msgUntil) { delay(10); return; }   // Textmeldung stehen lassen

  for (int e = 0; e < NUM_EYES; e++) {
    santa::Params p = base;
    bool mirror = (e == 0);            // Display 1 (links) ist gespiegelt
    if (mirror) p.lookX = -p.lookX;    // damit beide in dieselbe Richtung schauen
    p.open *= winkFactor(e, now);
    santa::prepare(p);

    selectEye(e);
    tft.setRotation(eyeRot[e]);        // Drehung gilt fuer das gerade gewaehlte Display
    tft.startWrite();
    static int lastDim[2] = {256, 256};
    if (base.dim != lastDim[e]) { fullRedraw[e] = true; lastDim[e] = base.dim; }
    // Herzchen: solange sie laufen, ganzes Bild zeichnen; danach einmal sauber neu
    const bool heartsOn = heartsStart && now >= heartsStart;
    if (heartsOn && now - heartsStart > HERZ_MS) { heartsStart = 0; fullRedraw[0] = fullRedraw[1] = true; }
    const bool full = fullRedraw[e] || testStep >= 0 || (heartsStart && now >= heartsStart);
    const int ys = full ? 0 : DIRTY_Y0, ye = full ? 240 : DIRTY_Y1;
    int k = 0;
    for (int y = ys; y < ye; y += STRIP_H, k++) {
      const int h = min(STRIP_H, ye - y);
      uint16_t* buf = stripBuf[k & 1];
      santa::renderStrip(buf, y, h, mirror);
      drawTestOverlay(buf, y, h, e);
      drawHearts(buf, y, h, now);
      tft.pushImageDMA(0, y, santa::W, h, (lgfx::swap565_t*)buf);
    }
    fullRedraw[e] = false;
    tft.waitDMA();
    tft.endWrite();
    selectEye(-1);
  }
}
