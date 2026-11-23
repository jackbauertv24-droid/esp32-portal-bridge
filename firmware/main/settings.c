#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "devlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

typedef enum { T_STR, T_SECRET, T_INT, T_BOOL, T_ENUM } ctype_t;

typedef struct {
  const char *key;    // NVS key, max 15 chars
  const char *group;  // admin page section; "internal" is hidden
  const char *label;
  const char *help;
  ctype_t type;
  const char *def;
  int min, max;         // INT: value range; STR/SECRET: length range
  const char *options;  // ENUM: "a|b|c"; STR: format "ip", "mac", "digits", "url" or NULL
  bool reboot;          // takes effect after a restart
} def_t;

#define MOBILE_UA \
  "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Mobile Safari/537.36"

static const def_t DEFS[] = {
    // Hotspot
    {"ap_ssid", "Hotspot", "Hotspot name", "Wi-Fi name this board broadcasts.", T_STR, "", 1, 32, NULL, true},
    {"ap_pass", "Hotspot", "Hotspot password", "WPA2 password, 8 to 63 characters.", T_SECRET, "portalbridge", 8,
     63, NULL, true},
    {"ap_chan", "Hotspot", "Channel", "Used only when no upstream Wi-Fi is connected; otherwise the hotspot follows "
     "the upstream channel (one radio).", T_INT, "6", 1, 13, NULL, true},
    {"ap_hidden", "Hotspot", "Hide hotspot name", NULL, T_BOOL, "0", 0, 0, NULL, true},
    {"ap_maxcli", "Hotspot", "Max devices", NULL, T_INT, "8", 1, 10, NULL, true},
    {"ap_ip", "Hotspot", "Admin address", "Gateway address of the hotspot and of this admin page. If an upstream "
     "network uses the same range, the board moves to 10.42.0.1 automatically and logs it.", T_STR, "192.168.0.1",
     7, 15, "ip", true},
    // Admin
    {"adm_pass", "Admin", "Admin password", "User name is always admin. 8+ characters.", T_SECRET, "portalbridge", 8,
     64, NULL, false},
    // Phone identity
    {"ph_cc", "Phone number", "Country code", "Digits only, e.g. 852.", T_STR, "852", 1, 4, "digits", false},
    {"ph_local", "Phone number", "Local number", "The SIM's number without country code. Empty = read it from the "
     "SIM (not every SIM stores it).", T_STR, "", 0, 16, "digits", false},
    {"ph_fmt", "Phone number", "Format sent to login pages", "local = 91234567, plus_cc = +85291234567, cc = "
     "85291234567, 00cc = 0085291234567.", T_ENUM, "local", 0, 0, "local|plus_cc|cc|00cc", false},
    // Uplink
    {"up_mode", "Internet uplink", "Uplink mode", "Where the hotspot's internet comes from.", T_ENUM, "wifi_then_4g",
     0, 0, "wifi_then_4g|4g_then_wifi|wifi|4g", false},
    {"probe_url", "Internet uplink", "Internet check URL", "Plain-HTTP URL that answers 204 (or 200 with an empty "
     "body) when there is internet. Anything else means a login page or no internet.", T_STR,
     "http://connectivitycheck.gstatic.com/generate_204", 10, 200, "url", false},
    {"probe_s", "Internet uplink", "Check interval (s)", NULL, T_INT, "30", 10, 600, NULL, false},
    // 4G
    {"lte_en", "4G", "Use 4G data", "Needs the USB DIP switch set to modem <-> ESP32.", T_BOOL, "1", 0, 0, NULL,
     true},
    {"apn", "4G", "APN", "Empty = the modem's default.", T_STR, "", 0, 63, NULL, true},
    {"apn_user", "4G", "APN user", NULL, T_STR, "", 0, 63, NULL, true},
    {"apn_pass", "4G", "APN password", NULL, T_SECRET, "", 0, 63, NULL, true},
    {"sim_pin", "4G", "SIM PIN", "Only if the SIM asks for one.", T_SECRET, "", 0, 8, "digits", true},
    // Upstream Wi-Fi
    {"w1_ssid", "Upstream Wi-Fi", "Network 1 name", "Tried first. 2.4 GHz only.", T_STR, "", 0, 32, NULL, false},
    {"w1_pass", "Upstream Wi-Fi", "Network 1 password", "Empty for open networks.", T_SECRET, "", 0, 63, NULL, false},
    {"w2_ssid", "Upstream Wi-Fi", "Network 2 name", NULL, T_STR, "", 0, 32, NULL, false},
    {"w2_pass", "Upstream Wi-Fi", "Network 2 password", NULL, T_SECRET, "", 0, 63, NULL, false},
    {"w3_ssid", "Upstream Wi-Fi", "Network 3 name", NULL, T_STR, "", 0, 32, NULL, false},
    {"w3_pass", "Upstream Wi-Fi", "Network 3 password", NULL, T_SECRET, "", 0, 63, NULL, false},
    {"sta_mac", "Upstream Wi-Fi", "MAC address override", "Empty = the board's own. Format aa:bb:cc:dd:ee:ff.",
     T_STR, "", 0, 17, "mac", true},
    // Portal
    {"po_mode", "Login page", "Login mode", "auto = log in by itself; assisted = only when you log in through the "
     "board's mirror page; off = do nothing.", T_ENUM, "auto", 0, 0, "auto|assisted|off", false},
    {"po_strat", "Login page", "Auto-login method", "recipe_then_form = use the saved recipe if there is one, else "
     "fill the form automatically.", T_ENUM, "recipe_then_form", 0, 0, "recipe_then_form|form|recipe", false},
    {"po_tls", "Login page", "Verify login page HTTPS certificate", "Turn off only if the login page has a broken "
     "certificate.", T_BOOL, "1", 0, 0, NULL, false},
    {"po_ua", "Login page", "Browser identity (User-Agent)", NULL, T_STR, MOBILE_UA, 1, 200, NULL, false},
    {"po_retry_s", "Login page", "Wait between automatic attempts (s)", NULL, T_INT, "300", 30, 86400, NULL, false},
    {"otp_wait_s", "Login page", "How long to wait for the SMS code (s)", NULL, T_INT, "180", 30, 600, NULL, false},
    {"otp_sender", "Login page", "Code SMS sender contains", "Empty = any sender.", T_STR, "", 0, 40, NULL, false},
    {"otp_kw", "Login page", "Code SMS text contains", "Empty = any text.", T_STR, "", 0, 40, NULL, false},
    {"otp_min", "Login page", "Code length min", NULL, T_INT, "4", 3, 10, NULL, false},
    {"otp_max", "Login page", "Code length max", NULL, T_INT, "8", 3, 12, NULL, false},
    // SMS
    {"sms_poll_s", "SMS", "Check SIM for new SMS every (s)", NULL, T_INT, "15", 5, 600, NULL, false},
    {"sms_del", "SMS", "Remove from SIM after saving on the board", "Keeps the SIM from filling up. Messages are "
     "only removed after they are safely written to the board's flash.", T_BOOL, "1", 0, 0, NULL, false},
    // Forwarding
    {"tg_en", "Forwarding", "Telegram: enabled", NULL, T_BOOL, "0", 0, 0, NULL, false},
    {"tg_token", "Forwarding", "Telegram: bot token", "From @BotFather.", T_SECRET, "", 0, 120, NULL, false},
    {"tg_chat", "Forwarding", "Telegram: chat id", "Use the Tools page 'Find my Telegram chat id' after messaging "
     "your bot.", T_STR, "", 0, 32, NULL, false},
    {"wh_en", "Forwarding", "Webhook: enabled", NULL, T_BOOL, "0", 0, 0, NULL, false},
    {"wh_url", "Forwarding", "Webhook: URL", "HTTPS recommended. JSON is POSTed.", T_STR, "", 0, 250, NULL, false},
    {"wh_hdr", "Forwarding", "Webhook: auth header name", NULL, T_STR, "Authorization", 0, 60, NULL, false},
    {"wh_secret", "Forwarding", "Webhook: auth header value", NULL, T_SECRET, "", 0, 250, NULL, false},
    {"mq_en", "Forwarding", "MQTT: enabled", NULL, T_BOOL, "0", 0, 0, NULL, false},
    {"mq_uri", "Forwarding", "MQTT: broker URI", "e.g. mqtts://xxxx.s1.eu.hivemq.cloud:8883", T_STR, "", 0, 200, NULL,
     false},
    {"mq_user", "Forwarding", "MQTT: user", NULL, T_STR, "", 0, 64, NULL, false},
    {"mq_pass", "Forwarding", "MQTT: password", NULL, T_SECRET, "", 0, 64, NULL, false},
    {"mq_topic", "Forwarding", "MQTT: topic", NULL, T_STR, "portal-bridge/sms", 1, 120, NULL, false},
    {"fw_otp", "Forwarding", "Also forward login-code SMS", NULL, T_BOOL, "1", 0, 0, NULL, false},
    {"fw_events", "Forwarding", "Send device events", "Login results, uplink changes, restarts.", T_BOOL, "1", 0, 0,
     NULL, false},
    // Reports
    {"gh_repo", "Reports", "GitHub repository for reports", "owner/name", T_STR,
     "jackbauertv24-droid/esp32-portal-bridge", 0, 100, NULL, false},
    {"gh_token", "Reports", "GitHub token", "Fine-grained token with Issues: read and write on that repository "
     "only.", T_SECRET, "", 0, 200, NULL, false},
    // Device
    {"led_bri", "Device", "LED brightness", "0 = off.", T_INT, "24", 0, 255, NULL, false},
    {"tz", "Device", "Time zone (POSIX)", "HKT-8 for Hong Kong.", T_STR, "HKT-8", 1, 40, NULL, true},
    {"ntp", "Device", "Time server", NULL, T_STR, "pool.ntp.org", 1, 64, NULL, true},
    // Internal
    {"setup_done", "internal", "", NULL, T_BOOL, "0", 0, 0, NULL, false},
};
#define N_DEFS (sizeof(DEFS) / sizeof(DEFS[0]))

