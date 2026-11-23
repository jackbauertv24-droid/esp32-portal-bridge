#include "settings.h"

#include <Preferences.h>
#include <WiFi.h>

Settings settings;
static Preferences prefs;

String default_ap_ssid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "PortalBridge-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

void settings_load() {
  prefs.begin("pb", true);
  settings.apSsid = prefs.getString("ap_ssid", "");
  settings.apPass = prefs.getString("ap_pass", DEFAULT_PASSWORD);
  settings.adminPass = prefs.getString("admin_pass", DEFAULT_PASSWORD);
  settings.setupDone = prefs.getBool("setup_done", false);
  prefs.end();
  if (settings.apSsid.isEmpty()) settings.apSsid = default_ap_ssid();
}

void settings_save() {
  prefs.begin("pb", false);
  prefs.putString("ap_ssid", settings.apSsid);
  prefs.putString("ap_pass", settings.apPass);
  prefs.putString("admin_pass", settings.adminPass);
  prefs.putBool("setup_done", settings.setupDone);
  prefs.end();
}
