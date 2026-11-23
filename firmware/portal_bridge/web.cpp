#include "web.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#include "battery.h"
#include "build_info.h"
#include "devlog.h"
#include "modem.h"
#include "redact.h"
#include "settings.h"
#include "util.h"
#include "web_pages.h"

static WebServer server(80);
static bool uploadAuthorized = false;
static bool firstChunk = true;
static String uploadError;

static const char *reset_reason_text() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (power dip)";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
  }
}

static String fw_id() { return String(FW_NAME " " FW_VERSION " (" FW_COMMIT ", built " FW_BUILT ")"); }

static bool auth() {
  if (server.authenticate("admin", settings.adminPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Portal Bridge", "Login required");
  return false;
}

// Until the first-run setup is done, every page leads to it.
static bool needs_setup_redirect() {
  if (settings.setupDone) return false;
  server.sendHeader("Location", "/setup");
  server.send(302, "text/plain", "setup required");
  return true;
}

static void send_page(const char *body, const char *extraTop = "") {
  String h;
  h.reserve(9000);
  h += FPSTR(PAGE_HEAD);
  h += "<header><b>Portal Bridge</b><small>" + html_escape(fw_id()) + "</small></header>";
  h += FPSTR(PAGE_NAV);
  h += extraTop;
  h += FPSTR(body);
  h += "</body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void handle_status_api() {
  if (!auth()) return;
  ModemStatus m = modem_status();
  BatteryInfo b = battery_read();
  String j = "{\"modem\":{";
  j += "\"alive\":" + String(m.alive ? "true" : "false");
  j += ",\"model\":";
  json_str(j, m.model);
  j += ",\"revision\":";
  json_str(j, m.revision);
  j += ",\"imei\":";
  json_str(j, m.imei);
  j += ",\"iccid\":";
  json_str(j, m.iccid);
  j += ",\"sim\":";
  json_str(j, m.simState);
  j += ",\"own_number\":";
  json_str(j, m.ownNumber);
  j += ",\"operator\":";
  json_str(j, m.operatorName);
  j += ",\"csq\":" + String(m.csq) + ",\"rssi_dbm\":" + String(m.rssiDbm);
  j += ",\"reg_eps\":" + String(m.regEps) + ",\"reg_eps_text\":";
  json_str(j, reg_state_name(m.regEps));
  j += ",\"reg_cs_text\":";
  json_str(j, reg_state_name(m.regCs));
  j += ",\"cpsi\":";
  json_str(j, m.cpsi);
  j += ",\"sms_storage\":";
  json_str(j, m.smsStorage);
  j += ",\"sms_notifications\":" + String(m.smsNotifications);
  j += "},\"board\":{\"fw\":";
  json_str(j, fw_id());
  j += ",\"uptime_s\":" + String(millis() / 1000);
  j += ",\"reset_reason\":";
  json_str(j, reset_reason_text());
  j += ",\"psram_kb\":" + String(ESP.getPsramSize() / 1024);
  j += ",\"flash_mb\":" + String(ESP.getFlashChipSize() / (1024 * 1024));
  j += ",\"heap_kb\":" + String(ESP.getFreeHeap() / 1024);
  j += ",\"battery_found\":" + String(b.found ? "true" : "false");
  j += ",\"battery_v\":" + String(b.volts, 3) + ",\"battery_pct\":" + String(b.percent, 1);
  j += "},\"ap\":{\"ssid\":";
  json_str(j, settings.apSsid);
  j += ",\"ip\":";
  json_str(j, WiFi.softAPIP().toString());
  j += ",\"clients\":" + String(WiFi.softAPgetStationNum()) + "}}";
  server.send(200, "application/json", j);
}

static void handle_setup_get() {
  if (!auth()) return;
  String f = "<main><div class=\"card\"><h2>";
  f += settings.setupDone ? "Change passwords" : "First-time setup";
  f += "</h2>";
  if (!settings.setupDone)
    f += "<p class=\"warn\">The board is still using the public default password. Set your own before anything "
         "else.</p>";
  f += "<form method=\"post\" action=\"/setup\">"
       "<label>Admin password (user name stays <b>admin</b>), 8+ characters</label>"
       "<input name=\"admin\" type=\"password\" minlength=\"8\" required>"
       "<label>Hotspot name</label><input name=\"ssid\" maxlength=\"32\" required value=\"" +
       html_escape(settings.apSsid) +
       "\">"
       "<label>Hotspot password, 8 to 63 characters</label>"
       "<input name=\"appass\" type=\"password\" minlength=\"8\" maxlength=\"63\" required>"
       "<button>Save and restart</button></form></div></main>";
  // Built at run time, so it is passed as the "extra" part with an empty body.
  String h;
  h += FPSTR(PAGE_HEAD);
  h += "<header><b>Portal Bridge</b><small>" + html_escape(fw_id()) + "</small></header>";
  if (settings.setupDone) h += FPSTR(PAGE_NAV);
  h += f + "</body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void handle_setup_post() {
  if (!auth()) return;
  String admin = server.arg("admin"), ssid = server.arg("ssid"), appass = server.arg("appass");
  ssid.trim();
  if (admin.length() < 8 || appass.length() < 8 || appass.length() > 63 || ssid.isEmpty() || ssid.length() > 32) {
    server.send(400, "text/plain", "Invalid values: passwords need 8+ characters (hotspot max 63), name 1-32.");
    return;
  }
  settings.adminPass = admin;
  settings.apSsid = ssid;
  settings.apPass = appass;
  settings.setupDone = true;
  settings_save();
  devlog("setup: passwords and hotspot name saved, restarting");
  String msg = "<main><div class=\"card\"><h2>Saved</h2><p>The board restarts now. Reconnect to the hotspot <b>" +
               html_escape(ssid) +
               "</b> with the new password, then open <b>http://" + WiFi.softAPIP().toString() +
               "</b> and log in as <b>admin</b> with the new admin password.</p></div></main>";
  String h;
  h += FPSTR(PAGE_HEAD);
  h += msg + "</body></html>";
  server.send(200, "text/html; charset=utf-8", h);
  delay(1500);
  ESP.restart();
}

static void handle_at_api() {
  if (!auth() || needs_setup_redirect()) return;
  String cmd = server.arg("cmd");
  cmd.trim();
  if (!cmd.length() || cmd.length() > 200) {
    server.send(400, "text/plain", "empty or too long");
    return;
  }
  bool ok = false;
  String r = modem_cmd(cmd.c_str(), 15000, &ok);
  server.send(200, "text/plain; charset=utf-8", r + (ok ? "\nOK" : ""));
}

static void handle_sms_api() {
  if (!auth() || needs_setup_redirect()) return;
  server.send(200, "application/json; charset=utf-8", modem_list_sms_json());
}

static void handle_log() {
  if (!auth() || needs_setup_redirect()) return;
  String s = fw_id() + "\n\n";
  devlog_dump(s);
  server.send(200, "text/plain; charset=utf-8", s);
}

String build_report() {
  ModemStatus m = modem_status();
  BatteryInfo b = battery_read();
  String r;
  r.reserve(32 * 1024);
  r += "=== portal-bridge diagnostics report ===\n";
  r += "firmware: " + fw_id() + "\n";
  r += "uptime_s: " + String(millis() / 1000) + "\nreset: " + reset_reason_text() + "\n";
  r += "chip: " + String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()) +
       ", cores " + String(ESP.getChipCores()) + ", " + String(ESP.getCpuFreqMHz()) + " MHz\n";
  r += "flash: " + String(ESP.getFlashChipSize() / 1024) + " KB, psram: " + String(ESP.getPsramSize() / 1024) +
       " KB, free heap: " + String(ESP.getFreeHeap() / 1024) + " KB\n";
  r += "battery gauge: " +
       (b.found ? String("found on SDA ") + b.sda + " / SCL " + b.scl + ", " + String(b.volts, 3) + " V, " +
                      String(b.percent, 1) + " %"
                : String("not found")) +
       "\n";
  r += "ap: clients " + String(WiFi.softAPgetStationNum()) + ", ip " + WiFi.softAPIP().toString() + "\n";
  r += "modem alive: " + String(m.alive ? "yes" : "no") + ", first answer after " + String(m.bootWaitMs) +
       " ms\n";
  r += "model: " + m.model + " / " + m.revision + "\n";
  r += "sim: " + m.simState + ", operator: " + m.operatorName + ", csq: " + String(m.csq) + "\n";
  r += "reg eps: " + String(m.regEps) + " (" + reg_state_name(m.regEps) + "), reg cs: " + String(m.regCs) + " (" +
       reg_state_name(m.regCs) + ")\n";
  r += "cpsi: " + m.cpsi + "\nsms storage: " + m.smsStorage + "\n";
  r += "imei/iccid/imsi/own number: " + m.imei + " / " + m.iccid + " / " + m.imsi + " / " + m.ownNumber + "\n";
  r += "urc count: " + String(m.urcCount) + ", +CMTI since boot: " + String(m.smsNotifications) + "\n";
  if (m.alive) {
    int count = 0, errs = 0;
    modem_list_sms_json(&count, &errs);  // contents are deliberately not included
    r += "sms listed: " + String(count) + ", decode errors: " + String(errs) + "\n";
  }
  r += "\n=== device log (modem traffic included, SMS PDUs omitted) ===\n";
  devlog_dump(r);
  // Mask identifiers last so nothing above can bypass it.
  char *buf = (char *)malloc(r.length() + 1);
  if (!buf) return "out of memory building report";
  memcpy(buf, r.c_str(), r.length() + 1);
  redact_long_numbers(buf);
  String out(buf);
  free(buf);
  return out;
}

static void handle_report() {
  if (!auth()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=\"portal-bridge-report.txt\"");
  server.send(200, "text/plain; charset=utf-8", build_report());
}

static void handle_update_done() {
  if (!uploadAuthorized) {
    auth();
    return;
  }
  bool ok = uploadError.isEmpty() && !Update.hasError();
  String msg = ok ? "Update installed. Restarting… reconnect in about 20 seconds."
                  : "Update failed: " + (uploadError.length() ? uploadError : String(Update.errorString()));
  server.send(ok ? 200 : 500, "text/plain; charset=utf-8", msg);
  devlog("ota: %s", msg.c_str());
  if (ok) {
    delay(1000);
    ESP.restart();
  }
}

static void handle_update_upload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadAuthorized = server.authenticate("admin", settings.adminPass.c_str());
    uploadError = "";
    firstChunk = true;
    if (!uploadAuthorized) return;
    devlog("ota: receiving %s", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) uploadError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadAuthorized || uploadError.length()) return;
    // A merged "-full" image starts with the bootloader, not an app; refuse it early.
    if (firstChunk && up.currentSize > 0 && up.buf[0] != 0xE9) {
      uploadError = "this is not an app image; use the -ota.bin file";
      Update.abort();
      return;
    }
    firstChunk = false;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) uploadError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (!uploadAuthorized || uploadError.length()) return;
    if (!Update.end(true)) uploadError = Update.errorString();
    else devlog("ota: %u bytes written", (unsigned)up.totalSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    uploadError = "upload aborted";
  }
}

