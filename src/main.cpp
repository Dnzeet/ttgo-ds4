// Tes kontroler Bluetooth (DualShock 4 dll.) untuk LilyGo T-Display (TTGO, ESP32 biasa)
// Bluepad32: ESP32 scan sendiri, DS4 dipairing dengan PS+Share.
//
// Tombol bawah (GPIO0): klik = halaman berikutnya, tahan = halaman sebelumnya
// Tombol atas (GPIO35): pilih / aksi di halaman:
//   TOMBOL: reset tes | TRIGGER: reset puncak | STIK: reset jejak | OUTPUT: ganti warna/mode
//   STATUS: ganti kontroler (tahan 2 dtk: lupakan pairing) | GETAR: ganti pola
//   SETELAN: kecerahan +10% / bahasa (tahan: pindah baris) | STATS: reset jeda maks
//   dobel klik (< 450 ms): power off | menyalakan: tekan sekali

#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <TFT_eSPI.h>
#include <Bluepad32.h>

#define BTN_TOP 35
#define BTN_BOTTOM 0
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
enum { P_STATUS, P_BUTTONS, P_TRIGGER, P_STICK, P_SENSOR, P_VIBRATE, P_OUTPUT, P_SETTINGS, P_STATS, NPAGES };
static const char *TITLES_ID[NPAGES] = {"STATUS", "TOMBOL", "TRIGGER", "STIK", "SENSOR", "GETAR", "OUTPUT", "SETELAN", "STATS"};
static const char *TITLES_EN[NPAGES] = {"STATUS", "BUTTONS", "TRIGGER", "STICKS", "SENSORS", "RUMBLE", "OUTPUT", "SETTINGS", "STATS"};
#define TITLE(i) (lang ? TITLES_EN[i] : TITLES_ID[i])
static uint8_t page = 0;
static uint8_t lang = 0;  // 0 = Indonesia, 1 = English
#define T(id, en) (lang ? (en) : (id))

// ---------- pilihan output: 10 warna default (disimpan) + 3 mode ----------
struct Preset { const char *name, *en; uint8_t r, g, b; };
static const Preset PRESETS[] = {
  {"Biru", "Blue", 0, 0, 255},       {"Merah", "Red", 255, 0, 0},      {"Hijau", "Green", 0, 255, 0},
  {"Ungu", "Purple", 140, 0, 255},   {"Kuning", "Yellow", 255, 190, 0}, {"Cyan", "Cyan", 0, 255, 255},
  {"Putih", "White", 255, 255, 255}, {"Oranye", "Orange", 255, 70, 0}, {"Pink", "Pink", 255, 0, 120},
  {"Mati", "Off", 0, 0, 0}};
#define PNAME(i) (lang ? PRESETS[i].en : PRESETS[i].name)
static const uint8_t NPRESET = sizeof(PRESETS) / sizeof(PRESETS[0]);
static const uint8_t NSEL = NPRESET + 3;
static const char *MODE_ID[3] = {"Stik", "Pelangi", "Default"};
static const char *MODE_EN[3] = {"Stick", "Rainbow", "Default"};
#define MODE_NAME(i) (lang ? MODE_EN[i] : MODE_ID[i])
static uint8_t sel = 0;

// ---------- statistik ----------
static uint32_t evCount = 0, lastEvMs = 0, maxGap = 0;
static uint32_t connCount = 0, discCount = 0;
static uint32_t evPerSec = 0, prevEv = 0, prevEvT = 0;
static uint32_t forgetMsgUntil = 0;

// ---------- kecerahan layar (PWM backlight) ----------
#define BL_CH 0
static uint8_t brightPct = 100;

static void backlightInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(TFT_BL, BL_CH);
#endif
}

static void setBacklight(uint8_t pct) {
  uint8_t duty = (uint8_t)(255L * pct / 100);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, duty);
#else
  ledcWrite(BL_CH, duty);
#endif
}

