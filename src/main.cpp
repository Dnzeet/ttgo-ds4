// Tes kontroler Bluetooth (DualShock 4 dll.) untuk LilyGo T-Display (TTGO, ESP32 biasa)
// Bluepad32: ESP32 scan sendiri, DS4 dipairing dengan PS+Share.
//
// GPIO35 = klik: halaman berikutnya | dobel klik (< 450 ms): power off | nyala: tekan sekali
// GPIO0  = klik pendek, fungsi tergantung halaman:
//          TOMBOL: reset tes | TRIGGER: reset puncak | STIK: reset jejak
//          OUTPUT: ganti warna/mode | STATS: reset jeda maks
//          tahan 2 detik di STATUS: lupakan semua pairing

#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <TFT_eSPI.h>
#include <Bluepad32.h>

#define BTN_NEXT 35
#define BTN_MODE 0
#define BAT_ADC 34
#define BAT_EN 14

TFT_eSPI tft;
Preferences prefs;
ControllerPtr myControllers[BP32_MAX_GAMEPADS];

// ---------- warna ----------
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t C_BG = RGB(8, 10, 18);
static const uint16_t C_BAR = RGB(20, 26, 48);
static const uint16_t C_PANEL = RGB(24, 30, 52);
static const uint16_t C_OFF = RGB(52, 58, 82);
static const uint16_t C_DIM = RGB(90, 98, 124);
static const uint16_t C_TXT = RGB(232, 237, 247);
static const uint16_t C_SUB = RGB(140, 150, 175);
static const uint16_t C_ACC = RGB(0, 205, 255);
static const uint16_t C_OK = RGB(60, 225, 125);
static const uint16_t C_WARN = RGB(255, 190, 40);
static const uint16_t C_BAD = RGB(255, 75, 75);
static const uint16_t C_PINK = RGB(255, 130, 205);

// ---------- halaman ----------
enum { P_STATUS, P_BUTTONS, P_TRIGGER, P_STICK, P_SENSOR, P_OUTPUT, P_STATS, NPAGES };
static const char *TITLES[NPAGES] = {"STATUS", "TOMBOL", "TRIGGER", "STIK", "SENSOR", "OUTPUT", "STATS"};
static uint8_t page = 0;

// ---------- pilihan output: 10 warna default (disimpan) + 3 mode ----------
struct Preset { const char *name; uint8_t r, g, b; };
static const Preset PRESETS[] = {
  {"Biru", 0, 0, 255},      {"Merah", 255, 0, 0},   {"Hijau", 0, 255, 0},
  {"Ungu", 140, 0, 255},    {"Kuning", 255, 190, 0}, {"Cyan", 0, 255, 255},
  {"Putih", 255, 255, 255}, {"Oranye", 255, 70, 0},  {"Pink", 255, 0, 120},
  {"Mati", 0, 0, 0}};
static const uint8_t NPRESET = sizeof(PRESETS) / sizeof(PRESETS[0]);
static const uint8_t NSEL = NPRESET + 3;
static const char *MODE_NAMES[3] = {"Stik", "Pelangi", "Mati"};
static uint8_t sel = 0;
static uint8_t savedColor = 0;

// ---------- statistik ----------
static uint32_t evCount = 0, lastEvMs = 0, maxGap = 0;
static uint32_t connCount = 0, discCount = 0;
static uint32_t evPerSec = 0, prevEv = 0, prevEvT = 0;
static uint32_t forgetMsgUntil = 0;

// ---------- cache teks ----------
static char tc_s[24][48];
static uint16_t tc_c[24];

static void clearTextCache() {
  memset(tc_s, 0, sizeof tc_s);
  memset(tc_c, 0, sizeof tc_c);
}

static void put(uint8_t id, int x, int y, uint8_t font, uint8_t datum, uint16_t fg, uint16_t bg, int pad,
                const char *fmt, ...) {
  char b[48];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  if (tc_c[id] == fg && strcmp(tc_s[id], b) == 0) return;
  strcpy(tc_s[id], b);
  tc_c[id] = fg;
  tft.setTextDatum(datum);
  tft.setTextColor(fg, bg);
  tft.setTextPadding(pad);
  tft.drawString(b, x, y, font);
}

// ---------- bar dengan update selisih (halus, tanpa kedip) ----------
static void hbar(int x, int y, int w, int h, int &prev, int val, int vmax, uint16_t col, uint16_t bg) {
  int nw = (int)((long)w * val / vmax);
  if (nw < 0) nw = 0;
  if (nw > w) nw = w;
  if (prev < 0) {
    tft.fillRect(x, y, w, h, bg);
    if (nw) tft.fillRect(x, y, nw, h, col);
    prev = nw;
    return;
  }
  if (nw == prev) return;
  if (nw > prev) tft.fillRect(x + prev, y, nw - prev, h, col);
  else tft.fillRect(x + nw, y, prev - nw, h, bg);
  prev = nw;
}

