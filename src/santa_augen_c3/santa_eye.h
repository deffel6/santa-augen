// =====================================================================
//  santa_eye.h – freundliches Weihnachtsmann-Auge, pixelweise gerendert
// =====================================================================
//  Reiner C++-Code ohne Display-Bibliothek, nur Ganzzahl-Rechnung im
//  Frame (der ESP32-C3 hat keine Gleitkomma-Einheit). Gleitkomma wird
//  nur einmal beim Start fuer Tabellen benutzt.
//  Pixelformat im Puffer: RGB565, Bytes vertauscht (wie das Display).
//
//  Gezeichnet wird das Auge, das der Betrachter RECHTS sieht; das
//  linke entsteht durch Spiegeln (mirror = true, lookX negieren).
// =====================================================================
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef SANTA_SKIN
#define SANTA_SKIN 1   // 1 = Hautfarbe um das Auge, 0 = schwarzer Hintergrund
#endif
#ifndef SANTA_BROWS
#define SANTA_BROWS 1  // 1 = weisse Augenbraue zeichnen, 0 = keine
#endif

namespace santa {

constexpr int W      = 240;
constexpr int CX     = 120;   // Augenmitte
constexpr int CY     = 134;
constexpr float EYE_W = 108;  // halbe Breite der Lidoeffnung
constexpr float UP_H  = 80;   // Oberlid ueber der Mitte (ganz offen)
constexpr float LO_H  = 52;   // Unterlid unter der Mitte
constexpr float SM_H  = 95;   // so weit hebt sich das Unterlid beim Laecheln
constexpr int IRIS_R = 56;
constexpr int TEX    = IRIS_R * 2 + 3;
constexpr int TEXC   = TEX / 2;

// Augenbraue (buschig, weiss)
constexpr float BCX = 128, BW = 104, BROW_Y = 38, BROW_ARCH = 26, BT = 30;

// Glanzpunkte relativ zur Irismitte
constexpr int HL1_X = -18, HL1_Y = -20, HL1_R = 11;
constexpr int HL2_X =  17, HL2_Y =  16, HL2_R = 5;
constexpr int STAR  = 26;    // halbe Groesse des Funkel-Sterns

struct Params {
  int   lookX, lookY;  // Blickrichtung in Pixeln
  int   pupil;         // Pupillenradius
  float open;          // Oberlid 0 = zu, 1 = ganz offen (inkl. Blinzeln)
  float smile;         // 0..1 Lachfalten: Unterlid hebt sich
  int   browDY;        // Augenbraue verschieben (negativ = hoch)
  int   twinkle;       // 0..256 Funkel-Stern
  int   dim = 256;     // Bildhelligkeit 0..256 (ersetzt die fehlende Displayhelligkeit)
};

// ---------- Tabellen ----------
static uint8_t  tex[TEX * TEX * 3];
static float    upBase[W], loBase[W], smileK[W];
static int16_t  browTop[W], browBot[W];
static int32_t  topQ[W], botQ[W];          // Lidkanten, Q8 (1/256 Pixel)
static int16_t  creaseY[W];
static uint8_t  sclR[W], sclG[W], sclB[W];
static uint16_t shTop[64], shBot[64];      // Schatten 0..256
static uint8_t  irisEdge[4 * IRIS_R + 8];  // Alpha am Irisrand ueber d2
static uint8_t  pupEdge[4 * 40 + 8];       // Alpha am Pupillenrand ueber d2
static uint8_t  hl1[(2 * HL1_R + 3) * (2 * HL1_R + 3)];
static uint8_t  hl2[(2 * HL2_R + 3) * (2 * HL2_R + 3)];
static uint8_t  star[(2 * STAR + 1) * (2 * STAR + 1)];
static Params   P;
static int      pupLo2, pupHi2;

#if SANTA_SKIN
// an das beige Druckmaterial des Kopfes angepasst (Foto: ca. 185/160/120);
// deutlich dunkler und weniger gelb als das Material, weil das Display selbst
// leuchtet und kraeftiger wirkt (200/168/124 war zu hell/gelb; Haut = der Ton,
// der vorher unten im Bäckchen-Verlauf lag und am besten zum Kopf passte)
constexpr uint8_t SK_R = 179, SK_G = 142, SK_B = 118;   // Haut
constexpr uint8_t LA_R =  80, LA_G =  58, LA_B =  44;   // Wimpernlinie
constexpr uint8_t CR_R = 148, CR_G = 114, CR_B =  94;   // Lidfalte
constexpr uint8_t CH_R = 186, CH_G = 138, CH_B = 116;   // Baeckchen (nur leicht waermer)
#else
constexpr uint8_t SK_R = 0, SK_G = 0, SK_B = 0;
constexpr uint8_t LA_R = 0, LA_G = 0, LA_B = 0;
constexpr uint8_t CR_R = 0, CR_G = 0, CR_B = 0;
constexpr uint8_t CH_R = 0, CH_G = 0, CH_B = 0;
#endif

static inline uint32_t hash(uint32_t x, uint32_t y) {
  uint32_t h = x * 374761393u + y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline uint16_t pack(int r, int g, int b) {
  uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return (uint16_t)((v >> 8) | (v << 8));
}
static inline int mix(int a, int b, int al) { return a + (((b - a) * al) >> 8); }  // al 0..256

// Kantenglaettung fuer Kreise: Alpha 0..256 fuer d2 im Bereich (r-1)^2..(r+1)^2
static void edgeLut(uint8_t* lut, int r, int& lo2, int& hi2) {
  lo2 = (r - 1) * (r - 1);
  hi2 = (r + 1) * (r + 1);
  for (int d2 = lo2; d2 <= hi2; d2++) {
    float a = clampf(r + 0.5f - sqrtf((float)d2), 0, 1);
    lut[d2 - lo2] = (uint8_t)(a * 255);
  }
}

static void circleMask(uint8_t* m, int r) {
  int n = 2 * r + 3, c = r + 1;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      float d = sqrtf((float)((x - c) * (x - c) + (y - c) * (y - c)));
      m[y * n + x] = (uint8_t)(clampf(r + 0.5f - d, 0, 1) * 255);
    }
}

static void buildIris() {
  for (int ty = 0; ty < TEX; ty++)
    for (int tx = 0; tx < TEX; tx++) {
      float dx = tx - TEXC, dy = ty - TEXC;
      float r = sqrtf(dx * dx + dy * dy) / IRIS_R;
      float a = atan2f(dy, dx);
      float s = 0.5f + 0.5f * (0.34f * sinf(23 * a + 1.3f + 2.0f * r)
                             + 0.26f * sinf(41 * a + 4.1f - 3.0f * r)
                             + 0.20f * sinf(67 * a + 2.2f + 5.0f * r)
                             + 0.20f * sinf(97 * a + 0.7f));
      s = s * s * (3 - 2 * s);
      float m = 0.30f + 0.70f * s * (1.0f - 0.25f * r);
      // freundliches, helles Himmelblau
      float R = 60 + (175 - 60) * m, G = 110 + (215 - 110) * m, B = 175 + (250 - 175) * m;
      float k = 1.0f;
      float coll = 0.50f + 0.035f * sinf(9 * a + 0.5f);
      if (fabsf(r - coll) < 0.05f) k *= 1.15f;                     // helle Krause
      if (r > 0.84f) k *= 1.0f - 0.70f * (r - 0.84f) / 0.16f;      // dunkler Rand
      if (r < 0.42f) k *= 0.85f + 0.15f * (r / 0.42f);
      uint8_t* p = &tex[(ty * TEX + tx) * 3];
      p[0] = (uint8_t)clampf(R * k, 0, 255);
      p[1] = (uint8_t)clampf(G * k, 0, 255);
      p[2] = (uint8_t)clampf(B * k, 0, 255);
    }
}

static void init() {
  for (int x = 0; x < W; x++) {
    float dx = (x + 0.5f - CX) / EYE_W;
    if (fabsf(dx) >= 1.0f) {
      upBase[x] = loBase[x] = CY; smileK[x] = 0;
      creaseY[x] = -100;
    } else {
      float s = 1.0f - dx * dx;
      upBase[x] = CY - UP_H * powf(s, 0.62f) * (1.0f + 0.05f * dx);
      loBase[x] = CY + LO_H * powf(s, 0.95f);
      smileK[x] = SM_H * powf(s, 0.7f);
      creaseY[x] = (int16_t)(CY - (UP_H + 13) * powf(s, 0.55f));
    }
    float k = dx * dx;
    sclR[x] = (uint8_t)clampf(248 - 40 * k, 0, 255);
    sclG[x] = (uint8_t)clampf(246 - 55 * k, 0, 255);
    sclB[x] = (uint8_t)clampf(244 - 60 * k, 0, 255);

    // Augenbraue: Bogen, aussen etwas tiefer, Rand zerzaust
    float bx = (x + 0.5f - BCX) / BW;
    if (fabsf(bx) < 1.0f) {
      float c  = BROW_Y + BROW_ARCH * bx * bx + 8 * (bx > 0 ? bx * bx : 0);
      float th = BT * (1.0f - 0.55f * bx * bx);
      float n1 = ((hash(x, 1) & 255) / 255.0f - 0.5f) * 7;
      float n2 = ((hash(x, 2) & 255) / 255.0f - 0.5f) * 5;
      browTop[x] = (int16_t)(c - th * 0.5f + n1);
      browBot[x] = (int16_t)(c + th * 0.5f + n2);
    } else {
      browTop[x] = browBot[x] = -1000;
    }
  }
  for (int i = 0; i < 64; i++) {
    shTop[i] = (uint16_t)(256 * (1.0f - 0.55f * expf(-i / 8.0f)));
    shBot[i] = (uint16_t)(256 * (1.0f - 0.30f * expf(-i / 5.0f)));
  }
  int lo, hi;
  edgeLut(irisEdge, IRIS_R, lo, hi);
  circleMask(hl1, HL1_R);
  circleMask(hl2, HL2_R);
  for (int y = -STAR; y <= STAR; y++)       // vierzackiger Stern mit Kern
    for (int x = -STAR; x <= STAR; x++) {
      float ax = fabsf((float)x) / STAR, ay = fabsf((float)y) / STAR;
      float ray = fmaxf(0, 1 - ax * 2.4f - ay) + fmaxf(0, 1 - ay * 2.4f - ax);
      float core = fmaxf(0, 1 - sqrtf(ax * ax + ay * ay) * 2.5f);
      ray = ray * 1.6f;
      star[(y + STAR) * (2 * STAR + 1) + x + STAR] = (uint8_t)(clampf(ray + core, 0, 1) * 255);
    }
  buildIris();
}

// einmal pro Frame
static void prepare(const Params& p) {
  P = p;
  float open = clampf(p.open, 0, 1) * (1.0f - 0.30f * p.smile);
  for (int x = 0; x < W; x++) {
    float lo = loBase[x] - smileK[x] * p.smile;
    float closeY = lo - 1.0f;
    float top = closeY + (upBase[x] - closeY) * open;
    if (top > lo) top = lo;
    topQ[x] = (int32_t)(top * 256);
    botQ[x] = (int32_t)(lo * 256);
  }
  int pr = p.pupil < 3 ? 3 : (p.pupil > 40 ? 40 : p.pupil);
  edgeLut(pupEdge, pr, pupLo2, pupHi2);
}

// Zeilen y0..y0+h-1 rendern; mirror = linkes Auge (horizontal gespiegelt)
static void renderStrip(uint16_t* buf, int y0, int h, bool mirror) {
  const int ix = CX + P.lookX, iy = CY + P.lookY;
  const int irLo2 = (IRIS_R - 1) * (IRIS_R - 1), irHi2 = (IRIS_R + 1) * (IRIS_R + 1);
  const int h1x = ix + HL1_X - HL1_R - 1, h1y = iy + HL1_Y - HL1_R - 1, h1n = 2 * HL1_R + 3;
  const int h2x = ix + HL2_X - HL2_R - 1, h2y = iy + HL2_Y - HL2_R - 1, h2n = 2 * HL2_R + 3;
  const int sx0 = ix + HL1_X - STAR, sy0 = iy + HL1_Y - STAR, sn = 2 * STAR + 1;

  for (int y = y0; y < y0 + h; y++) {
    uint16_t* row = buf + (y - y0) * W;
    const int32_t yQ = y * 256 + 128;
    for (int x = 0; x < W; x++) {
      int r, g, b;
      const int32_t tq = topQ[x], bq = botQ[x];
      const bool inside = (yQ + 128 > tq) && (yQ - 128 < bq) && (bq > tq);

      if (inside) {
        // ---------- Augapfel ----------
        r = sclR[x]; g = sclG[x]; b = sclB[x];
        const int dx = x - ix, dy = y - iy;
        const int d2 = dx * dx + dy * dy;
        if (d2 <= irHi2) {
          const uint8_t* tp = &tex[((dy + TEXC) * TEX + dx + TEXC) * 3];
          int ir = tp[0], ig = tp[1], ib = tp[2];
          if (d2 <= pupHi2) {
            int pa = d2 < pupLo2 ? 256 : pupEdge[d2 - pupLo2] + 1;
            ir = mix(ir, 12, pa); ig = mix(ig, 14, pa); ib = mix(ib, 28, pa);
          }
          int ia = d2 < irLo2 ? 256 : irisEdge[d2 - irLo2] + 1;
          r = mix(r, ir, ia); g = mix(g, ig, ia); b = mix(b, ib, ia);
        }
        // Schatten der Lider
        int it = (yQ - tq) >> 8, ib2 = (bq - yQ) >> 8;
        if (it < 0) it = 0; if (it > 63) it = 63;
        if (ib2 < 0) ib2 = 0; if (ib2 > 63) ib2 = 63;
        int sh = (shTop[it] * shBot[ib2]) >> 8;
        r = (r * sh) >> 8; g = (g * sh) >> 8; b = (b * sh) >> 8;

        // Glanzpunkte + Funkel-Stern
        int a = 0;
        if (x >= h1x && x < h1x + h1n && y >= h1y && y < h1y + h1n)
          a = hl1[(y - h1y) * h1n + (x - h1x)];
        if (x >= h2x && x < h2x + h2n && y >= h2y && y < h2y + h2n) {
          int a2 = hl2[(y - h2y) * h2n + (x - h2x)];
          if (a2 > a) a = a2;
        }
        if (d2 > irHi2) a = 0;  // Glanz nur auf der Iris
        if (P.twinkle && x >= sx0 && x < sx0 + sn && y >= sy0 && y < sy0 + sn) {
          int s = (star[(y - sy0) * sn + (x - sx0)] * P.twinkle) >> 8;
          if (s > a) a = s;
        }
        if (a) { a++; r = mix(r, 255, a); g = mix(g, 255, a); b = mix(b, 255, a); }

        // weiche Kanten an Ober- und Unterlid
        int ct = yQ + 128 - tq; if (ct > 256) ct = 256;
        int cb = bq - (yQ - 128); if (cb > 256) cb = 256;
        if (ct < 256) { r = mix(LA_R, r, ct); g = mix(LA_G, g, ct); b = mix(LA_B, b, ct); }
        if (cb < 256) { r = mix(CR_R, r, cb); g = mix(CR_G, g, cb); b = mix(CR_B, b, cb); }
      } else {
        // ---------- Haut / Hintergrund ----------
        r = SK_R; g = SK_G; b = SK_B;
#if SANTA_SKIN
        int32_t above = tq - yQ;                // Abstand ueber dem Oberlid (Q8)
        if (bq > tq && above > 0 && above < 3 * 256) {
          r = LA_R; g = LA_G; b = LA_B;         // Wimpernlinie
        } else if (bq > tq && yQ > bq && yQ < bq + 2 * 256) {
          r = CR_R; g = CR_G; b = CR_B;         // Unterlidkante
        } else if (y >= creaseY[x] - 1 && y <= creaseY[x] + 1 && yQ < tq) {
          r = CR_R; g = CR_G; b = CR_B;         // Lidfalte
        }
        if (y > CY + 30) {                      // rote Baeckchen nach unten hin
          int c = (y - CY - 30) * 4; if (c > 150) c = 150;
          r = mix(r, CH_R, c); g = mix(g, CH_G, c); b = mix(b, CH_B, c);
        }
#endif
#if SANTA_BROWS
        // Augenbraue
        int bt = browTop[x] + P.browDY, bb = browBot[x] + P.browDY;
        if (y >= bt && y <= bb) {
          uint32_t hs = hash(x + ((y - P.browDY) >> 1), 7);   // schraege Haarstraehnen
          int lum = 200 + (hs & 55);
          int al = 256;
          if (y - bt < 2 || bb - y < 2) al = 140;             // weicher Rand
          r = mix(r, lum, al); g = mix(g, lum, al); b = mix(b, lum + 4 > 255 ? 255 : lum + 4, al);
        }
#endif
      }
      if (P.dim < 256) { r = (r * P.dim) >> 8; g = (g * P.dim) >> 8; b = (b * P.dim) >> 8; }
      row[mirror ? W - 1 - x : x] = pack(r, g, b);
    }
  }
}

}  // namespace santa