// ---------- tes getar ----------
static uint8_t vibeMode = 0;
static uint8_t curWeak = 0, curStrong = 0;
static const char *VIBE_ID[7] = {"Mati", "Besar", "Ringan", "Keduanya", "Ramp", "Pulsa", "Trigger"};
static const char *VIBE_EN[7] = {"Off", "Strong", "Light", "Both", "Ramp", "Pulse", "Trigger"};
#define VIBE_NAME(i) (lang ? VIBE_EN[i] : VIBE_ID[i])

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
  BP32.enableNewBluetoothConnections(true);  // tetap terima pairing baru
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

// Kontroler aktif = slot yang dipilih (selSlot); bila putus, pindah ke yang lain.
// Tidak memakai hasData(), dan ada masa tenggang 1,5 dtk agar status tidak berkedip.
static int selSlot = 0;

static ControllerPtr getCtl() {
  static ControllerPtr last = nullptr;
  static uint32_t lastSeen = 0;
  uint32_t now = millis();
  ControllerPtr c = myControllers[selSlot];
  if (c && c->isConnected()) {
    last = c;
    lastSeen = now;
    return c;
  }
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    c = myControllers[i];
    if (c && c->isConnected()) {
      selSlot = i;
      last = c;
      lastSeen = now;
      return c;
    }
  }
  if (last && now - lastSeen < 1500) return last;
  return nullptr;
}

static int connectedMask() {
  int m = 0;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++)
    if (myControllers[i] && myControllers[i]->isConnected()) m |= 1 << i;
  return m;
}

static void cycleController() {
  for (int k = 1; k <= BP32_MAX_GAMEPADS; k++) {
    int i = (selSlot + k) % BP32_MAX_GAMEPADS;
    if (myControllers[i] && myControllers[i]->isConnected()) { selSlot = i; return; }
  }
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

// warna default tiap slot kontroler: 1 = biru, 2 = merah, 3 = hijau, 4 = kuning
static const uint8_t SLOT_COL[4][3] = {{0, 0, 255}, {255, 0, 0}, {0, 255, 0}, {255, 190, 0}};

static void applyOutput(ControllerPtr c) {
  static uint8_t prevLed[BP32_MAX_GAMEPADS][3];
  static uint32_t lastLed[BP32_MAX_GAMEPADS], connectedAt[BP32_MAX_GAMEPADS], lastForce[BP32_MAX_GAMEPADS];
  static ControllerPtr lastC[BP32_MAX_GAMEPADS];
  static uint32_t lastRumble = 0;
  static bool wasRumbling = false;
  uint32_t now = millis();

  // LED: setiap kontroler punya warna default sesuai slotnya.
  // Kontroler terpilih bisa di-override sementara dari halaman OUTPUT.
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    ControllerPtr k = myControllers[i];
    if (!k || !k->isConnected()) { lastC[i] = nullptr; continue; }
    if (k != lastC[i]) {  // baru terhubung: kirim warna
      lastC[i] = k;
      connectedAt[i] = now;
      memset(prevLed[i], 0xFF, 3);
    }
    if (now - lastLed[i] < 50) continue;
    lastLed[i] = now;
    uint8_t r = SLOT_COL[i % 4][0], g = SLOT_COL[i % 4][1], b = SLOT_COL[i % 4][2];
    if (k == c && sel != NPRESET + 2) {
      if (sel < NPRESET) {
        r = PRESETS[sel].r; g = PRESETS[sel].g; b = PRESETS[sel].b;
      } else if (sel == NPRESET) {
        r = clamp8((k->axisX() + 512) / 4);
        g = clamp8((k->axisY() + 512) / 4);
        b = clamp8(k->throttle() / 4);
      } else {
        hue((now / 12) & 0xFF, r, g, b);
      }
    }
    uint8_t cur[3] = {r, g, b};
    // 2,5 detik pertama setelah konek: kirim ulang tiap 300 ms (DS4 kadang mengabaikan yang pertama)
    bool force = (now - connectedAt[i] < 2500) && (now - lastForce[i] >= 300);
    if (force || memcmp(cur, prevLed[i], 3) != 0) {
      k->setColorLED(r, g, b);
      memcpy(prevLed[i], cur, 3);
      if (force) lastForce[i] = now;
    }
  }

  // getar: hanya kontroler terpilih
  if (!c) { curWeak = curStrong = 0; return; }
  uint8_t weak = 0, strong = 0;
  if (page == P_VIBRATE) {
    uint32_t t = now % 2000;
    switch (vibeMode) {
      case 1: strong = 255; break;
      case 2: weak = 255; break;
      case 3: weak = strong = 255; break;
      case 4: weak = strong = (uint8_t)(t < 1000 ? t * 255 / 1000 : (2000 - t) * 255 / 1000); break;
      case 5: weak = strong = (now % 600 < 300) ? 255 : 0; break;
      case 6: strong = clamp8(c->brake() / 4); weak = clamp8(c->throttle() / 4); break;
    }
  } else if (sel == NPRESET) {
    weak = clamp8(c->throttle() / 4);
    strong = clamp8(c->brake() / 4);
  }
  curWeak = weak;
  curStrong = strong;
  if (weak || strong) {
    if (now - lastRumble >= 100) {
      c->playDualRumble(0, 150, weak, strong);
      lastRumble = now;
    }
    wasRumbling = true;
  } else if (wasRumbling) {
    c->playDualRumble(0, 0, 0, 0);  // hentikan getar
    wasRumbling = false;
  }
}

