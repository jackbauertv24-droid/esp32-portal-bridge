#include "modem.h"

#include "board.h"
#include "devlog.h"
#include "sms_pdu.h"
#include "util.h"

static HardwareSerial &uart = Serial1;
static SemaphoreHandle_t lock = nullptr;
static ModemStatus st;
static String rxLine;

const char *reg_state_name(int stat) {
  switch (stat) {
    case 0: return "not registered, not searching";
    case 1: return "registered (home)";
    case 2: return "searching";
    case 3: return "registration denied";
    case 4: return "unknown";
    case 5: return "registered (roaming)";
    case 6: return "registered (SMS only, home)";
    case 7: return "registered (SMS only, roaming)";
    case 11: return "emergency only";
    default: return "-";
  }
}

// Long hex lines are SMS PDUs; they hold private content and never reach the log.
static bool looks_like_pdu(const String &l) {
  if (l.length() < 24) return false;
  for (size_t i = 0; i < l.length(); i++)
    if (!isxdigit((unsigned char)l[i])) return false;
  return true;
}

static void log_rx(const char *tag, const String &l) {
  if (looks_like_pdu(l)) devlog("%s <pdu, %u hex chars>", tag, (unsigned)l.length());
  else devlog("%s %s", tag, l.c_str());
}

static void handle_urc(const String &l) {
  st.urcCount++;
  log_rx("URC", l);
  if (l.startsWith("+CMTI:")) st.smsNotifications++;
}

// Reads one complete line if available within `waitMs`. Empty lines are skipped.
static bool read_line(String &out, uint32_t waitMs) {
  uint32_t start = millis();
  do {
    while (uart.available()) {
      char c = (char)uart.read();
      if (c == '\n') {
        rxLine.trim();
        if (rxLine.length()) {
          out = rxLine;
          rxLine = "";
          return true;
        }
      } else if (c != '\r') {
        if (rxLine.length() < 2048) rxLine += c;
      }
    }
    if (waitMs) vTaskDelay(pdMS_TO_TICKS(5));
  } while (millis() - start < waitMs);
  return false;
}

static bool is_final(const String &l, bool &ok) {
  if (l == "OK") {
    ok = true;
    return true;
  }
  if (l == "ERROR" || l.startsWith("+CME ERROR") || l.startsWith("+CMS ERROR") || l == "NO CARRIER") {
    ok = false;
    return true;
  }
  return false;
}

static String cmd_locked(const char *cmd, uint32_t timeoutMs, bool *okOut) {
  String l;
  while (read_line(l, 0)) handle_urc(l);  // anything already waiting is unsolicited
  devlog("AT> %s", cmd);
  uart.print(cmd);
  uart.print("\r");
  String resp;
  bool ok = false, done = false;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!read_line(l, 50)) continue;
    if (l == cmd) continue;  // echo
    log_rx("AT<", l);
    if (is_final(l, ok)) {
      done = true;
      if (!ok) {
        if (resp.length()) resp += "\n";
        resp += l;
      }
      break;
    }
    if (resp.length()) resp += "\n";
    resp += l;
  }
  if (!done) devlog("AT! timeout after %lu ms: %s", (unsigned long)timeoutMs, cmd);
  if (okOut) *okOut = ok;
  return resp;
}

String modem_cmd(const char *cmd, uint32_t timeoutMs, bool *ok) {
  xSemaphoreTake(lock, portMAX_DELAY);
  String r = cmd_locked(cmd, timeoutMs, ok);
  xSemaphoreGive(lock);
  return r;
}

// Value after "PREFIX: " on the first line that has it, else "".
static String field(const String &resp, const char *prefix) {
  int p = resp.indexOf(prefix);
  if (p < 0) return "";
  int s = p + strlen(prefix);
  while (s < (int)resp.length() && resp[s] == ' ') s++;
  int e = resp.indexOf('\n', s);
  return resp.substring(s, e < 0 ? resp.length() : e);
}

