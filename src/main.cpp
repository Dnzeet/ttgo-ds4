// Tes kontroler Bluetooth (DualShock 4 dll.) untuk LilyGo T-Display (TTGO, ESP32 biasa)
// Memakai Bluepad32: ESP32 scan sendiri, DS4 dipairing dengan PS+Share.
//
// GPIO35 = klik: halaman berikutnya | dobel klik (< 450 ms): power off | nyala: tekan sekali
// GPIO0  = klik pendek: halaman OUTPUT ganti warna/mode, halaman STATS reset jeda maks
//          tahan 2 detik di halaman STATUS: lupakan semua pairing

#include <Arduino.h>
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

static const uint8_t NPAGES = 5;
static const char *TITLES[NPAGES] = {"STATUS", "INPUT", "SENSOR", "OUTPUT", "STATS"};
static uint8_t page = 0;

// Daftar pilihan output: 10 warna default (disimpan) + 3 mode
struct Preset { const char *name; uint8_t r, g, b; };
static const Preset PRESETS[] = {
  {"Biru", 0, 0, 255},     {"Merah", 255, 0, 0},     {"Hijau", 0, 255, 0},
  {"Ungu", 140, 0, 255},   {"Kuning", 255, 190, 0},  {"Cyan", 0, 255, 255},
  {"Putih", 255, 255, 255}, {"Oranye", 255, 70, 0},  {"Pink", 255, 0, 120},
  {"Mati", 0, 0, 0}};
static const uint8_t NPRESET = sizeof(PRESETS) / sizeof(PRESETS[0]);
static const uint8_t NSEL = NPRESET + 3;
static const char *MODE_NAMES[3] = {"LED=stik, rumble=trigger", "LED pelangi", "Semua mati"};
static uint8_t sel = 0;         // pilihan aktif
static uint8_t savedColor = 0;  // preset default yang tersimpan di flash

static uint32_t evCount = 0, lastEvMs = 0, maxGap = 0;
static uint32_t connCount = 0, discCount = 0;
static uint32_t evPerSec = 0, prevEv = 0, prevEvT = 0;
static uint32_t forgetMsgUntil = 0;

// ---------- cache gambar: hanya gambar ulang bila berubah ----------
struct RowCache { char s[64]; uint16_t fg; };
static RowCache rc[8];
static int bc[8];
static uint8_t lastSwatchSel = 255;

static void resetCache() {
  memset(rc, 0, sizeof rc);
  for (int i = 0; i < 8; i++) bc[i] = -1;
  lastSwatchSel = 255;
}

void header() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 240, 16, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextPadding(0);
  char b[40];
  snprintf(b, sizeof b, "BP32 Test  %u/%u  %s", page + 1, NPAGES, TITLES[page]);
  tft.drawString(b, 2, 0, 2);
  resetCache();
}

void line(uint8_t row, uint16_t fg, const char *fmt, ...) {
  char b[64];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  if (rc[row].fg == fg && strcmp(rc[row].s, b) == 0) return;
  strcpy(rc[row].s, b);
  rc[row].fg = fg;
  tft.setTextColor(fg, TFT_BLACK);
  tft.setTextPadding(240);
  tft.drawString(b, 2, 18 + row * 16, 2);
}

void bar(uint8_t row, const char *lbl, int v, int vmax, uint16_t col) {
  int w = (int)(190L * v / vmax);
  if (bc[row] == w) return;
  bc[row] = w;
  int y = 18 + row * 16;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(30);
  tft.drawString(lbl, 2, y, 2);
  tft.fillRect(40, y + 3, 190, 10, TFT_DARKGREY);
  if (w > 0) tft.fillRect(40, y + 3, w, 10, col);
}

// ---------- controller ----------
void onConnectedController(ControllerPtr ctl) {
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

static ControllerPtr activeCtl() {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    ControllerPtr c = myControllers[i];
    if (c && c->isConnected() && c->hasData()) return c;
  }
  return nullptr;
}

static int connectedCount() {
  int n = 0;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++)
    if (myControllers[i] && myControllers[i]->isConnected()) n++;
  return n;
}