// ---------- tombol 17 buah ----------
enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_CROSS, B_CIRCLE, B_SQUARE, B_TRI, B_L1, B_R1, B_L2, B_R2,
       B_L3, B_R3, B_SHARE, B_OPT, B_PS, NB };
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

  put(1, 6, 106, 1, TL_DATUM, C_SUB, C_BG, 0, T("TERUJI", "TESTED"));
  if (c) put(0, 6, 116, 2, TL_DATUM, cnt == NB ? C_OK : C_TXT, C_BG, 64, "%d/%d", cnt, NB);
  else put(0, 6, 116, 2, TL_DATUM, C_WARN, C_BG, 64, T("Tunggu..", "Wait.."));
  put(2, 172, 120, 1, TL_DATUM, C_SUB, C_BG, 0, T("Atas: reset", "Top: reset"));
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
  put(4, 120, 72, 1, TC_DATUM, C_SUB, C_BG, 0, T("PUNCAK", "PEAK"));
  put(5, 120, 86, 2, TC_DATUM, C_WARN, C_BG, 76, "%d%% %d%%", (int)(peakL * 100L / 1023), (int)(peakR * 100L / 1023));
  put(6, 120, 108, 1, TC_DATUM, C_SUB, C_BG, 76, "%d | %d", l, r);
  put(7, 120, 122, 1, TC_DATUM, C_SUB, C_BG, 0, T("Atas: reset", "Top: reset"));
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
  put(2, 62, 24, 1, TC_DATUM, C_SUB, C_BG, 0, T("STIK KIRI", "LEFT STICK"));
  put(3, 178, 24, 1, TC_DATUM, C_SUB, C_BG, 0, T("STIK KANAN", "RIGHT STICK"));
  put(4, 120, 56, 1, TC_DATUM, C_SUB, C_BG, 0, T("Atas", "Top"));
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
  }
  for (int k = 0; k < 3; k++) {
    int x = 8 + k * 76;
    bool on = (sel == NPRESET + k);
    tft.fillRoundRect(x, 104, 70, 20, 5, on ? C_ACC : C_BAR);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(on ? C_BG : C_SUB, on ? C_ACC : C_BAR);
    tft.setTextPadding(0);
    tft.drawString(MODE_NAME(k), x + 35, 114, 2);
  }
}