static void vbar(int x, int y, int w, int h, int &prev, int val, int vmax, uint16_t col, uint16_t bg) {
  int nh = (int)((long)h * val / vmax);
  if (nh < 0) nh = 0;
  if (nh > h) nh = h;
  if (prev < 0) {
    tft.fillRect(x, y, w, h, bg);
    if (nh) tft.fillRect(x, y + h - nh, w, nh, col);
    prev = nh;
    return;
  }
  if (nh == prev) return;
  if (nh > prev) tft.fillRect(x, y + h - nh, w, nh - prev, col);
  else tft.fillRect(x, y + h - prev, w, prev - nh, bg);
  prev = nh;
}

// ---------- kontroler ----------
void onConnectedController(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++)
    if (myControllers[i] == ctl) return;  // sudah tercatat
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == nullptr) {
      myControllers[i] = ctl;
      connCount++;
      return;
    }
  }
}

void onDisconnectedController(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == ctl) {
      myControllers[i] = nullptr;
      discCount++;
      return;
    }
  }
}

// Kontroler aktif. Tidak lagi memakai hasData(), dan ada masa tenggang 1,5 dtk
// supaya status tidak berkedip "belum ada kontroler" saat terhubung.
static ControllerPtr getCtl() {
  static ControllerPtr last = nullptr;
  static uint32_t lastSeen = 0;
  uint32_t now = millis();
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    ControllerPtr c = myControllers[i];
    if (c && c->isConnected()) {
      last = c;
      lastSeen = now;
      return c;
    }
  }
  if (last && now - lastSeen < 1500) return last;
  return nullptr;
}

static int connectedCount() {
  int n = 0;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++)
    if (myControllers[i] && myControllers[i]->isConnected()) n++;
  return n;
}

// ---------- output: LED & rumble ----------
static void hue(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (h < 85) { r = 255 - h * 3; g = h * 3; b = 0; }
  else if (h < 170) { h -= 85; r = 0; g = 255 - h * 3; b = h * 3; }
  else { h -= 170; r = h * 3; g = 0; b = 255 - h * 3; }
}

static uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void applyOutput(ControllerPtr c) {
  static uint8_t prevLed[3] = {1, 1, 1};
  static uint32_t lastLed = 0, lastRumble = 0, connectedAt = 0, lastForce = 0;
  static ControllerPtr lastC = nullptr;

  uint32_t now = millis();
  if (!c) { lastC = nullptr; return; }
  if (c != lastC) {  // kontroler baru: kirim warna default
    lastC = c;
    connectedAt = now;
    memset(prevLed, 0xFF, sizeof prevLed);
  }

  uint8_t r = 0, g = 0, b = 0, weak = 0, strong = 0;
  if (sel < NPRESET) {
    r = PRESETS[sel].r; g = PRESETS[sel].g; b = PRESETS[sel].b;
  } else if (sel == NPRESET) {
    r = clamp8((c->axisX() + 512) / 4);
    g = clamp8((c->axisY() + 512) / 4);
    b = clamp8(c->throttle() / 4);
    weak = clamp8(c->throttle() / 4);
    strong = clamp8(c->brake() / 4);
  } else if (sel == NPRESET + 1) {
    hue((now / 12) & 0xFF, r, g, b);
  }

  if (now - lastLed >= 50) {
    lastLed = now;
    uint8_t cur[3] = {r, g, b};
    bool force = (now - connectedAt < 2500) && (now - lastForce >= 300);
    if (force || memcmp(cur, prevLed, sizeof cur) != 0) {
      c->setColorLED(r, g, b);
      memcpy(prevLed, cur, sizeof cur);
      if (force) lastForce = now;
    }
  }
  if ((weak || strong) && now - lastRumble >= 100) {
    c->playDualRumble(0, 150, weak, strong);
    lastRumble = now;
  }
}

// ---------- tombol 18 buah ----------
enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_CROSS, B_CIRCLE, B_SQUARE, B_TRI, B_L1, B_R1, B_L2, B_R2,
       B_L3, B_R3, B_SHARE, B_OPT, B_PS, B_TP, NB };
static bool btested[NB];
static int8_t bdrawn[NB];
static int pv_l2 = -1, pv_r2 = -1;
static int sdx[2] = {-999, -999}, sdy[2];