// First line of a reply that is not a "+XXX:" header, e.g. the IMEI for AT+CGSN.
static String bare(const String &resp) {
  int e = resp.indexOf('\n');
  String l = e < 0 ? resp : resp.substring(0, e);
  return l.startsWith("+") || l.indexOf("ERROR") >= 0 ? String("") : l;
}

static int stat_of(const String &v) {  // "0,1" or "2,1,..." -> second number
  int c = v.indexOf(',');
  if (c < 0) return -1;
  return v.substring(c + 1).toInt();
}

static String quoted(const String &v, int nth) {  // nth quoted string in v
  int from = 0;
  for (int i = 0; i <= nth; i++) {
    int a = v.indexOf('"', from);
    if (a < 0) return "";
    int b = v.indexOf('"', a + 1);
    if (b < 0) return "";
    if (i == nth) return v.substring(a + 1, b);
    from = b + 1;
  }
  return "";
}

static void refresh_locked() {
  String r;
  r = cmd_locked("AT+CPIN?", 3000, nullptr);
  st.simState = field(r, "+CPIN:");
  if (st.simState.isEmpty()) st.simState = r;
  r = cmd_locked("AT+CSQ", 2000, nullptr);
  st.csq = field(r, "+CSQ:").toInt();
  if (field(r, "+CSQ:").isEmpty()) st.csq = 99;
  st.rssiDbm = (st.csq >= 0 && st.csq <= 31) ? -113 + 2 * st.csq : 0;
  st.regCs = stat_of(field(cmd_locked("AT+CREG?", 2000, nullptr), "+CREG:"));
  st.regEps = stat_of(field(cmd_locked("AT+CEREG?", 2000, nullptr), "+CEREG:"));
  st.operatorName = quoted(field(cmd_locked("AT+COPS?", 5000, nullptr), "+COPS:"), 0);
  st.cpsi = field(cmd_locked("AT+CPSI?", 3000, nullptr), "+CPSI:");
  st.smsStorage = field(cmd_locked("AT+CPMS?", 3000, nullptr), "+CPMS:");
  if (st.ownNumber.isEmpty() && st.simState == "READY")
    st.ownNumber = quoted(field(cmd_locked("AT+CNUM", 3000, nullptr), "+CNUM:"), 1);
  st.lastRefreshMs = millis();
}

// One-off commands whose answers matter for later milestones (SMS over LTE,
// data, storage). Only logged; the report collects them from the log.
static const char *const PROBE_ONCE[] = {
    "ATI",          "AT+CGMR",     "AT+CSUB",    "AT+CNMP?",   "AT+CGDCONT?", "AT+CGATT?",  "AT+CGPADDR",
    "AT+CSMS?",     "AT+CSCA?",    "AT+CMGF?",   "AT+CSCS?",   "AT+CNMI?",    "AT+CNMI=?",  "AT+CPMS=?",
    "AT+CSDH?",     "AT+CUSBCFG?", "AT+CGNSSPWR?", "AT+CBC",   "AT+CPMUTEMP",
};

static void identify_locked() {
  bool ok;
  cmd_locked("ATE0", 2000, nullptr);
  cmd_locked("AT+CMEE=2", 2000, nullptr);  // verbose error text
  st.model = bare(cmd_locked("AT+CGMM", 2000, nullptr));
  st.revision = field(cmd_locked("AT+CGMR", 2000, nullptr), "+CGMR:");
  st.imei = bare(cmd_locked("AT+CGSN", 2000, nullptr));
  st.iccid = field(cmd_locked("AT+CICCID", 2000, &ok), "+ICCID:");
  st.imsi = bare(cmd_locked("AT+CIMI", 2000, nullptr));
  for (const char *c : PROBE_ONCE) cmd_locked(c, 5000, nullptr);
}