static void pageOutput(ControllerPtr c) {
  if (drawnSel != sel) {
    drawnSel = sel;
    drawOutputUI();
  }
  if (sel < NPRESET) put(0, 8, 84, 2, TL_DATUM, C_OK, C_BG, 224, T("%s (sementara)", "%s (temporary)"), PNAME(sel));
  else put(0, 8, 84, 2, TL_DATUM, C_ACC, C_BG, 224, "Mode %s", MODE_NAME(sel - NPRESET));
  if (sel == NPRESET) put(1, 6, 127, 1, TL_DATUM, C_SUB, C_BG, 230, T("RGB: stik kiri+R2 | rumble: R2/L2", "RGB: left stick+R2 | rumble: R2/L2"));
  else if (sel == NPRESET + 2) put(1, 6, 127, 1, TL_DATUM, C_SUB, C_BG, 230, T("Default: kontroler 1 biru, 2 merah", "Default: ctrl 1 blue, 2 red"));
  else put(1, 6, 127, 1, TL_DATUM, C_SUB, C_BG, 230, T("Atas: ganti | tidak disimpan", "Top: change | not saved"));
}

// ---------- halaman STATS ----------
static void pageStats(ControllerPtr c) {
  static const char *LI[6] = {"Update/detik", "Jeda update maks", "Total update", "Heap bebas", "Heap minimum", "Uptime"};
  static const char *LE[6] = {"Updates/sec", "Max update gap", "Total updates", "Free heap", "Min heap", "Uptime"};
  const char **L = lang ? LE : LI;
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
static int st_shown = -1, pv_bat = -1, dotsKey = -1;
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
  static uint32_t lastAnim = 0;
  int state = c ? 1 : 0;

  if (state != st_shown) {
    st_shown = state;
    tft.fillRoundRect(6, 24, 228, 52, 6, C_PANEL);
    for (int i = 0; i < 4; i++) tc_s[i][0] = 0;
    pv_bat = -1;
    dotsKey = -1;
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
    put(1, 50, 56, 2, TL_DATUM, C_OK, C_PANEL, 76, T("Terhubung", "Connected"));
    int mask = connectedMask();
    int total = __builtin_popcount(mask);
    int key = total > 1 ? (mask << 4 | selSlot) : 0;
    if (key != dotsKey) {
      dotsKey = key;
      tft.fillRect(208, 26, 26, 14, C_PANEL);
      if (total > 1) {
        for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
          if (!(mask & (1 << i))) continue;
          if (i == selSlot) tft.fillCircle(214 + i * 7, 32, 3, C_ACC);
          else tft.fillCircle(214 + i * 7, 32, 2, C_DIM);
        }
      }
    }
    int pct = c->battery() * 100 / 255;
    hbar(133, 59, 54, 8, pv_bat, pct, 100, pct > 50 ? C_OK : (pct > 20 ? C_WARN : C_BAD), C_BAR);
    put(2, 196, 56, 2, TL_DATUM, C_TXT, C_PANEL, 36, "%d%%", pct);
  } else {
    put(0, 50, 28, 4, TL_DATUM, C_TXT, C_PANEL, 176, T("Mencari...", "Searching..."));
    put(1, 50, 56, 2, TL_DATUM, C_WARN, C_PANEL, 176, T("Tahan PS+Share di DS4", "Hold PS+Share on DS4"));
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

  put(4, 14, 84, 2, TL_DATUM, C_SUB, C_PANEL, 120, T("Tegangan DS4", "DS4 voltage"));
  if (c) {
    int dsMv = 3300 + (c->battery() * 100 / 255) * 9;  // perkiraan dari level baterai (DS4 tidak melaporkan voltase)
    put(5, 226, 84, 2, TR_DATUM, C_TXT, C_PANEL, 90, "~%d.%02d V", dsMv / 1000, (dsMv % 1000) / 10);
  } else {
    put(5, 226, 84, 2, TR_DATUM, C_SUB, C_PANEL, 90, "--");
  }
  put(6, 14, 100, 2, TL_DATUM, C_SUB, C_PANEL, 120, T("Konek / Putus", "Conn / Disc"));
  put(7, 226, 100, 2, TR_DATUM, C_TXT, C_PANEL, 90, "%lu / %lu", (unsigned long)connCount, (unsigned long)discCount);
  int nCtl = connectedCount();
  if (millis() < forgetMsgUntil) put(8, 14, 118, 1, TL_DATUM, C_WARN, C_PANEL, 210, T("Pairing dihapus!", "Pairing cleared!"));
  else if (nCtl > 1 && (millis() / 2500) % 2 == 0) put(8, 14, 118, 1, TL_DATUM, C_ACC, C_PANEL, 210, T("Kontroler %d/%d | Atas klik: ganti", "Ctrl %d/%d | Top click: switch"), __builtin_popcount(connectedMask() & ((2 << selSlot) - 1)), nCtl);
  else put(8, 14, 118, 1, TL_DATUM, C_SUB, C_PANEL, 210, T("Atas tahan 2 dtk: lupakan pairing", "Top hold 2s: forget pairing"));
  put(9, 14, 125, 1, TL_DATUM, C_SUB, C_PANEL, 210, T("Bawah: klik maju, tahan mundur", "Bottom: click next, hold back"));
}