static bool pressedNow(ControllerPtr c, int i) {
  uint8_t d = c->dpad();
  uint8_t m = c->miscButtons();
  switch (i) {
    case B_UP: return d & 0x01;
    case B_DOWN: return d & 0x02;
    case B_RIGHT: return d & 0x04;
    case B_LEFT: return d & 0x08;
    case B_CROSS: return c->a();
    case B_CIRCLE: return c->b();
    case B_SQUARE: return c->x();
    case B_TRI: return c->y();
    case B_L1: return c->l1();
    case B_R1: return c->r1();
    case B_L2: return c->l2() || c->brake() > 700;
    case B_R2: return c->r2() || c->throttle() > 700;
    case B_L3: return c->thumbL();
    case B_R3: return c->thumbR();
    case B_PS: return m & 0x01;
    case B_SHARE: return m & 0x02;
    case B_OPT: return m & 0x04;
    case B_TP: return m & 0x08;  // klik touchpad (pemetaan bit perlu dicek di perangkat)
  }
  return false;
}

static uint16_t btnBase(int i) {
  switch (i) {
    case B_CROSS: return RGB(90, 150, 255);
    case B_CIRCLE: return RGB(255, 90, 90);
    case B_SQUARE: return C_PINK;
    case B_TRI: return RGB(70, 225, 150);
    default: return C_ACC;
  }
}

static void faceShape(int i, int cx, int cy, uint16_t col) {
  switch (i) {
    case B_TRI: tft.fillTriangle(cx, cy - 5, cx - 5, cy + 4, cx + 5, cy + 4, col); break;
    case B_CROSS:
      tft.drawLine(cx - 4, cy - 4, cx + 4, cy + 4, col);
      tft.drawLine(cx - 4, cy + 4, cx + 4, cy - 4, col);
      tft.drawLine(cx - 3, cy - 4, cx + 5, cy + 4, col);
      tft.drawLine(cx - 3, cy + 4, cx + 5, cy - 4, col);
      break;
    case B_SQUARE:
      tft.drawRect(cx - 4, cy - 4, 9, 9, col);
      tft.drawRect(cx - 3, cy - 3, 7, 7, col);
      break;
    case B_CIRCLE:
      tft.drawCircle(cx, cy, 5, col);
      tft.drawCircle(cx, cy, 4, col);
      break;
  }
}

// st: 0 = belum diuji, 1 = sedang ditekan, 2 = sudah teruji
static void drawButton(int i, int st) {
  uint16_t base = btnBase(i);
  uint16_t ln = st == 0 ? C_OFF : base;
  uint16_t fill = st == 1 ? base : C_BG;
  uint16_t shape = st == 0 ? C_OFF : (st == 1 ? C_BG : base);
  tft.setTextPadding(0);
  switch (i) {
    case B_UP: case B_DOWN: case B_LEFT: case B_RIGHT: {
      int x, y;
      if (i == B_UP) { x = 43; y = 57; }
      else if (i == B_DOWN) { x = 43; y = 87; }
      else if (i == B_LEFT) { x = 28; y = 72; }
      else { x = 58; y = 72; }
      tft.fillRoundRect(x, y, 15, 15, 3, fill);
      tft.drawRoundRect(x, y, 15, 15, 3, ln);
      int cx = x + 7, cy = y + 7;
      if (i == B_UP) tft.fillTriangle(cx, cy - 3, cx - 3, cy + 2, cx + 3, cy + 2, shape);
      else if (i == B_DOWN) tft.fillTriangle(cx, cy + 3, cx - 3, cy - 2, cx + 3, cy - 2, shape);
      else if (i == B_LEFT) tft.fillTriangle(cx - 3, cy, cx + 2, cy - 3, cx + 2, cy + 3, shape);
      else tft.fillTriangle(cx + 3, cy, cx - 2, cy - 3, cx - 2, cy + 3, shape);
      break;
    }
    case B_TRI: case B_CROSS: case B_SQUARE: case B_CIRCLE: {
      int cx, cy;
      if (i == B_TRI) { cx = 190; cy = 62; }
      else if (i == B_CROSS) { cx = 190; cy = 96; }
      else if (i == B_SQUARE) { cx = 173; cy = 79; }
      else { cx = 207; cy = 79; }
      tft.fillCircle(cx, cy, 10, fill);
      tft.drawCircle(cx, cy, 10, ln);
      faceShape(i, cx, cy, shape);
      break;
    }
    case B_L1: case B_R1: {
      int x = i == B_L1 ? 6 : 172;
      tft.fillRoundRect(x, 35, 62, 11, 4, fill);
      tft.drawRoundRect(x, 35, 62, 11, 4, ln);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(shape, fill);
      tft.drawString(i == B_L1 ? "L1" : "R1", x + 31, 41, 1);
      break;
    }
    case B_L2: case B_R2: {
      int x = i == B_L2 ? 21 : 171;
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(C_SUB, C_BG);
      tft.drawString(i == B_L2 ? "L2" : "R2", i == B_L2 ? 6 : 222, 24, 1);
      tft.drawRect(x, 23, 48, 10, ln);
      break;
    }
    case B_L3: case B_R3: {
      int cx = i == B_L3 ? 92 : 148;
      tft.drawCircle(cx, 112, 17, ln);
      tft.drawCircle(cx, 112, 16, ln);
      tft.drawCircle(cx, 112, 15, st == 1 ? base : C_BG);
      break;
    }
    case B_SHARE: case B_OPT: {
      int x = i == B_SHARE ? 76 : 154;
      tft.fillRoundRect(x, 30, 10, 16, 3, fill);
      tft.drawRoundRect(x, 30, 10, 16, 3, ln);
      tft.setTextDatum(TC_DATUM);
      tft.setTextColor(C_SUB, C_BG);
      tft.drawString(i == B_SHARE ? "SHR" : "OPT", x + 5, 48, 1);
      break;
    }
    case B_PS:
      tft.fillCircle(120, 64, 9, fill);
      tft.drawCircle(120, 64, 9, ln);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(shape, fill);
      tft.drawString("PS", 120, 64, 1);
      break;
    case B_TP:
      tft.fillRoundRect(90, 24, 60, 26, 5, fill);
      tft.drawRoundRect(90, 24, 60, 26, 5, ln);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(shape, fill);
      tft.drawString("TOUCHPAD", 120, 37, 1);
      break;
  }
}

