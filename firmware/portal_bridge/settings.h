// Settings kept in flash (NVS), edited from the admin page.
// Nothing user-specific is compiled into the firmware: the repository is public.
#pragma once
#include <Arduino.h>

// Used until the first-run setup page has been completed. Documented in the README.
#define DEFAULT_PASSWORD "portalbridge"

struct Settings {
  String apSsid;     // hotspot name; default "PortalBridge-XXXX" from the MAC
  String apPass;     // hotspot WPA2 password, 8..63 chars
  String adminPass;  // admin page password (user name is always "admin")
  bool setupDone = false;
};

extern Settings settings;

void settings_load();
void settings_save();
String default_ap_ssid();