// ---------- halaman GETAR ----------
static int pv_vw = -1, pv_vs = -1;

static void pageVibrate(ControllerPtr c) {
  put(0, 8, 26, 2, TL_DATUM, C_SUB, C_BG, 0, T("Pola", "Mode"));
  put(1, 232, 26, 2, TR_DATUM, vibeMode ? C_OK : C_DIM, C_BG, 150, "%s", VIBE_NAME(vibeMode));
  put(2, 8, 52, 2, TL_DATUM, C_SUB, C_BG, 0, T("Besar", "Strong"));
  put(3, 8, 74, 2, TL_DATUM, C_SUB, C_BG, 0, T("Ringan", "Light"));
  hbar(66, 55, 118, 12, pv_vs, curStrong, 255, C_ACC, C_BAR);
  hbar(66, 77, 118, 12, pv_vw, curWeak, 255, C_PINK, C_BAR);
  put(4, 232, 52, 2, TR_DATUM, C_TXT, C_BG, 44, "%d%%", curStrong * 100 / 255);
  put(5, 232, 74, 2, TR_DATUM, C_TXT, C_BG, 44, "%d%%", curWeak * 100 / 255);
  if (c) put(6, 8, 96, 1, TL_DATUM, C_OK, C_BG, 226, T("Kontroler terhubung", "Controller connected"));
  else put(6, 8, 96, 1, TL_DATUM, C_WARN, C_BG, 226, T("Belum ada kontroler", "No controller"));
  put(7, 8, 108, 1, TL_DATUM, C_SUB, C_BG, 226, T("Atas klik: ganti pola", "Top click: change pattern"));
  put(8, 8, 118, 1, TL_DATUM, C_SUB, C_BG, 226, T("Trigger: L2=besar, R2=ringan", "Trigger: L2=strong, R2=light"));
  put(9, 8, 127, 1, TL_DATUM, C_SUB, C_BG, 226, T("Berhenti otomatis saat pindah halaman", "Stops automatically on page change"));
}

// ---------- halaman SETELAN ----------
static int pv_br = -1;
static uint8_t setRow = 0, drawnLang = 255;

static void drawLangChips() {
  const char *n[2] = {"Indonesia", "English"};
  for (int k = 0; k < 2; k++) {
    int x = 12 + k * 116;
    bool on = (lang == k);
    tft.fillRoundRect(x, 82, 104, 18, 5, on ? C_ACC : C_BAR);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(on ? C_BG : C_SUB, on ? C_ACC : C_BAR);
    tft.setTextPadding(0);
    tft.drawString(n[k], x + 52, 91, 2);
  }
}

static void pageSettings(ControllerPtr c) {
  put(0, 8, 24, 2, TL_DATUM, setRow == 0 ? C_ACC : C_SUB, C_BG, 170, "%s%s", setRow == 0 ? "> " : "  ", T("Kecerahan layar", "Screen brightness"));
  put(1, 232, 24, 2, TR_DATUM, C_TXT, C_BG, 56, "%d%%", brightPct);
  hbar(13, 45, 214, 12, pv_br, brightPct, 100, C_ACC, C_BAR);
  put(2, 8, 64, 2, TL_DATUM, setRow == 1 ? C_ACC : C_SUB, C_BG, 170, "%s%s", setRow == 1 ? "> " : "  ", T("Bahasa", "Language"));
  if (drawnLang != lang) {
    drawnLang = lang;
    drawLangChips();
  }
  put(3, 8, 106, 1, TL_DATUM, C_SUB, C_BG, 226, "%s", T("Atas klik: ubah | tahan: pindah baris", "Top click: change | hold: switch row"));
  put(4, 8, 116, 1, TL_DATUM, C_SUB, C_BG, 226, "%s", T("Tersimpan otomatis", "Saved automatically"));
  put(5, 8, 126, 1, TL_DATUM, C_SUB, C_BG, 226, "%s", T("Atas dobel klik: power off", "Top double click: power off"));
}