static void pageButtons(ControllerPtr c) {
  int cnt = 0;
  for (int i = 0; i < NB; i++) {
    bool p = c ? pressedNow(c, i) : false;
    if (p) btested[i] = true;
    int st = p ? 1 : (btested[i] ? 2 : 0);
    if (st != bdrawn[i]) {
      drawButton(i, st);
      bdrawn[i] = st;
      if (i == B_L3) sdx[0] = -999;
      if (i == B_R3) sdx[1] = -999;
    }
    if (btested[i]) cnt++;
  }
  hbar(22, 24, 46, 8, pv_l2, c ? c->brake() : 0, 1023, C_WARN, C_BAR);
  hbar(172, 24, 46, 8, pv_r2, c ? c->throttle() : 0, 1023, C_WARN, C_BAR);

  for (int s = 0; s < 2; s++) {
    int ax = c ? (s ? c->axisRX() : c->axisX()) : 0;
    int ay = c ? (s ? c->axisRY() : c->axisY()) : 0;
    float nx = ax / 512.0f, ny = ay / 512.0f;
    float r = sqrtf(nx * nx + ny * ny);
    if (r > 1.0f) { nx /= r; ny /= r; }
    int cx = s ? 148 : 92;
    int px = cx + (int)(nx * 10), py = 112 + (int)(ny * 10);
    if (px != sdx[s] || py != sdy[s]) {
      if (sdx[s] != -999) tft.fillCircle(sdx[s], sdy[s], 4, C_BG);
      tft.fillCircle(px, py, 3, C_TXT);
      sdx[s] = px;
      sdy[s] = py;
    }
  }

  put(1, 6, 106, 1, TL_DATUM, C_SUB, C_BG, 0, "TERUJI");
  if (c) put(0, 6, 116, 2, TL_DATUM, cnt == NB ? C_OK : C_TXT, C_BG, 64, "%d/%d", cnt, NB);
  else put(0, 6, 116, 2, TL_DATUM, C_WARN, C_BG, 64, "Tunggu..");
  put(2, 172, 120, 1, TL_DATUM, C_SUB, C_BG, 0, "GPIO0: reset");
}

// ---------- halaman TRIGGER ----------
static int pv_tl = -1, pv_tr = -1;
static int peakL = 0, peakR = 0;

static void pageTrigger(ControllerPtr c) {
  int l = c ? c->brake() : 0, r = c ? c->throttle() : 0;
  if (l > peakL) peakL = l;
  if (r > peakR) peakR = r;
  vbar(24, 70, 56, 58, pv_tl, l, 1023, C_ACC, C_BAR);
  vbar(160, 70, 56, 58, pv_tr, r, 1023, C_PINK, C_BAR);
  if (peakL > 0) tft.drawFastHLine(24, 70 + 58 - (int)(58L * peakL / 1023), 56, C_TXT);
  if (peakR > 0) tft.drawFastHLine(160, 70 + 58 - (int)(58L * peakR / 1023), 56, C_TXT);

  put(2, 52, 22, 2, TC_DATUM, C_SUB, C_BG, 0, "L2");
  put(3, 188, 22, 2, TC_DATUM, C_SUB, C_BG, 0, "R2");
  put(0, 52, 40, 4, TC_DATUM, C_TXT, C_BG, 72, "%d%%", (int)(l * 100L / 1023));
  put(1, 188, 40, 4, TC_DATUM, C_TXT, C_BG, 72, "%d%%", (int)(r * 100L / 1023));
  put(4, 120, 72, 1, TC_DATUM, C_SUB, C_BG, 0, "PUNCAK");
  put(5, 120, 86, 2, TC_DATUM, C_WARN, C_BG, 76, "%d%% %d%%", (int)(peakL * 100L / 1023), (int)(peakR * 100L / 1023));
  put(6, 120, 108, 1, TC_DATUM, C_SUB, C_BG, 76, "%d | %d", l, r);
  put(7, 120, 122, 1, TC_DATUM, C_SUB, C_BG, 0, "GPIO0: reset");
}