static char *vals[N_DEFS];
static char *runtime_def[N_DEFS];
static SemaphoreHandle_t mtx;

static int find(const char *key) {
  for (size_t i = 0; i < N_DEFS; i++)
    if (!strcmp(DEFS[i].key, key)) return (int)i;
  return -1;
}

static const char *default_of(int i) { return runtime_def[i] ? runtime_def[i] : DEFS[i].def; }

static void put(int i, const char *v) {
  free(vals[i]);
  vals[i] = strdup(v);
}

void cfg_init(void) {
  mtx = xSemaphoreCreateMutex();
  nvs_handle_t h;
  bool open = nvs_open("cfg", NVS_READONLY, &h) == ESP_OK;
  for (size_t i = 0; i < N_DEFS; i++) {
    char buf[300];
    size_t len = sizeof(buf);
    if (open && nvs_get_str(h, DEFS[i].key, buf, &len) == ESP_OK) put((int)i, buf);
    else put((int)i, DEFS[i].def);
  }
  if (open) nvs_close(h);
}

void cfg_set_runtime_default(const char *key, const char *value) {
  int i = find(key);
  if (i < 0) return;
  free(runtime_def[i]);
  runtime_def[i] = strdup(value);
  if (!vals[i][0] || !strcmp(vals[i], DEFS[i].def)) put(i, value);
}