// ---------- top bar & header ----------
static int ttgoMv = -1;

static int ttgoPct() {
  if (ttgoMv <= 0) return -1;
  int p = (ttgoMv - 3300) * 100 / 850;
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

static uint16_t battColor(int pct) { return pct > 50 ? C_OK : (pct > 20 ? C_WARN : C_BAD); }

static void drawTopRight(ControllerPtr c) {
  tft.fillRect(174, 1, 66, 18, C_BAR);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  // TTGO: "T" + ikon baterai (atau "USB" bila baterai tidak terbaca)
  tft.setTextColor(C_SUB, C_BAR);
  tft.drawString("T", 176, 6, 1);
  tft.drawRect(183, 6, 19, 10, C_SUB);
  tft.fillRect(202, 9, 2, 4, C_SUB);
  int tp = ttgoPct();
  if (tp >= 0) {
    tft.fillRect(184, 7, tp * 17 / 100, 8, battColor(tp));
  } else {
    tft.setTextColor(C_DIM, C_BAR);
    tft.drawString("USB", 184, 7, 1);
  }
  // DS4: "D" + ikon baterai
  tft.setTextColor(c ? C_OK : C_DIM, C_BAR);
  tft.drawString("D", 209, 6, 1);
  tft.drawRect(218, 6, 19, 10, c ? C_SUB : C_DIM);
  tft.fillRect(237, 9, 2, 4, c ? C_SUB : C_DIM);
  if (c) {
    int pct = c->battery() * 100 / 255;
    tft.fillRect(219, 7, pct * 17 / 100, 8, battColor(pct));
  } else {
    tft.setTextColor(C_DIM, C_BAR);
    tft.drawString("--", 223, 7, 1);
  }
}

static void refreshTopBar(ControllerPtr c) {
  static int lastKey = -2;
  static uint32_t lastBat = 0;
  uint32_t now = millis();
  if (ttgoMv < 0 || now - lastBat > 5000) { ttgoMv = readBatteryMv(); lastBat = now; }
  int tp = ttgoPct();
  int key = ((c ? (c->battery() * 100 / 255) / 5 : -1) + 1) * 1000 + (tp < 0 ? 0 : tp / 5 + 1);
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
  tft.drawString(TITLE(page), 6, 2, 2);
  for (int i = 0; i < NPAGES; i++) {
    int x = 112 + i * 7;
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
    case P_VIBRATE:
      pv_vw = pv_vs = -1;
      break;
    case P_OUTPUT:
      drawnSel = 255;
      break;
    case P_SETTINGS:
      pv_br = -1;
      drawnLang = 255;
      tft.drawRect(12, 44, 216, 14, C_DIM);
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
    case P_VIBRATE: pageVibrate(c); break;
    case P_OUTPUT: pageOutput(c); break;
    case P_SETTINGS: pageSettings(c); break;
    case P_STATS: pageStats(c); break;
  }
}

// ---------- tombol fisik & power ----------
void powerOff() {
  while (!digitalRead(BTN_TOP)) delay(10);  // tunggu tombol dilepas
  delay(80);
  setBacklight(0);
  tft.writecommand(TFT_DISPOFF);
  tft.writecommand(TFT_SLPIN);
  delay(120);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 0);
  esp_deep_sleep_start();
}

static void goPage(int p) {
  page = p;
  vibeMode = 0;  // getar berhenti saat pindah halaman
  header();
}