// ---------- halaman STIK ----------
static uint8_t trace_[2][48];
static float cosT[48], sinT[48];
static int spx[2] = {-999, -999}, spy[2];

static void stickBase(int s) {
  int cx = s ? 178 : 62, cy = 73;
  tft.drawCircle(cx, cy, 40, C_DIM);
  tft.drawCircle(cx, cy, 20, C_OFF);
  tft.drawFastHLine(cx - 40, cy, 81, C_OFF);
  tft.drawFastVLine(cx, cy - 40, 81, C_OFF);
}

static void stickTrace(int s) {
  int cx = s ? 178 : 62, cy = 73;
  for (int b = 0; b < 48; b++) {
    if (!trace_[s][b]) continue;
    float r = trace_[s][b] / 100.0f * 40.0f;
    tft.fillRect(cx + (int)(cosT[b] * r) - 1, cy + (int)(sinT[b] * r) - 1, 3, 3, C_OK);
  }
}

static void pageStick(ControllerPtr c) {
  for (int s = 0; s < 2; s++) {
    int cx = s ? 178 : 62, cy = 73;
    int ax = c ? (s ? c->axisRX() : c->axisX()) : 0;
    int ay = c ? (s ? c->axisRY() : c->axisY()) : 0;
    float nx = ax / 512.0f, ny = ay / 512.0f;
    float r = sqrtf(nx * nx + ny * ny);
    float k = r > 1.1f ? 1.1f / r : 1.0f;
    int px = cx + (int)(nx * k * 40), py = cy + (int)(ny * k * 40);
    bool newTrace = false;
    if (r > 0.3f) {
      int b = (int)((atan2f(ny, nx) + PI) * 48.0f / (2.0f * PI));
      if (b > 47) b = 47;
      if (b < 0) b = 0;
      uint8_t v = (uint8_t)(fminf(r, 1.1f) * 100.0f);
      if (v > trace_[s][b]) { trace_[s][b] = v; newTrace = true; }
    }
    if (px != spx[s] || py != spy[s] || newTrace) {
      if (spx[s] != -999) tft.fillCircle(spx[s], spy[s], 4, C_BG);
      stickBase(s);
      stickTrace(s);
      tft.fillCircle(px, py, 3, C_TXT);
      spx[s] = px;
      spy[s] = py;
    }
    put(s, cx, 119, 2, TC_DATUM, C_TXT, C_BG, 112, "%4d %4d", ax, ay);
  }
  put(2, 62, 24, 1, TC_DATUM, C_SUB, C_BG, 0, "STIK KIRI");
  put(3, 178, 24, 1, TC_DATUM, C_SUB, C_BG, 0, "STIK KANAN");
  put(4, 120, 56, 1, TC_DATUM, C_SUB, C_BG, 0, "GPIO0");
  put(5, 120, 66, 1, TC_DATUM, C_SUB, C_BG, 0, "reset");
}

// ---------- halaman SENSOR ----------
static int pvs[6];

static void centerBar(int row, int val, int range, uint16_t col) {
  int y = 24 + row * 17;
  int off = (int)((long)val * 80 / range);
  if (off > 80) off = 80;
  if (off < -80) off = -80;
  if (off == pvs[row]) return;
  pvs[row] = off;
  tft.fillRect(30, y, 160, 11, C_BAR);
  if (off > 0) tft.fillRect(110, y, off, 11, col);
  else if (off < 0) tft.fillRect(110 + off, y, -off, 11, col);
  tft.drawFastVLine(110, y, 11, C_TXT);
}

static void pageSensor(ControllerPtr c) {
  static uint32_t last = 0;
  if (millis() - last < 30) return;
  last = millis();
  static const char *L[6] = {"GX", "GY", "GZ", "AX", "AY", "AZ"};
  long v[6] = {0, 0, 0, 0, 0, 0};
  if (c) {
    v[0] = c->gyroX(); v[1] = c->gyroY(); v[2] = c->gyroZ();
    v[3] = c->accelX(); v[4] = c->accelY(); v[5] = c->accelZ();
  }
  for (int i = 0; i < 6; i++) {
    int y = 24 + i * 17;
    put(i, 4, y, 2, TL_DATUM, C_SUB, C_BG, 0, "%s", L[i]);
    centerBar(i, v[i], i < 3 ? 12000 : 8192, i < 3 ? C_ACC : C_PINK);
    put(6 + i, 236, y, 2, TR_DATUM, C_TXT, C_BG, 44, "%ld", v[i]);
  }
}