static void hue(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (h < 85) { r = 255 - h * 3; g = h * 3; b = 0; }
  else if (h < 170) { h -= 85; r = 0; g = 255 - h * 3; b = h * 3; }
  else { h -= 170; r = h * 3; g = 0; b = 255 - h * 3; }
}

static uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void applyOutput() {
  static uint8_t prevLed[3] = {1, 1, 1};
  static uint32_t lastLed = 0, lastRumble = 0, connectedAt = 0, lastForce = 0;
  static ControllerPtr lastC = nullptr;

  ControllerPtr c = activeCtl();
  uint32_t now = millis();
  if (!c) { lastC = nullptr; return; }
  if (c != lastC) {  // kontroler baru terhubung: kirim warna default
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
    // 2,5 detik pertama setelah konek: kirim ulang tiap 300 ms (DS4 kadang mengabaikan yang pertama)
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

// ---------- tombol & power ----------
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
  if (!m && !longDone && page == 0 && now - pressM >= 2000) {
    longDone = true;
    BP32.forgetBluetoothKeys();
    forgetMsgUntil = now + 3000;
  }
  if (m && !lastM && !longDone && now - pressM > 30 && now - pressM < 1000) {
    if (page == 4) maxGap = 0;
    else if (page == 3) {
      sel = (sel + 1) % NSEL;
      if (sel < NPRESET && sel != savedColor) {  // simpan warna default ke flash
        savedColor = sel;
        prefs.putUChar("col", savedColor);
      }
    }
  }
  lastN = n;
  lastM = m;
}

static int readBatteryMv() {
  digitalWrite(BAT_EN, HIGH);
  delay(5);
  int mv = analogReadMilliVolts(BAT_ADC) * 2;
  digitalWrite(BAT_EN, LOW);
  return mv > 2800 ? mv : 0;
}

// ---------- halaman ----------
static void noCtl() {
  line(0, TFT_YELLOW, "Belum ada kontroler");
  for (int i = 1; i < 7; i++) line(i, TFT_BLACK, "");
}

void drawPage() {
  ControllerPtr c = activeCtl();
  switch (page) {
    case 0: {
      static uint32_t lastBat = 0;
      static int batMv = -1;
      if (batMv < 0 || millis() - lastBat > 5000) { batMv = readBatteryMv(); lastBat = millis(); }
      int n = connectedCount();
      line(0, TFT_CYAN, "Scan: aktif  Terhubung: %d", n);
      int row = 1;
      for (int i = 0; i < BP32_MAX_GAMEPADS && row < 3; i++) {
        ControllerPtr k = myControllers[i];
        if (k && k->isConnected()) {
          line(row++, TFT_GREEN, "%d: %s %d%%", i + 1, k->getModelName().c_str(), k->battery() * 100 / 255);
        }
      }
      while (row < 3) line(row++, TFT_BLACK, "");
      line(3, TFT_WHITE, "Connect: %lu  Putus: %lu", (unsigned long)connCount, (unsigned long)discCount);
      if (batMv) line(4, TFT_WHITE, "Baterai TTGO: %d.%02d V", batMv / 1000, (batMv % 1000) / 10);
      else line(4, TFT_DARKGREY, "Baterai TTGO: tidak terbaca");
      if (millis() < forgetMsgUntil) line(5, TFT_ORANGE, "Pairing dihapus!");
      else line(5, TFT_DARKGREY, "DS4: tahan PS+Share utk pairing");
      line(6, TFT_DARKGREY, "GPIO0 tahan: lupakan | GPIO35 x2: off");
      break;
    }
    case 1: {
      if (!c) { noCtl(); break; }
      String a, s;
      uint8_t d = c->dpad();
      if (d & 0x01) a += "Up "; if (d & 0x02) a += "Dn "; if (d & 0x08) a += "Lf "; if (d & 0x04) a += "Rt ";
      if (c->a()) a += "X "; if (c->b()) a += "O "; if (c->x()) a += "Sq "; if (c->y()) a += "Tr ";
      if (c->l1()) s += "L1 "; if (c->r1()) s += "R1 "; if (c->l2()) s += "L2 "; if (c->r2()) s += "R2 ";
      if (c->thumbL()) s += "L3 "; if (c->thumbR()) s += "R3 ";
      line(0, TFT_GREEN, "%s", a.c_str());
      line(1, TFT_GREEN, "%s misc:0x%02X", s.c_str(), c->miscButtons());
      line(2, TFT_WHITE, "LS: %5ld,%5ld", (long)c->axisX(), (long)c->axisY());
      line(3, TFT_WHITE, "RS: %5ld,%5ld", (long)c->axisRX(), (long)c->axisRY());
      bar(4, "L2", c->brake(), 1023, TFT_ORANGE);
      bar(5, "R2", c->throttle(), 1023, TFT_ORANGE);
      break;
    }
    case 2:
      if (!c) { noCtl(); break; }
      line(0, TFT_CYAN, "Gyro  X:%7ld", (long)c->gyroX());
      line(1, TFT_CYAN, "Gyro  Y:%7ld", (long)c->gyroY());
      line(2, TFT_CYAN, "Gyro  Z:%7ld", (long)c->gyroZ());
      line(3, TFT_MAGENTA, "Accel X:%7ld", (long)c->accelX());
      line(4, TFT_MAGENTA, "Accel Y:%7ld", (long)c->accelY());
      line(5, TFT_MAGENTA, "Accel Z:%7ld", (long)c->accelZ());
      line(6, TFT_WHITE, "Baterai: %d%%", c->battery() * 100 / 255);
      break;
    case 3: {
      line(0, TFT_WHITE, "Pilih (GPIO0 klik):");
      if (sel < NPRESET) {
        line(1, TFT_GREEN, "Warna default: %s", PRESETS[sel].name);
        line(2, TFT_DARKGREY, "Tersimpan, dikirim tiap konek");
      } else {
        line(1, TFT_GREEN, "Mode: %s", MODE_NAMES[sel - NPRESET]);
        line(2, TFT_DARKGREY, "Default tersimpan: %s", PRESETS[savedColor].name);
      }
      if (lastSwatchSel != sel) {
        lastSwatchSel = sel;
        uint16_t col = sel < NPRESET ? tft.color565(PRESETS[sel].r, PRESETS[sel].g, PRESETS[sel].b) : TFT_BLACK;
        tft.fillRect(190, 34, 44, 14, col);
      }
      line(3, c ? TFT_WHITE : TFT_YELLOW, c ? "Kontroler terhubung" : "Belum ada kontroler");
      line(4, TFT_DARKGREY, "Mode stik: R/G=stik kiri, B=R2,");
      line(5, TFT_DARKGREY, "rumble kecil=R2, besar=L2");
      break;
    }
    case 4: {
      uint32_t up = millis() / 1000;
      line(0, TFT_WHITE, "Update/detik: %lu", (unsigned long)evPerSec);
      line(1, TFT_WHITE, "Jeda update maks: %lu ms", (unsigned long)maxGap);
      line(2, TFT_WHITE, "Total update: %lu", (unsigned long)evCount);
      line(3, TFT_WHITE, "Heap: %u (min %u)", ESP.getFreeHeap(), ESP.getMinFreeHeap());
      line(4, TFT_WHITE, "Uptime: %lum %02lus", (unsigned long)(up / 60), (unsigned long)(up % 60));
      line(5, TFT_DARKGREY, "GPIO0 klik: reset jeda maks");
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_NEXT, INPUT);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BAT_EN, OUTPUT);
  digitalWrite(BAT_EN, LOW);

  prefs.begin("ds4t", false);
  savedColor = prefs.getUChar("col", 0);
  if (savedColor >= NPRESET) savedColor = 0;
  sel = savedColor;

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  Serial.printf("Bluepad32 firmware: %s\n", BP32.firmwareVersion());
  BP32.setup(&onConnectedController, &onDisconnectedController);
  BP32.enableVirtualDevice(false);

  header();
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();

  if (BP32.update()) {
    if (lastEvMs && now - lastEvMs > maxGap && connectedCount()) maxGap = now - lastEvMs;
    lastEvMs = now;
    evCount++;
  }

  handleButtons();
  applyOutput();

  if (now - prevEvT >= 1000) {
    evPerSec = (evCount - prevEv) * 1000UL / (now - prevEvT);
    prevEv = evCount;
    prevEvT = now;
  }
  if (now - lastDraw >= 50) {
    lastDraw = now;
    drawPage();
  }
  vTaskDelay(1);
}
