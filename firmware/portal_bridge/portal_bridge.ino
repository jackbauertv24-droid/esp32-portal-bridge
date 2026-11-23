// Portal Bridge firmware for the Waveshare ESP32-S3-A7670E-4G.
// Milestone 1: hotspot with admin page, modem/SIM diagnostics, read-only SMS
// inbox, downloadable diagnostics report, and firmware upload from the browser.
#include <WiFi.h>

#include "battery.h"
#include "board.h"
#include "build_info.h"
#include "devlog.h"
#include "modem.h"
#include "settings.h"
#include "web.h"

static const IPAddress AP_IP(192, 168, 0, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

static void led(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(RGB_LED_PIN, r, g, b); }

void setup() {
  Serial.begin(115200);
  delay(300);
  devlog_begin();
  // First line on every boot: identifies exactly which build is running.
  devlog("=== " FW_NAME " " FW_VERSION " commit " FW_COMMIT " built " FW_BUILT " ===");
  devlog("chip %s rev %d, flash %u KB, psram %u KB", ESP.getChipModel(), ESP.getChipRevision(),
         (unsigned)(ESP.getFlashChipSize() / 1024), (unsigned)(ESP.getPsramSize() / 1024));
  led(0, 0, 30);  // blue: booting

  settings_load();
  battery_begin();
  modem_start_task();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  bool ok = WiFi.softAP(settings.apSsid.c_str(), settings.apPass.c_str(), 6, 0, 8);
  devlog("wifi: hotspot \"%s\" %s at %s%s", settings.apSsid.c_str(), ok ? "up" : "FAILED",
         WiFi.softAPIP().toString().c_str(), settings.setupDone ? "" : " (default password, setup pending)");
  web_begin();
}

void loop() {
  web_loop();
  static uint32_t last = 0;
  if (millis() - last > 1000) {
    last = millis();
    // LED: yellow = setup pending, red = modem not answering, green = modem up.
    if (!settings.setupDone) led(30, 20, 0);
    else if (!modem_status().alive) led(30, 0, 0);
    else led(0, 30, 0);
  }
  delay(2);
}