// ---------- halaman OUTPUT ----------
static uint8_t drawnSel = 255;

static void drawOutputUI() {
  for (int i = 0; i < NPRESET; i++) {
    int x = 8 + (i % 5) * 46, y = 26 + (i / 5) * 30;
    tft.fillRoundRect(x, y, 40, 24, 4, tft.color565(PRESETS[i].r, PRESETS[i].g, PRESETS[i].b));
    tft.drawRoundRect(x, y, 40, 24, 4, C_DIM);
    tft.drawRoundRect(x - 2, y - 2, 44, 28, 5, i == sel ? C_TXT : C_BG);
    tft.drawRoundRect(x - 3, y - 3, 46, 30, 6, i == sel ? C_TXT : C_BG);
    if (i == savedColor) tft.fillCircle(x + 34, y + 18, 3, i == 9 ? C_TXT : C_BG);
  }
  for (int k = 0; k < 3; k++) {
    int x = 8 + k * 76;
    bool on = (sel == NPRESET + k);
    tft.fillRoundRect(x, 104, 70, 20, 5, on ? C_ACC : C_BAR);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(on ? C_BG : C_SUB, on ? C_ACC : C_BAR);
    tft.setTextPadding(0);
    tft.drawString(MODE_NAMES[k], x + 35, 114, 2);
  }
}

static void pageOutput(ControllerPtr c) {
  if (drawnSel != sel) {
    drawnSel = sel;
    drawOutputUI();
  }
  if (sel < NPRESET) put(0, 8, 84, 2, TL_DATUM, C_OK, C_BG, 224, "%s (tersimpan)", PRESETS[sel].name);
  else put(0, 8, 84, 2, TL_DATUM, C_ACC, C_BG, 224, "Mode %s", MODE_NAMES[sel - NPRESET]);
  if (sel == NPRESET) put(1, 6, 127, 1, TL_DATUM, C_SUB, C_BG, 230, "RGB: stik kiri+R2 | rumble: R2/L2");
  else put(1, 6, 127, 1, TL_DATUM, C_SUB, C_BG, 230, "GPIO0: ganti | otomatis tersimpan");
}

// ---------- halaman STATS ----------
static void pageStats(ControllerPtr c) {
  static const char *L[6] = {"Update/detik", "Jeda update maks", "Total update", "Heap bebas", "Heap minimum", "Uptime"};
  uint32_t up = millis() / 1000;
  for (int i = 0; i < 6; i++) put(i, 8, 26 + i * 17, 2, TL_DATUM, C_SUB, C_BG, 0, "%s", L[i]);
  put(6, 232, 26, 2, TR_DATUM, C_OK, C_BG, 70, "%lu", (unsigned long)evPerSec);
  put(7, 232, 43, 2, TR_DATUM, maxGap > 50 ? C_WARN : C_OK, C_BG, 70, "%lu ms", (unsigned long)maxGap);
  put(8, 232, 60, 2, TR_DATUM, C_TXT, C_BG, 90, "%lu", (unsigned long)evCount);
  put(9, 232, 77, 2, TR_DATUM, C_TXT, C_BG, 90, "%u", (unsigned)ESP.getFreeHeap());
  put(10, 232, 94, 2, TR_DATUM, C_TXT, C_BG, 90, "%u", (unsigned)ESP.getMinFreeHeap());
  put(11, 232, 111, 2, TR_DATUM, C_TXT, C_BG, 90, "%lum %02lus", (unsigned long)(up / 60), (unsigned long)(up % 60));
}

// ---------- halaman STATUS ----------
static int st_shown = -1, pv_bat = -1;
static int dotX = -999, dotY;

static int readBatteryMv() {
  digitalWrite(BAT_EN, HIGH);
  delay(5);
  int mv = analogReadMilliVolts(BAT_ADC) * 2;
  digitalWrite(BAT_EN, LOW);
  return mv > 2800 ? mv : 0;
}

static void drawStatusCard2() {
  tft.fillRoundRect(6, 80, 228, 54, 6, C_PANEL);
}

