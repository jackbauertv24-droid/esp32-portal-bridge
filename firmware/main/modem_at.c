#include "modem_at.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "devlog.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"
#include "sms_pdu.h"
#include "smsstore.h"

#define UART UART_NUM_1

typedef struct {
  bool alive;
  int64_t boot_wait_ms;
  char model[32], revision[48], imei[24], iccid[28], imsi[20], own_number[24];
  char sim[32], oper[40], cpsi[120], cpms[80];
  int csq, reg_cs, reg_eps;
  unsigned urc_count, cmti_count, stored_count, sim_deleted;
  int64_t last_refresh_us;
} mstate_t;

static mstate_t st = {.csq = 99, .reg_cs = -1, .reg_eps = -1};
static SemaphoreHandle_t lock;  // serialises AT traffic and guards `st`
static char rxbuf[2048];
static size_t rxlen;
static volatile bool sms_poll_now;

// ---------- line I/O ----------

static bool looks_like_pdu(const char *l) {
  size_t n = strlen(l);
  if (n < 24) return false;
  for (size_t i = 0; i < n; i++)
    if (!isxdigit((unsigned char)l[i])) return false;
  return true;
}

static void log_line(const char *tag, const char *l) {
  if (looks_like_pdu(l)) devlog("%s <pdu, %u hex chars>", tag, (unsigned)strlen(l));
  else devlog("%s %s", tag, l);
}

// Reads one non-empty line into `out` within `wait_ms`. Returns true if found.
static bool read_line(char *out, size_t cap, int wait_ms) {
  int64_t until = esp_timer_get_time() + (int64_t)wait_ms * 1000;
  for (;;) {
    uint8_t c;
    int n = uart_read_bytes(UART, &c, 1, wait_ms > 0 ? pdMS_TO_TICKS(10) : 0);
    if (n == 1) {
      if (c == '\n') {
        while (rxlen && (rxbuf[rxlen - 1] == '\r' || rxbuf[rxlen - 1] == ' ')) rxlen--;
        if (rxlen) {
          size_t k = rxlen < cap - 1 ? rxlen : cap - 1;
          memcpy(out, rxbuf, k);
          out[k] = 0;
          rxlen = 0;
          return true;
        }
      } else if (c != '\r' || rxlen) {
        if (rxlen < sizeof(rxbuf) - 1) rxbuf[rxlen++] = (char)c;
      }
      continue;
    }
    if (wait_ms <= 0 || esp_timer_get_time() >= until) return false;
  }
}

static void handle_urc(const char *l) {
  st.urc_count++;
  log_line("URC", l);
  if (!strncmp(l, "+CMTI:", 6)) {
    st.cmti_count++;
    sms_poll_now = true;
  }
}

static bool is_final(const char *l, bool *ok) {
  if (!strcmp(l, "OK")) {
    *ok = true;
    return true;
  }
  if (!strcmp(l, "ERROR") || !strncmp(l, "+CME ERROR", 10) || !strncmp(l, "+CMS ERROR", 10) ||
      !strcmp(l, "NO CARRIER")) {
    *ok = false;
    return true;
  }
  return false;
}

static char *cmd_locked(const char *cmd, int timeout_ms, bool *ok_out) {
  char line[2048];
  while (read_line(line, sizeof(line), 0)) handle_urc(line);
  devlog("AT> %s", cmd);
  uart_write_bytes(UART, cmd, strlen(cmd));
  uart_write_bytes(UART, "\r", 1);
  size_t cap = 1024, len = 0;
  char *resp = malloc(cap);
  resp[0] = 0;
  bool ok = false, done = false;
  int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
  while (esp_timer_get_time() < until) {
    if (!read_line(line, sizeof(line), 50)) continue;
    if (!strcmp(line, cmd)) continue;  // echo
    log_line("AT<", line);
    bool fin = is_final(line, &ok);
    if (fin && ok) {
      done = true;
      break;
    }
    size_t ll = strlen(line);
    if (len + ll + 2 > cap) {
      cap = (len + ll + 2) * 2;
      resp = realloc(resp, cap);
    }
    if (len) resp[len++] = '\n';
    memcpy(resp + len, line, ll + 1);
    len += ll;
    if (fin) {
      done = true;
      break;
    }
  }
  if (!done) devlog("AT! timeout after %d ms: %s", timeout_ms, cmd);
  if (ok_out) *ok_out = ok;
  return resp;
}