void web_begin() {
  server.on("/", HTTP_GET, [] {
    if (!auth() || needs_setup_redirect()) return;
    send_page(PAGE_STATUS);
  });
  server.on("/api/status", HTTP_GET, handle_status_api);
  server.on("/setup", HTTP_GET, handle_setup_get);
  server.on("/setup", HTTP_POST, handle_setup_post);
  server.on("/sms", HTTP_GET, [] {
    if (!auth() || needs_setup_redirect()) return;
    send_page(PAGE_SMS);
  });
  server.on("/api/sms", HTTP_GET, handle_sms_api);
  server.on("/at", HTTP_GET, [] {
    if (!auth() || needs_setup_redirect()) return;
    send_page(PAGE_AT);
  });
  server.on("/api/at", HTTP_POST, handle_at_api);
  server.on("/log", HTTP_GET, handle_log);
  server.on("/report.txt", HTTP_GET, handle_report);
  server.on("/update", HTTP_GET, [] {
    if (!auth() || needs_setup_redirect()) return;
    send_page(PAGE_UPDATE);
  });
  server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  server.on("/reboot", HTTP_POST, [] {
    if (!auth()) return;
    server.send(200, "text/plain", "Rebooting…");
    delay(500);
    ESP.restart();
  });
  server.onNotFound([] {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
  devlog("web: admin page on http://%s/", WiFi.softAPIP().toString().c_str());
}

void web_loop() { server.handleClient(); }