static void modem_task(void *) {
  pinMode(MODEM_EN_PIN, OUTPUT);
  digitalWrite(MODEM_EN_PIN, HIGH);
  uart.setRxBufferSize(8192);
  uart.begin(MODEM_BAUD, SERIAL_8N1, MODEM_UART_RX, MODEM_UART_TX);
  devlog("modem: UART1 rx=%d tx=%d %d baud, enable pin %d held HIGH", MODEM_UART_RX, MODEM_UART_TX, MODEM_BAUD,
         MODEM_EN_PIN);

  uint32_t t0 = millis();
  for (;;) {
    // Wait for the modem to answer. It takes ~10-20 s after power-up.
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = false;
    cmd_locked("AT", 1000, &ok);
    if (ok) {
      st.alive = true;
      st.aliveSinceMs = millis();
      if (!st.bootWaitMs) st.bootWaitMs = millis() - t0;
      devlog("modem: answered AT after %lu ms", (unsigned long)(millis() - t0));
      identify_locked();
      refresh_locked();
      xSemaphoreGive(lock);
      break;
    }
    xSemaphoreGive(lock);
    uint32_t waited = millis() - t0;
    if (waited > 45000 && waited < 46500)
      devlog("modem: no answer after 45 s. Check the 4G DIP switch is ON and the USB switch routes the modem's "
             "UART to the ESP32. Still retrying.");
    vTaskDelay(pdMS_TO_TICKS(waited > 60000 ? 5000 : 1000));
  }

  for (;;) {
    // Pick up URCs between refreshes, refresh every 15 s.
    for (int i = 0; i < 150; i++) {
      if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        String l;
        while (read_line(l, 0)) handle_urc(l);
        xSemaphoreGive(lock);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = false;
    cmd_locked("AT", 1500, &ok);
    if (!ok) {
      if (st.alive) devlog("modem: stopped answering");
      st.alive = false;
    } else {
      if (!st.alive) {
        devlog("modem: answering again");
        st.alive = true;
        identify_locked();
      }
      refresh_locked();
    }
    xSemaphoreGive(lock);
  }
}

void modem_start_task() {
  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(modem_task, "modem", 8192, nullptr, 2, nullptr, 1);
}

ModemStatus modem_status() {
  xSemaphoreTake(lock, portMAX_DELAY);
  ModemStatus copy = st;
  xSemaphoreGive(lock);
  return copy;
}

String modem_list_sms_json(int *count, int *decodeErrors) {
  String out = "[";
  int n = 0, errs = 0;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (!st.alive) {
    xSemaphoreGive(lock);
    if (count) *count = -1;
    return "[]";
  }
  cmd_locked("AT+CMGF=0", 2000, nullptr);
  String r = cmd_locked("AT+CMGL=4", 20000, nullptr);
  xSemaphoreGive(lock);

  sms_t *m = (sms_t *)malloc(sizeof(sms_t));
  int pos = 0;
  while (m && pos < (int)r.length()) {
    int e = r.indexOf('\n', pos);
    String line = r.substring(pos, e < 0 ? r.length() : e);
    pos = e < 0 ? r.length() : e + 1;
    if (!line.startsWith("+CMGL:")) continue;
    int index = line.substring(6).toInt();
    int c1 = line.indexOf(',');
    int stat = c1 > 0 ? line.substring(c1 + 1).toInt() : -1;
    e = r.indexOf('\n', pos);
    String pdu = r.substring(pos, e < 0 ? r.length() : e);
    pos = e < 0 ? r.length() : e + 1;
    int rc = sms_pdu_decode(pdu.c_str(), m);
    if (n) out += ",";
    n++;
    out += "{\"index\":" + String(index) + ",\"stat\":" + String(stat);
    if (rc == 0) {
      out += ",\"from\":";
      json_str(out, m->sender);
      out += ",\"time\":";
      json_str(out, m->timestamp);
      out += ",\"text\":";
      json_str(out, m->text);
      out += ",\"enc\":" + String((int)m->encoding);
      out += ",\"part\":" + String(m->concat_seq) + ",\"parts\":" + String(m->concat_total) +
             ",\"ref\":" + String(m->concat_ref);
    } else {
      errs++;
      out += ",\"error\":";
      json_str(out, sms_pdu_error(rc));
      out += ",\"pdu_len\":" + String(pdu.length());
    }
    out += "}";
  }
  free(m);
  out += "]";
  if (count) *count = n;
  if (decodeErrors) *decodeErrors = errs;
  devlog("sms: listed %d message(s), %d decode error(s)", n, errs);
  return out;
}