char *modem_cmd(const char *cmd, int timeout_ms, bool *ok) {
  xSemaphoreTake(lock, portMAX_DELAY);
  char *r = cmd_locked(cmd, timeout_ms, ok);
  xSemaphoreGive(lock);
  return r;
}

static void cmd_quiet(const char *cmd, int timeout_ms) { free(cmd_locked(cmd, timeout_ms, NULL)); }

// ---------- reply parsing ----------

// Value after "PREFIX:" on the line that has it.
static void field(const char *resp, const char *prefix, char *out, size_t cap) {
  out[0] = 0;
  const char *p = strstr(resp, prefix);
  if (!p) return;
  p += strlen(prefix);
  while (*p == ' ') p++;
  size_t n = strcspn(p, "\n");
  if (n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
}

// First line unless it is a header or an error (e.g. IMEI for AT+CGSN).
static void bare(const char *resp, char *out, size_t cap) {
  out[0] = 0;
  if (resp[0] == '+' || strstr(resp, "ERROR")) return;
  size_t n = strcspn(resp, "\n");
  if (n >= cap) n = cap - 1;
  memcpy(out, resp, n);
  out[n] = 0;
}

static void quoted(const char *v, int nth, char *out, size_t cap) {
  out[0] = 0;
  const char *p = v;
  for (int i = 0; i <= nth; i++) {
    const char *a = strchr(p, '"');
    if (!a) return;
    const char *b = strchr(a + 1, '"');
    if (!b) return;
    if (i == nth) {
      size_t n = (size_t)(b - a - 1);
      if (n >= cap) n = cap - 1;
      memcpy(out, a + 1, n);
      out[n] = 0;
      return;
    }
    p = b + 1;
  }
}

static int second_number(const char *v) {
  const char *c = strchr(v, ',');
  return c ? atoi(c + 1) : -1;
}

static char *q(const char *cmd, int t) { return cmd_locked(cmd, t, NULL); }

static void refresh_locked(void) {
  char v[160];
  char *r = q("AT+CPIN?", 3000);
  field(r, "+CPIN:", st.sim, sizeof(st.sim));
  if (!st.sim[0]) snprintf(st.sim, sizeof(st.sim), "%.31s", r);
  free(r);
  r = q("AT+CSQ", 2000);
  field(r, "+CSQ:", v, sizeof(v));
  st.csq = v[0] ? atoi(v) : 99;
  free(r);
  r = q("AT+CREG?", 2000);
  field(r, "+CREG:", v, sizeof(v));
  st.reg_cs = second_number(v);
  free(r);
  r = q("AT+CEREG?", 2000);
  field(r, "+CEREG:", v, sizeof(v));
  st.reg_eps = second_number(v);
  free(r);
  r = q("AT+COPS?", 5000);
  field(r, "+COPS:", v, sizeof(v));
  quoted(v, 0, st.oper, sizeof(st.oper));
  free(r);
  r = q("AT+CPSI?", 3000);
  field(r, "+CPSI:", st.cpsi, sizeof(st.cpsi));
  free(r);
  r = q("AT+CPMS?", 3000);
  field(r, "+CPMS:", st.cpms, sizeof(st.cpms));
  free(r);
  if (!st.own_number[0] && !strcmp(st.sim, "READY")) {
    r = q("AT+CNUM", 3000);
    field(r, "+CNUM:", v, sizeof(v));
    quoted(v, 1, st.own_number, sizeof(st.own_number));
    free(r);
  }
  st.last_refresh_us = esp_timer_get_time();
}

// Answers kept only in the log, for diagnosing later features remotely.
static const char *const PROBE_ONCE[] = {"ATI",        "AT+CGMR",   "AT+CSUB",    "AT+CNMP?",  "AT+CGDCONT?",
                                         "AT+CGATT?",  "AT+CGPADDR", "AT+CSMS?",  "AT+CSCA?",  "AT+CNMI=?",
                                         "AT+CPMS=?",  "AT+CUSBCFG?", "AT+CUSBPIDSWITCH?", "AT+CATR?", "AT+CBC",
                                         "AT+CPMUTEMP", NULL};

static void identify_locked(void) {
  char *r;
  cmd_quiet("ATE0", 2000);
  cmd_quiet("AT+CMEE=2", 2000);
  r = q("AT+CGMM", 2000);
  bare(r, st.model, sizeof(st.model));
  free(r);
  r = q("AT+CGMR", 2000);
  field(r, "+CGMR:", st.revision, sizeof(st.revision));
  free(r);
  r = q("AT+CGSN", 2000);
  bare(r, st.imei, sizeof(st.imei));
  free(r);
  r = q("AT+CICCID", 2000);
  field(r, "+ICCID:", st.iccid, sizeof(st.iccid));
  free(r);
  r = q("AT+CIMI", 2000);
  bare(r, st.imsi, sizeof(st.imsi));
  free(r);
  for (int i = 0; PROBE_ONCE[i]; i++) cmd_quiet(PROBE_ONCE[i], 5000);
  // PIN unlock if configured and needed.
  char pin[12];
  cfg_get("sim_pin", pin, sizeof(pin));
  r = q("AT+CPIN?", 3000);
  if (strstr(r, "SIM PIN") && pin[0]) {
    char c[32];
    snprintf(c, sizeof(c), "AT+CPIN=%s", pin);
    devlog("modem: SIM asks for a PIN, sending the configured one");
    uart_write_bytes(UART, c, strlen(c));
    uart_write_bytes(UART, "\r", 1);
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  free(r);
  // SMS: PDU mode, store on arrival and notify (+CMTI). Polling covers the case
  // where notifications go to the USB port instead of the UART.
  cmd_quiet("AT+CMGF=0", 2000);
  cmd_quiet("AT+CNMI=2,1,0,0,0", 2000);
}

// ---------- SMS ingestion ----------

typedef struct {
  int ref;
  int64_t first_seen_us;
} pending_t;
static pending_t pend[8];

static int64_t first_seen(int ref) {
  int64_t now = esp_timer_get_time();
  for (int i = 0; i < 8; i++)
    if (pend[i].ref == ref && pend[i].first_seen_us) return pend[i].first_seen_us;
  for (int i = 0; i < 8; i++)
    if (!pend[i].first_seen_us || now - pend[i].first_seen_us > 3600LL * 1000000) {
      pend[i].ref = ref;
      pend[i].first_seen_us = now;
      return now;
    }
  return now;
}

static void forget_ref(int ref) {
  for (int i = 0; i < 8; i++)
    if (pend[i].ref == ref) pend[i].first_seen_us = 0;
}

typedef struct {
  int index;
  sms_t *m;
} listed_t;

#define MAX_LIST 40

static void delete_index(int index) {
  if (!cfg_bool("sms_del")) return;
  char c[24];
  snprintf(c, sizeof(c), "AT+CMGD=%d", index);
  bool ok = false;
  free(cmd_locked(c, 5000, &ok));
  if (ok) st.sim_deleted++;
}

// Lists the SIM, stores complete messages durably, then removes them from the SIM.
static void poll_sms_locked(void) {
  cmd_quiet("AT+CMGF=0", 2000);
  char *r = cmd_locked("AT+CMGL=4", 20000, NULL);
  listed_t *L = calloc(MAX_LIST, sizeof(listed_t));
  int n = 0;
  const char *p = r;
  while (p && *p && n < MAX_LIST) {
    const char *e = strchr(p, '\n');
    if (!strncmp(p, "+CMGL:", 6) && e) {
      int idx = atoi(p + 6);
      const char *pdu = e + 1;
      size_t pl = strcspn(pdu, "\n");
      char *hex = strndup(pdu, pl);
      sms_t *m = malloc(sizeof(sms_t));
      int rc = sms_pdu_decode(hex, m);
      if (rc == 0) {
        L[n].index = idx;
        L[n].m = m;
        n++;
      } else {
        devlog("sms: index %d not decodable (%s), %u hex chars; stored raw", idx, sms_pdu_error(rc),
               (unsigned)pl);
        // Keep it anyway so nothing is lost silently.
        if (smsstore_add("?", "", hex, 1)) delete_index(idx);
        free(m);
      }
      free(hex);
      p = pdu + pl;
      if (*p) p++;
      continue;
    }
    p = e ? e + 1 : NULL;
  }
  free(r);

  for (int i = 0; i < n; i++) {
    sms_t *m = L[i].m;
    if (!m) continue;
    if (m->concat_ref < 0 || m->concat_total <= 1) {
      uint32_t id = smsstore_add(m->sender, m->timestamp, m->text, 1);
      if (id) {
        st.stored_count++;
        delete_index(L[i].index);
      }
      continue;
    }
    // Multipart: collect parts with the same sender and reference.
    int idx[16] = {0};
    sms_t *part[16] = {0};
    int have = 0;
    for (int k = i; k < n; k++) {
      sms_t *o = L[k].m;
      if (!o || o->concat_ref != m->concat_ref || strcmp(o->sender, m->sender)) continue;
      int s = o->concat_seq - 1;
      if (s >= 0 && s < 16 && !part[s]) {
        part[s] = o;
        idx[s] = L[k].index;
        have++;
      }
    }
    int total = m->concat_total > 16 ? 16 : m->concat_total;
    bool complete = have >= total;
    bool stale = esp_timer_get_time() - first_seen(m->concat_ref) > 10LL * 60 * 1000000;
    if (complete || stale) {
      size_t cap = 16 * SMS_TEXT_MAX + 32;
      char *text = malloc(cap);
      text[0] = 0;
      for (int s = 0; s < total; s++) {
        if (part[s]) strncat(text, part[s]->text, cap - strlen(text) - 1);
        else strncat(text, " [missing part] ", cap - strlen(text) - 1);
      }
      uint32_t id = smsstore_add(m->sender, m->timestamp, text, total);
      free(text);
      if (id) {
        st.stored_count++;
        for (int s = 0; s < total; s++)
          if (part[s]) delete_index(idx[s]);
        forget_ref(m->concat_ref);
      }
    }
    // Mark all parts of this message as handled for this pass.
    for (int k = i; k < n; k++)
      if (L[k].m && L[k].m->concat_ref == m->concat_ref && !strcmp(L[k].m->sender, m->sender) && L[k].m != m) {
        free(L[k].m);
        L[k].m = NULL;
      }
  }
  for (int i = 0; i < n; i++) free(L[i].m);
  free(L);
}

// ---------- task ----------

static void modem_task(void *arg) {
  gpio_reset_pin(MODEM_EN_PIN);
  gpio_set_direction(MODEM_EN_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(MODEM_EN_PIN, 1);
  uart_config_t uc = {.baud_rate = MODEM_BAUD,
                      .data_bits = UART_DATA_8_BITS,
                      .parity = UART_PARITY_DISABLE,
                      .stop_bits = UART_STOP_BITS_1,
                      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                      .source_clk = UART_SCLK_DEFAULT};
  uart_driver_install(UART, 16384, 2048, 0, NULL, 0);
  uart_param_config(UART, &uc);
  uart_set_pin(UART, MODEM_UART_TX, MODEM_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  devlog("modem: UART1 rx=%d tx=%d %d baud, enable GPIO%d held high", MODEM_UART_RX, MODEM_UART_TX, MODEM_BAUD,
         MODEM_EN_PIN);

  int64_t t0 = esp_timer_get_time();
  bool warned = false;
  for (;;) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = false;
    free(cmd_locked("AT", 1000, &ok));
    if (ok) {
      st.alive = true;
      st.boot_wait_ms = (esp_timer_get_time() - t0) / 1000;
      devlog("modem: answered AT after %lld ms", st.boot_wait_ms);
      identify_locked();
      refresh_locked();
      xSemaphoreGive(lock);
      break;
    }
    xSemaphoreGive(lock);
    int64_t waited = (esp_timer_get_time() - t0) / 1000;
    if (waited > 45000 && !warned) {
      warned = true;
      event_post("Modem not answering on the serial port after 45 s. Check the 4G DIP switch is ON.");
    }
    vTaskDelay(pdMS_TO_TICKS(waited > 60000 ? 5000 : 1000));
  }

  int64_t next_sms = 0, next_refresh = 0;
  for (;;) {
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    char line[2048];
    while (read_line(line, sizeof(line), 0)) handle_urc(line);
    if (now >= next_refresh) {
      bool ok = false;
      free(cmd_locked("AT", 1500, &ok));
      if (!ok) {
        if (st.alive) event_post("Modem stopped answering on the serial port");
        st.alive = false;
      } else {
        if (!st.alive) {
          devlog("modem: answering again");
          st.alive = true;
          identify_locked();
        }
        refresh_locked();
      }
      next_refresh = now + 15LL * 1000000;
    }
    if (st.alive && (sms_poll_now || now >= next_sms)) {
      sms_poll_now = false;
      poll_sms_locked();
      next_sms = now + (int64_t)cfg_int("sms_poll_s") * 1000000;
    }
    xSemaphoreGive(lock);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void modem_uart_start(void) {
  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(modem_task, "modem_at", 12288, NULL, 5, NULL, 1);
}

bool modem_alive(void) { return st.alive; }

void modem_own_number(char *out, size_t cap) {
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(out, cap, "%s", st.own_number);
  xSemaphoreGive(lock);
}

static const char *reg_name(int s) {
  switch (s) {
    case 0: return "not registered";
    case 1: return "registered (home)";
    case 2: return "searching";
    case 3: return "denied";
    case 5: return "registered (roaming)";
    case 6: return "SMS only (home)";
    case 7: return "SMS only (roaming)";
    default: return "-";
  }
}

void modem_status_json(cJSON *root) {
  cJSON *m = cJSON_AddObjectToObject(root, "modem");
  xSemaphoreTake(lock, portMAX_DELAY);
  cJSON_AddBoolToObject(m, "alive", st.alive);
  cJSON_AddStringToObject(m, "model", st.model);
  cJSON_AddStringToObject(m, "revision", st.revision);
  cJSON_AddStringToObject(m, "imei", st.imei);
  cJSON_AddStringToObject(m, "iccid", st.iccid);
  cJSON_AddStringToObject(m, "sim", st.sim);
  cJSON_AddStringToObject(m, "own_number", st.own_number);
  cJSON_AddStringToObject(m, "operator", st.oper);
  cJSON_AddNumberToObject(m, "csq", st.csq);
  cJSON_AddNumberToObject(m, "rssi_dbm", st.csq >= 0 && st.csq <= 31 ? -113 + 2 * st.csq : 0);
  cJSON_AddStringToObject(m, "lte_reg", reg_name(st.reg_eps));
  cJSON_AddStringToObject(m, "cs_reg", reg_name(st.reg_cs));
  cJSON_AddStringToObject(m, "radio", st.cpsi);
  cJSON_AddStringToObject(m, "sms_storage", st.cpms);
  cJSON_AddNumberToObject(m, "sms_saved_since_boot", st.stored_count);
  cJSON_AddNumberToObject(m, "sms_notifications", st.cmti_count);
  xSemaphoreGive(lock);
}

void modem_summary(char *out, size_t cap) {
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(out, cap,
           "modem alive: %s (first answer after %lld ms)\nmodel: %s / %s\nsim: %s, operator: %s, csq: %d\n"
           "lte reg: %d (%s), cs reg: %d (%s)\ncpsi: %s\ncpms: %s\nimei/iccid/imsi/own: %s / %s / %s / %s\n"
           "urc: %u, +CMTI: %u, stored: %u, removed from SIM: %u\n",
           st.alive ? "yes" : "no", st.boot_wait_ms, st.model, st.revision, st.sim, st.oper, st.csq, st.reg_eps,
           reg_name(st.reg_eps), st.reg_cs, reg_name(st.reg_cs), st.cpsi, st.cpms, st.imei, st.iccid, st.imsi,
           st.own_number, st.urc_count, st.cmti_count, st.stored_count, st.sim_deleted);
  xSemaphoreGive(lock);
}

// ---------- sending ----------

// Waits for the "> " prompt of AT+CMGS; false on ERROR or timeout.
static bool wait_prompt(int timeout_ms) {
  int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
  char seen[64];
  size_t n = 0;
  while (esp_timer_get_time() < until) {
    uint8_t c;
    if (uart_read_bytes(UART, &c, 1, pdMS_TO_TICKS(20)) != 1) continue;
    if (c == '>') return true;
    if (n < sizeof(seen) - 1) seen[n++] = (char)c;
    seen[n] = 0;
    if (strstr(seen, "ERROR")) return false;
  }
  return false;
}

int modem_send_sms(const char *to, const char *text, char *err, size_t errcap) {
  static uint8_t ref;
  if (errcap) err[0] = 0;
  sms_submit_part_t *parts = malloc(sizeof(sms_submit_part_t) * SMS_SUBMIT_MAX_PARTS);
  if (!parts) {
    snprintf(err, errcap, "out of memory");
    return -1;
  }
  int n = sms_pdu_encode_submit(to, text, ++ref, parts, SMS_SUBMIT_MAX_PARTS);
  if (n < 0) {
    snprintf(err, errcap, n == -1 ? "invalid phone number" : n == -2 ? "message too long (max 8 parts)" : "invalid text");
    free(parts);
    return n;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  int sent = 0;
  if (!st.alive) {
    snprintf(err, errcap, "modem not answering");
    goto done;
  }
  cmd_quiet("AT+CMGF=0", 2000);
  for (int i = 0; i < n; i++) {
    char line[256], c[24];
    while (read_line(line, sizeof(line), 0)) handle_urc(line);
    snprintf(c, sizeof(c), "AT+CMGS=%d", parts[i].tpdu_len);
    devlog("AT> %s (part %d/%d)", c, i + 1, n);
    uart_write_bytes(UART, c, strlen(c));
    uart_write_bytes(UART, "\r", 1);
    if (!wait_prompt(10000)) {
      snprintf(err, errcap, "modem refused to send (no prompt) at part %d/%d", i + 1, n);
      uart_write_bytes(UART, "\x1B", 1);  // cancel
      goto done;
    }
    uart_write_bytes(UART, parts[i].hex, strlen(parts[i].hex));
    uart_write_bytes(UART, "\x1A", 1);
    bool ok = false, fin = false;
    int64_t until = esp_timer_get_time() + 60LL * 1000000;
    while (!fin && esp_timer_get_time() < until) {
      if (!read_line(line, sizeof(line), 100)) continue;
      if (looks_like_pdu(line)) continue;  // echo of the PDU
      log_line("AT<", line);
      if (!strncmp(line, "+CMGS:", 6)) ok = true;
      else {
        bool fok = false;
        if (is_final(line, &fok)) {
          if (!fok) {
            snprintf(err, errcap, "network rejected part %d/%d: %s", i + 1, n, line);
            ok = false;
          }
          fin = true;
        }
      }
    }
    if (!ok) {
      if (!err[0]) snprintf(err, errcap, "no confirmation from the network for part %d/%d", i + 1, n);
      goto done;
    }
    sent++;
  }
done:
  xSemaphoreGive(lock);
  free(parts);
  devlog("sms: sent %d of %d part(s) to %s", sent, n, to);
  return sent == n ? n : -1;
}
