// Tes kontroler Bluetooth (DualShock 4 dll.) untuk LilyGo T-Display (TTGO, ESP32 biasa)
// Memakai Bluepad32: ESP32 scan sendiri, DS4 dipairing dengan PS+Share.
//
// GPIO35 = halaman berikutnya
// GPIO0  = klik pendek: ganti mode output (di halaman STATS: reset jeda maks)
//          tahan 2 detik di halaman STATUS: lupakan semua pairing

#include <Arduino.h>
#include <stdarg.h>
#include <TFT_eSPI.h>
#include <Bluepad32.h>

#define BTN_NEXT 35
#define BTN_MODE 0

TFT_eSPI tft;
ControllerPtr myControllers[BP32_MAX_GAMEPADS];

static const uint8_t NPAGES = 5;
static const char *TITLES[NPAGES] = {"STATUS", "INPUT", "SENSOR", "OUTPUT", "STATS"};
static uint8_t page = 0;
static uint8_t outMode = 0;  // 0 = LED ikut stik + rumble ikut trigger, 1 = LED pelangi, 2 = mati
static const char *MODES[3] = {"LED=stik, rumble=trigger", "LED pelangi", "Semua mati"};

static uint32_t evCount = 0, lastEvMs = 0, maxGap = 0;
static uint32_t connCount = 0, discCount = 0;
static uint32_t evPerSec = 0, prevEv = 0, prevEvT = 0;
static uint32_t forgetMsgUntil = 0;

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

void header() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 240, 16, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextPadding(0);
  char b[40];
  snprintf(b, sizeof b, "BP32 Test  %u/%u  %s", page + 1, NPAGES, TITLES[page]);
  tft.drawString(b, 2, 0, 2);
}

void line(uint8_t row, uint16_t fg, const char *fmt, ...) {
  char b[64];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  tft.setTextColor(fg, TFT_BLACK);
  tft.setTextPadding(240);
  tft.drawString(b, 2, 18 + row * 16, 2);
}

void bar(uint8_t row, const char *lbl, int v, int vmax, uint16_t col) {
  int y = 18 + row * 16;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(30);
  tft.drawString(lbl, 2, y, 2);
  int w = (int)(190L * v / vmax);
  tft.fillRect(40, y + 3, 190, 10, TFT_DARKGREY);
  if (w > 0) tft.fillRect(40, y + 3, w, 10, col);
}

static void hue(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (h < 85) { r = 255 - h * 3; g = h * 3; b = 0; }
  else if (h < 170) { h -= 85; r = 0; g = 255 - h * 3; b = h * 3; }
  else { h -= 170; r = h * 3; g = 0; b = 255 - h * 3; }
}

static uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void applyOutput() {
  static uint8_t prevLed[3] = {1, 1, 1};
  static uint32_t lastLed = 0, lastRumble = 0;
  ControllerPtr c = activeCtl();
  if (!c) { memset(prevLed, 0xFF, sizeof prevLed); return; }
  uint32_t now = millis();

  uint8_t r = 0, g = 0, b = 0, weak = 0, strong = 0;
  if (outMode == 0) {
    r = clamp8((c->axisX() + 512) / 4);
    g = clamp8((c->axisY() + 512) / 4);
    b = clamp8(c->throttle() / 4);
    weak = clamp8(c->throttle() / 4);
    strong = clamp8(c->brake() / 4);
  } else if (outMode == 1) {
    hue((now / 12) & 0xFF, r, g, b);
  }

  if (now - lastLed >= 50) {
    uint8_t cur[3] = {r, g, b};
    if (memcmp(cur, prevLed, sizeof cur) != 0) {
      c->setColorLED(r, g, b);
      memcpy(prevLed, cur, sizeof cur);
    }
    lastLed = now;
  }
  if ((weak || strong) && now - lastRumble >= 100) {
    c->playDualRumble(0, 150, weak, strong);
    lastRumble = now;
  }
}

void handleButtons() {
  static bool lastN = true, lastM = true;
  static uint32_t tN = 0, tM = 0, pressM = 0;
  static bool longDone = false;
  uint32_t now = millis();
  bool n = digitalRead(BTN_NEXT), m = digitalRead(BTN_MODE);

  if (!n && lastN && now - tN > 200) {
    tN = now;
    page = (page + 1) % NPAGES;
    header();
  }

  if (!m && lastM) { pressM = now; longDone = false; }
  if (!m && !longDone && page == 0 && now - pressM >= 2000) {
    longDone = true;
    BP32.forgetBluetoothKeys();
    forgetMsgUntil = now + 3000;
  }
  if (m && !lastM && !longDone && now - tM > 200 && now - pressM < 1000) {
    tM = now;
    if (page == 4) maxGap = 0;
    else if (page != 0) outMode = (outMode + 1) % 3;
  }
  lastN = n;
  lastM = m;
}

static void noCtl() {
  line(0, TFT_YELLOW, "Belum ada kontroler");
  for (int i = 1; i < 7; i++) line(i, TFT_BLACK, "");
}

void drawPage() {
  ControllerPtr c = activeCtl();
  switch (page) {
    case 0: {
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
      if (millis() < forgetMsgUntil) line(4, TFT_ORANGE, "Pairing dihapus!");
      else line(4, TFT_DARKGREY, "DS4: tahan PS+Share utk pairing");
      line(5, TFT_DARKGREY, "GPIO0 tahan 2dtk: lupakan pairing");
      line(6, TFT_DARKGREY, "GPIO35: halaman berikutnya");
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
    case 3:
      line(0, TFT_WHITE, "Mode (GPIO0 ganti):");
      line(1, TFT_GREEN, "%u. %s", outMode + 1, MODES[outMode]);
      line(2, c ? TFT_WHITE : TFT_YELLOW, c ? "Kontroler terhubung" : "Belum ada kontroler");
      line(3, TFT_DARKGREY, "Mode 1: R/G = stik kiri,");
      line(4, TFT_DARKGREY, "B = R2; rumble kecil = R2,");
      line(5, TFT_DARKGREY, "rumble besar = L2");
      break;
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
  if (now - lastDraw >= 66) {
    lastDraw = now;
    drawPage();
  }
  vTaskDelay(1);
}