void cfg_get(const char *key, char *out, size_t cap) {
  int i = find(key);
  if (!cap) return;
  out[0] = 0;
  if (i < 0) {
    devlog("cfg: unknown key %s", key);
    return;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(out, cap, "%s", vals[i]);
  xSemaphoreGive(mtx);
}

int cfg_int(const char *key) {
  char b[32];
  cfg_get(key, b, sizeof(b));
  return atoi(b);
}

bool cfg_bool(const char *key) {
  char b[8];
  cfg_get(key, b, sizeof(b));
  return b[0] == '1';
}

bool cfg_is(const char *key, const char *value) {
  char b[64];
  cfg_get(key, b, sizeof(b));
  return !strcmp(b, value);
}

static bool valid_ip(const char *s) {
  int a, b, c, d;
  char tail;
  return sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 && a >= 0 && a < 256 && b >= 0 && b < 256 &&
         c >= 0 && c < 256 && d > 0 && d < 255;
}

static bool valid_mac(const char *s) {
  unsigned m[6];
  char tail;
  return sscanf(s, "%x:%x:%x:%x:%x:%x%c", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &tail) == 6;
}

// Normalises (booleans to "1"/"0") into `norm` and validates.
static esp_err_t validate(int i, const char *v, char *norm, size_t ncap, char *err, size_t ecap) {
  const def_t *d = &DEFS[i];
  snprintf(norm, ncap, "%s", v);
  switch (d->type) {
    case T_BOOL:
      if (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on")) snprintf(norm, ncap, "1");
      else if (!strcmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "off") || !v[0]) snprintf(norm, ncap, "0");
      else goto bad;
      return ESP_OK;
    case T_INT: {
      char *end;
      long n = strtol(v, &end, 10);
      if (!v[0] || *end || n < d->min || n > d->max) {
        snprintf(err, ecap, "%s: whole number from %d to %d", d->label, d->min, d->max);
        return ESP_ERR_INVALID_ARG;
      }
      return ESP_OK;
    }
    case T_ENUM: {
      size_t vl = strlen(v);
      for (const char *o = d->options; *o;) {
        size_t ol = strcspn(o, "|");
        if (ol == vl && !strncmp(o, v, vl)) return ESP_OK;
        o += ol + (o[ol] ? 1 : 0);
      }
      snprintf(err, ecap, "%s: one of %s", d->label, d->options);
      return ESP_ERR_INVALID_ARG;
    }
    case T_STR:
    case T_SECRET: {
      int len = (int)strlen(v);
      if (len == 0 && d->min > 0) goto bad;
      if (len && (len < d->min || len > d->max)) {
        snprintf(err, ecap, "%s: %d to %d characters", d->label, d->min, d->max);
        return ESP_ERR_INVALID_ARG;
      }
      if (len && d->options) {
        if (!strcmp(d->options, "ip") && !valid_ip(v)) goto bad;
        if (!strcmp(d->options, "mac") && !valid_mac(v)) goto bad;
        if (!strcmp(d->options, "url") && strncmp(v, "http://", 7) && strncmp(v, "https://", 8)) goto bad;
        if (!strcmp(d->options, "digits"))
          for (const char *p = v; *p; p++)
            if (!isdigit((unsigned char)*p)) goto bad;
      }
      return ESP_OK;
    }
  }
bad:
  snprintf(err, ecap, "%s: invalid value", d->label);
  return ESP_ERR_INVALID_ARG;
}

esp_err_t cfg_set(const char *key, const char *value, char *err, size_t errcap) {
  int i = find(key);
  if (i < 0) {
    snprintf(err, errcap, "unknown setting %s", key);
    return ESP_ERR_NOT_FOUND;
  }
  if (DEFS[i].type == T_SECRET && !strcmp(value, CFG_SECRET_MASK)) return ESP_OK;
  char norm[300];
  esp_err_t e = validate(i, value, norm, sizeof(norm), err, errcap);
  if (e != ESP_OK) return e;
  xSemaphoreTake(mtx, portMAX_DELAY);
  put(i, norm);
  xSemaphoreGive(mtx);
  return ESP_OK;
}

esp_err_t cfg_commit(void) {
  nvs_handle_t h;
  esp_err_t e = nvs_open("cfg", NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (size_t i = 0; i < N_DEFS && e == ESP_OK; i++) e = nvs_set_str(h, DEFS[i].key, vals[i]);
  xSemaphoreGive(mtx);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}

void cfg_reset_all(void) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (size_t i = 0; i < N_DEFS; i++) put((int)i, default_of((int)i));
  xSemaphoreGive(mtx);
}

static const char *type_name(ctype_t t) {
  switch (t) {
    case T_SECRET: return "secret";
    case T_INT: return "int";
    case T_BOOL: return "bool";
    case T_ENUM: return "enum";
    default: return "str";
  }
}

cJSON *cfg_schema_json(void) {
  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; i < N_DEFS; i++) {
    const def_t *d = &DEFS[i];
    if (!strcmp(d->group, "internal")) continue;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "key", d->key);
    cJSON_AddStringToObject(o, "group", d->group);
    cJSON_AddStringToObject(o, "label", d->label);
    if (d->help) cJSON_AddStringToObject(o, "help", d->help);
    cJSON_AddStringToObject(o, "type", type_name(d->type));
    cJSON_AddStringToObject(o, "def", d->type == T_SECRET ? "" : default_of((int)i));
    cJSON_AddNumberToObject(o, "min", d->min);
    cJSON_AddNumberToObject(o, "max", d->max);
    if (d->options) cJSON_AddStringToObject(o, "options", d->options);
    cJSON_AddBoolToObject(o, "reboot", d->reboot);
    cJSON_AddItemToArray(arr, o);
  }
  return arr;
}