// aksi tombol atas (pilih) sesuai halaman
static void topAction() {
  switch (page) {
    case P_BUTTONS: memset(btested, 0, sizeof btested); header(); break;
    case P_TRIGGER: peakL = peakR = 0; header(); break;
    case P_STICK: memset(trace_, 0, sizeof trace_); header(); break;
    case P_STATS: maxGap = 0; break;
    case P_STATUS: cycleController(); break;
    case P_VIBRATE: vibeMode = (vibeMode + 1) % 7; break;
    case P_SETTINGS:
      if (setRow == 0) {
        brightPct = brightPct >= 100 ? 10 : brightPct + 10;
        setBacklight(brightPct);
        prefs.putUChar("bri", brightPct);
      } else {
        lang = lang ? 0 : 1;
        prefs.putUChar("lang", lang);
        header();  // gambar ulang dengan bahasa baru
      }
      break;
    case P_OUTPUT:
      sel = (sel + 1) % NSEL;  // override sementara untuk kontroler terpilih
      break;
  }
}

void handleButtons() {
  // status awal dibaca ulang saat boot, jadi klik pembangun tidak terhitung
  static bool lastT = digitalRead(BTN_TOP), lastB = digitalRead(BTN_BOTTOM);
  static uint32_t edgeT = 0, edgeB = 0, firstClick = 0, pressT = 0, pressB = 0;
  static bool pressedT = false, longDoneT = false, pressedB = false, holdDoneB = false;
  uint32_t now = millis();
  bool t = digitalRead(BTN_TOP), b = digitalRead(BTN_BOTTOM);

  // ----- tombol atas (GPIO35): pilih/aksi halaman | tahan 2 dtk di STATUS: lupakan pairing | dobel klik: power off
  if (!t && lastT && now - edgeT > 30) {
    edgeT = now;
    if (firstClick && now - firstClick < 450) {
      powerOff();  // dobel klik, dideteksi saat penekanan kedua
    } else {
      firstClick = now;
      pressedT = true;
      longDoneT = false;
      pressT = now;
    }
  }
  if (pressedT && !t && !longDoneT && page == P_STATUS && now - pressT >= 2000) {
    longDoneT = true;
    firstClick = 0;
    BP32.forgetBluetoothKeys();
    forgetMsgUntil = now + 3000;
  }
  // di SETELAN: tahan 0,7 dtk pindah baris (kecerahan / bahasa)
  if (pressedT && !t && !longDoneT && page == P_SETTINGS && now - pressT >= 700) {
    longDoneT = true;
    firstClick = 0;
    setRow ^= 1;
  }
  if (t && !lastT) {  // dilepas: klik pendek = aksi pilih
    if (pressedT && !longDoneT && now - pressT < 1000) topAction();
    pressedT = false;
  }

  // ----- tombol bawah (GPIO0): klik = halaman berikutnya | tahan 0,6 dtk = halaman sebelumnya
  if (!b && lastB && now - edgeB > 30) {
    edgeB = now;
    pressedB = true;
    holdDoneB = false;
    pressB = now;
  }
  if (pressedB && !b && !holdDoneB && now - pressB >= 600) {
    holdDoneB = true;
    goPage((page + NPAGES - 1) % NPAGES);
  }
  if (b && !lastB) {
    if (pressedB && !holdDoneB) goPage((page + 1) % NPAGES);
    pressedB = false;
  }

  lastT = t;
  lastB = b;
}

// ---------- splash screen ----------
static void splashPad(int cx, int cy) {
  uint16_t body = RGB(44, 50, 80);
  tft.fillCircle(cx - 36, cy + 12, 14, body);
  tft.fillCircle(cx + 36, cy + 12, 14, body);
  tft.fillRoundRect(cx - 44, cy - 16, 88, 36, 14, body);
  tft.fillRoundRect(cx - 14, cy - 12, 28, 13, 3, RGB(70, 78, 112));  // touchpad
  tft.fillRect(cx - 34, cy, 12, 4, C_SUB);                           // d-pad
  tft.fillRect(cx - 30, cy - 4, 4, 12, C_SUB);
  tft.fillCircle(cx - 12, cy + 12, 7, RGB(24, 28, 46));              // stik
  tft.fillCircle(cx + 12, cy + 12, 7, RGB(24, 28, 46));
  tft.drawCircle(cx - 12, cy + 12, 7, C_DIM);
  tft.drawCircle(cx + 12, cy + 12, 7, C_DIM);
}