static void pageStatus(ControllerPtr c) {
  static uint32_t lastBat = 0, lastAnim = 0;
  static int batMv = -1;
  int state = c ? 1 : 0;

  if (state != st_shown) {
    st_shown = state;
    tft.fillRoundRect(6, 24, 228, 52, 6, C_PANEL);
    for (int i = 0; i < 4; i++) tc_s[i][0] = 0;
    pv_bat = -1;
    dotX = -999;
    if (state) {
      tft.fillCircle(28, 50, 12, C_OK);
      for (int d = 0; d < 2; d++) {
        tft.drawLine(22, 50 + d, 26, 55 + d, C_BG);
        tft.drawLine(26, 55 + d, 34, 44 + d, C_BG);
      }
      tft.drawRoundRect(132, 58, 56, 10, 2, C_SUB);
    } else {
      tft.drawCircle(28, 50, 12, C_WARN);
    }
  }

  if (state) {
    put(0, 50, 28, 4, TL_DATUM, C_TXT, C_PANEL, 176, "%.12s", c->getModelName().c_str());
    put(1, 50, 56, 2, TL_DATUM, C_OK, C_PANEL, 76, "Terhubung");
    int pct = c->battery() * 100 / 255;
    hbar(133, 59, 54, 8, pv_bat, pct, 100, pct > 50 ? C_OK : (pct > 20 ? C_WARN : C_BAD), C_BAR);
    put(2, 196, 56, 2, TL_DATUM, C_TXT, C_PANEL, 36, "%d%%", pct);
  } else {
    put(0, 50, 28, 4, TL_DATUM, C_TXT, C_PANEL, 176, "Mencari...");
    put(1, 50, 56, 2, TL_DATUM, C_WARN, C_PANEL, 176, "Tahan PS+Share di DS4");
    if (millis() - lastAnim > 40) {
      lastAnim = millis();
      float a = (millis() / 6 % 360) * 0.0174533f;
      int nx = 28 + (int)(cosf(a) * 12), ny = 50 + (int)(sinf(a) * 12);
      if (nx != dotX || ny != dotY) {
        if (dotX != -999) tft.fillCircle(dotX, dotY, 3, C_PANEL);
        tft.drawCircle(28, 50, 12, C_WARN);
        tft.fillCircle(nx, ny, 3, C_WARN);
        dotX = nx;
        dotY = ny;
      }
    }
  }

  if (batMv < 0 || millis() - lastBat > 5000) { batMv = readBatteryMv(); lastBat = millis(); }
  put(4, 14, 84, 2, TL_DATUM, C_SUB, C_PANEL, 120, "Baterai TTGO");
  if (batMv) put(5, 226, 84, 2, TR_DATUM, C_TXT, C_PANEL, 90, "%d.%02d V", batMv / 1000, (batMv % 1000) / 10);
  else put(5, 226, 84, 2, TR_DATUM, C_SUB, C_PANEL, 90, "--");
  put(6, 14, 100, 2, TL_DATUM, C_SUB, C_PANEL, 120, "Konek / Putus");
  put(7, 226, 100, 2, TR_DATUM, C_TXT, C_PANEL, 90, "%lu / %lu", (unsigned long)connCount, (unsigned long)discCount);
  if (millis() < forgetMsgUntil) put(8, 14, 118, 1, TL_DATUM, C_WARN, C_PANEL, 210, "Pairing dihapus!");
  else put(8, 14, 118, 1, TL_DATUM, C_SUB, C_PANEL, 210, "GPIO0 tahan 2 dtk: lupakan pairing");
  put(9, 14, 125, 1, TL_DATUM, C_SUB, C_PANEL, 210, "GPIO35 klik: halaman | x2: power off");
}

// ---------- top bar & header ----------
static void drawTopRight(ControllerPtr c) {
  tft.fillRect(178, 1, 62, 18, C_BAR);
  if (c) {
    tft.fillCircle(188, 10, 4, C_OK);
    int pct = c->battery() * 100 / 255;
    uint16_t col = pct > 50 ? C_OK : (pct > 20 ? C_WARN : C_BAD);
    tft.drawRect(198, 6, 26, 10, C_SUB);
    tft.fillRect(224, 9, 2, 4, C_SUB);
    tft.fillRect(199, 7, pct * 24 / 100, 8, col);
  } else {
    tft.drawCircle(188, 10, 4, C_DIM);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_DIM, C_BAR);
    tft.setTextPadding(0);
    tft.drawString("--", 200, 6, 1);
  }
}

static void refreshTopBar(ControllerPtr c) {
  static int lastKey = -2;
  int key = c ? (c->battery() * 100 / 255) / 5 : -1;
  if (key == lastKey) return;
  lastKey = key;
  drawTopRight(c);
}