cJSON *cfg_values_json(bool include_secrets) {
  cJSON *o = cJSON_CreateObject();
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (size_t i = 0; i < N_DEFS; i++) {
    const char *v = vals[i];
    if (DEFS[i].type == T_SECRET && !include_secrets) v = v[0] ? CFG_SECRET_MASK : "";
    cJSON_AddStringToObject(o, DEFS[i].key, v);
  }
  xSemaphoreGive(mtx);
  return o;
}

esp_err_t cfg_apply_json(const cJSON *obj, bool *reboot, char *err, size_t errcap) {
  if (reboot) *reboot = false;
  // Validate everything first so a bad value leaves nothing half-applied.
  const cJSON *it;
  cJSON_ArrayForEach(it, obj) {
    int i = find(it->string);
    if (i < 0) continue;  // ignore unknown keys (older/newer backups)
    char norm[300];
    const char *v = cJSON_IsString(it) ? it->valuestring : cJSON_IsTrue(it) ? "1" : cJSON_IsFalse(it) ? "0" : NULL;
    char num[24];
    if (!v && cJSON_IsNumber(it)) {
      snprintf(num, sizeof(num), "%d", it->valueint);
      v = num;
    }
    if (!v) continue;
    if (DEFS[i].type == T_SECRET && !strcmp(v, CFG_SECRET_MASK)) continue;
    esp_err_t e = validate(i, v, norm, sizeof(norm), err, errcap);
    if (e != ESP_OK) return e;
  }
  cJSON_ArrayForEach(it, obj) {
    int i = find(it->string);
    if (i < 0) continue;
    const char *v = cJSON_IsString(it) ? it->valuestring : cJSON_IsTrue(it) ? "1" : cJSON_IsFalse(it) ? "0" : NULL;
    char num[24];
    if (!v && cJSON_IsNumber(it)) {
      snprintf(num, sizeof(num), "%d", it->valueint);
      v = num;
    }
    if (!v || (DEFS[i].type == T_SECRET && !strcmp(v, CFG_SECRET_MASK))) continue;
    char norm[300], e2[8];
    validate(i, v, norm, sizeof(norm), e2, sizeof(e2));
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool changed = strcmp(vals[i], norm) != 0;
    if (changed) put(i, norm);
    xSemaphoreGive(mtx);
    if (changed && DEFS[i].reboot && reboot) *reboot = true;
  }
  return cfg_commit();
}