static void showSplash() {
  tft.fillScreen(C_BG);
  const int cx = 120, cy = 40;
  splashPad(cx, cy);
  const char *title = "DUALSHOCK";
  const int total = 2600;
  int w1 = tft.textWidth(title, 4);
  int x1 = (240 - w1) / 2;
  tft.fillRect(60, 124, 120, 4, C_BAR);
  const int bx[4] = {cx + 28, cx + 34, cx + 28, cx + 22};
  const int by[4] = {cy - 4, cy + 2, cy + 8, cy + 2};
  const uint16_t bc[4] = {RGB(70, 225, 150), RGB(255, 90, 90), RGB(90, 150, 255), C_PINK};
  int shown = 0, sub = -1, btn = -1;
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)total) {
    uint32_t e = millis() - t0;
    BP32.update();

    uint8_t r, g, b;
    hue((e / 4) & 0xFF, r, g, b);  // lightbar berganti warna
    tft.fillRect(cx - 16, cy - 20, 32, 3, tft.color565(r, g, b));

    int k = (e / 160) % 4;  // tombol bergantian menyala
    if (k != btn) {
      btn = k;
      for (int i = 0; i < 4; i++) tft.fillCircle(bx[i], by[i], 3, i == k ? bc[i] : C_DIM);
    }

    int n = e < 300 ? 0 : (int)((e - 300) / 70);  // judul muncul huruf demi huruf
    if (n > 9) n = 9;
    if (n != shown) {
      shown = n;
      char buf[12];
      memcpy(buf, title, n);
      buf[n] = 0;
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(C_TXT, C_BG);
      tft.setTextPadding(0);
      tft.drawString(buf, x1, 72, 4);
    }

    int f = e < 1100 ? -1 : (int)((e - 1100) / 60);  // subjudul memudar masuk
    if (f > 7) f = 7;
    if (f >= 0 && f != sub) {
      sub = f;
      uint16_t col = tft.color565(8 * (7 - f) / 7, 10 + (205 - 10) * f / 7, 18 + (255 - 18) * f / 7);
      tft.setTextDatum(TC_DATUM);
      tft.setTextColor(col, C_BG);
      tft.setTextPadding(0);
      tft.drawString("POCKET TESTER", 120, 102, 2);
    }

    tft.fillRect(60, 124, (int)(120L * e / total), 4, C_ACC);
    delay(16);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_TOP, INPUT);
  pinMode(BTN_BOTTOM, INPUT_PULLUP);
  pinMode(BAT_EN, OUTPUT);
  digitalWrite(BAT_EN, LOW);

  for (int b = 0; b < 48; b++) {
    float a = (b + 0.5f) * 2.0f * PI / 48.0f - PI;
    cosT[b] = cosf(a);
    sinT[b] = sinf(a);
  }

  prefs.begin("ds4t", false);
  sel = NPRESET + 2;  // default: warna per kontroler (1 biru, 2 merah)

  lang = prefs.getUChar("lang", 0);
  if (lang > 1) lang = 0;
  brightPct = prefs.getUChar("bri", 100);
  if (brightPct < 10 || brightPct > 100) brightPct = 100;

  tft.init();
  backlightInit();
  setBacklight(brightPct);
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  Serial.printf("Bluepad32 firmware: %s\n", BP32.firmwareVersion());
  BP32.setup(&onConnectedController, &onDisconnectedController);
  BP32.enableVirtualDevice(false);
  BP32.enableNewBluetoothConnections(true);

  ttgoMv = readBatteryMv();
  showSplash();
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

  static uint32_t lastScan = 0;
  if (now - lastScan > 2000) {  // pastikan pairing baru tetap terbuka walau sudah ada yang terhubung
    lastScan = now;
    if (connectedCount() < BP32_MAX_GAMEPADS) BP32.enableNewBluetoothConnections(true);
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