void header() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 240, 19, C_BAR);
  tft.fillRect(0, 19, 240, 1, C_ACC);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TXT, C_BAR);
  tft.setTextPadding(0);
  tft.drawString(TITLES[page], 6, 2, 2);
  for (int i = 0; i < NPAGES; i++) {
    int x = 112 + i * 9;
    if (i == page) tft.fillCircle(x, 10, 3, C_ACC);
    else tft.fillCircle(x, 10, 2, C_DIM);
  }
  drawTopRight(getCtl());

  clearTextCache();
  switch (page) {
    case P_STATUS:
      st_shown = -1;
      drawStatusCard2();
      break;
    case P_BUTTONS:
      for (int i = 0; i < NB; i++) bdrawn[i] = -1;
      pv_l2 = pv_r2 = -1;
      sdx[0] = sdx[1] = -999;
      break;
    case P_TRIGGER:
      pv_tl = pv_tr = -1;
      tft.drawRect(23, 69, 58, 60, C_DIM);
      tft.drawRect(159, 69, 58, 60, C_DIM);
      break;
    case P_STICK:
      spx[0] = spx[1] = -999;
      stickBase(0);
      stickBase(1);
      break;
    case P_SENSOR:
      for (int i = 0; i < 6; i++) pvs[i] = 9999;
      break;
    case P_OUTPUT:
      drawnSel = 255;
      break;
  }
}

static void drawPage(ControllerPtr c) {
  switch (page) {
    case P_STATUS: pageStatus(c); break;
    case P_BUTTONS: pageButtons(c); break;
    case P_TRIGGER: pageTrigger(c); break;
    case P_STICK: pageStick(c); break;
    case P_SENSOR: pageSensor(c); break;
    case P_OUTPUT: pageOutput(c); break;
    case P_STATS: pageStats(c); break;
  }
}

// ---------- tombol fisik & power ----------
void powerOff() {
  while (!digitalRead(BTN_NEXT)) delay(10);  // tunggu tombol dilepas
  delay(80);
  digitalWrite(TFT_BL, LOW);
  tft.writecommand(TFT_DISPOFF);
  tft.writecommand(TFT_SLPIN);
  delay(120);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 0);
  esp_deep_sleep_start();
}

void handleButtons() {
  // status awal dibaca ulang saat boot, jadi klik pembangun tidak terhitung
  static bool lastN = digitalRead(BTN_NEXT), lastM = digitalRead(BTN_MODE);
  static uint32_t edgeN = 0, firstClick = 0, pressM = 0;
  static bool longDone = false;
  uint32_t now = millis();
  bool n = digitalRead(BTN_NEXT), m = digitalRead(BTN_MODE);

  if (!n && lastN && now - edgeN > 30) {
    edgeN = now;
    if (firstClick && now - firstClick < 450) {
      powerOff();  // dobel klik
    } else {
      firstClick = now;  // klik pertama tetap langsung aksi
      page = (page + 1) % NPAGES;
      header();
    }
  }

  if (!m && lastM) { pressM = now; longDone = false; }
  if (!m && !longDone && page == P_STATUS && now - pressM >= 2000) {
    longDone = true;
    BP32.forgetBluetoothKeys();
    forgetMsgUntil = now + 3000;
  }
  if (m && !lastM && !longDone && now - pressM > 30 && now - pressM < 1000) {
    switch (page) {
      case P_BUTTONS: memset(btested, 0, sizeof btested); header(); break;
      case P_TRIGGER: peakL = peakR = 0; header(); break;
      case P_STICK: memset(trace_, 0, sizeof trace_); header(); break;
      case P_STATS: maxGap = 0; break;
      case P_OUTPUT:
        sel = (sel + 1) % NSEL;
        if (sel < NPRESET && sel != savedColor) {  // simpan warna default ke flash
          savedColor = sel;
          prefs.putUChar("col", savedColor);
        }
        break;
    }
  }
  lastN = n;
  lastM = m;
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_NEXT, INPUT);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BAT_EN, OUTPUT);
  digitalWrite(BAT_EN, LOW);

  for (int b = 0; b < 48; b++) {
    float a = (b + 0.5f) * 2.0f * PI / 48.0f - PI;
    cosT[b] = cosf(a);
    sinT[b] = sinf(a);
  }

  prefs.begin("ds4t", false);
  savedColor = prefs.getUChar("col", 0);
  if (savedColor >= NPRESET) savedColor = 0;
  sel = savedColor;

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  Serial.printf("Bluepad32 firmware: %s\n", BP32.firmwareVersion());
  BP32.setup(&onConnectedController, &onDisconnectedController);
  BP32.enableVirtualDevice(false);

  header();
}

void loop() {
  static uint32_t lastDraw = 0, lastTop = 0;
  uint32_t now = millis();

  if (BP32.update()) {
    if (lastEvMs && now - lastEvMs > maxGap && connectedCount()) maxGap = now - lastEvMs;
    lastEvMs = now;
    evCount++;
  }

  handleButtons();
  ControllerPtr c = getCtl();
  applyOutput(c);

  if (now - prevEvT >= 1000) {
    evPerSec = (evCount - prevEv) * 1000UL / (now - prevEvT);
    prevEv = evCount;
    prevEvT = now;
  }
  if (now - lastDraw >= 8) {
    lastDraw = now;
    drawPage(c);
  }
  if (now - lastTop >= 400) {
    lastTop = now;
    refreshTopBar(c);
  }
  vTaskDelay(1);
}
