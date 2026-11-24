#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "build_info.h"
#include "cJSON.h"
#include "devlog.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "forward.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mirror.h"
#include "modem_at.h"
#include "netmgr.h"
#include "portal.h"
#include "recorder.h"
#include "report.h"
#include "settings.h"
#include "smsstore.h"
#include "web.h"

extern const char index_html_start[] asm("_binary_index_html_start");

static httpd_handle_t server;

// ---------- helpers ----------

static esp_err_t auth(httpd_req_t *req) {
  char hdr[200];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK && !strncmp(hdr, "Basic ", 6)) {
    unsigned char dec[160];
    size_t n = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
      dec[n] = 0;
      char want[100], pass[70];
      cfg_get("adm_pass", pass, sizeof(pass));
      snprintf(want, sizeof(want), "admin:%s", pass);
      if (!strcmp((char *)dec, want)) return ESP_OK;
    }
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Portal Bridge\"");
  httpd_resp_sendstr(req, "Login required (user admin)");
  return ESP_FAIL;
}

// Everything except setup needs the first-run setup done.
static esp_err_t gate(httpd_req_t *req) {
  if (auth(req) != ESP_OK) return ESP_FAIL;
  if (cfg_bool("setup_done")) return ESP_OK;
  httpd_resp_set_status(req, "403 Forbidden");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"setup_required\":true}");
  return ESP_FAIL;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *o) {
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t e = httpd_resp_sendstr(req, s ? s : "null");
  free(s);
  return e;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", ok);
  cJSON_AddStringToObject(o, "message", msg);
  return send_json(req, o);
}

// Reads the request body (max `max` bytes) into a malloc'd string.
static char *read_body(httpd_req_t *req, size_t max) {
  if (req->content_len > max) return NULL;
  char *b = malloc(req->content_len + 1);
  size_t got = 0;
  while (b && got < req->content_len) {
    int n = httpd_req_recv(req, b + got, req->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) {
      free(b);
      return NULL;
    }
    got += (size_t)n;
  }
  if (b) b[got] = 0;
  return b;
}

static void query(httpd_req_t *req, const char *key, char *out, size_t cap) {
  char q[256];
  out[0] = 0;
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, key, out, cap);
}

static esp_err_t send_text_chunked(httpd_req_t *req, const char *text, const char *filename) {
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char disp[120];
  if (filename) {
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
  }
  size_t len = strlen(text), off = 0;
  while (off < len) {
    size_t n = len - off > 4096 ? 4096 : len - off;
    if (httpd_resp_send_chunk(req, text + off, (ssize_t)n) != ESP_OK) return ESP_FAIL;
    off += n;
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

static void restart_later(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
}

static void schedule_restart(void) { xTaskCreate(restart_later, "restart", 2048, NULL, 1, NULL); }

// ---------- pages ----------

static esp_err_t h_index(httpd_req_t *req) {
  if (auth(req) != ESP_OK) return ESP_OK;
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req) {
  if (auth(req) != ESP_OK) return ESP_OK;
  return send_json(req, status_json());
}

static esp_err_t h_schema(httpd_req_t *req) {
  if (auth(req) != ESP_OK) return ESP_OK;
  cJSON *o = cJSON_CreateObject();
  cJSON_AddItemToObject(o, "schema", cfg_schema_json());
  cJSON_AddItemToObject(o, "values", cfg_values_json(false));
  return send_json(req, o);
}

// The upstream Wi-Fi list as one string, to notice changes.
static void wifi_list(char *out, size_t cap) {
  static const char *const k[] = {"w1_ssid", "w1_pass", "w2_ssid", "w2_pass", "w3_ssid", "w3_pass"};
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < 6 && o + 70 < cap; i++) {
    cfg_get(k[i], out + o, cap - o);
    o = strlen(out);
    out[o++] = 0x1f;
    out[o] = 0;
  }
}

static esp_err_t h_settings_post(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *b = read_body(req, 16384);
  cJSON *o = b ? cJSON_Parse(b) : NULL;
  free(b);
  if (!cJSON_IsObject(o)) {
    cJSON_Delete(o);
    return send_result(req, false, "invalid JSON");
  }
  bool reboot = false;
  char err[160] = "", before[400], after[400];
  wifi_list(before, sizeof(before));
  esp_err_t e = cfg_apply_json(o, &reboot, err, sizeof(err));
  cJSON_Delete(o);
  if (e != ESP_OK) return send_result(req, false, err[0] ? err : esp_err_to_name(e));
  wifi_list(after, sizeof(after));
  if (strcmp(before, after)) netmgr_reconnect();
  forward_kick();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddBoolToObject(r, "reboot", reboot);
  cJSON_AddStringToObject(r, "message", reboot ? "Saved. Some changes apply after a restart." : "Saved.");
  return send_json(req, r);
}

static esp_err_t h_setup(httpd_req_t *req) {
  if (auth(req) != ESP_OK) return ESP_OK;
  char *b = read_body(req, 2048);
  cJSON *o = b ? cJSON_Parse(b) : NULL;
  free(b);
  if (!cJSON_IsObject(o)) {
    cJSON_Delete(o);
    return send_result(req, false, "invalid JSON");
  }
  cJSON_AddStringToObject(o, "setup_done", "1");
  bool reboot;
  char err[160] = "";
  esp_err_t e = cfg_apply_json(o, &reboot, err, sizeof(err));
  cJSON_Delete(o);
  if (e != ESP_OK) return send_result(req, false, err);
  devlog("setup: first-run setup saved, restarting");
  send_result(req, true, "Saved. Restarting; reconnect to the new hotspot with the new password.");
  schedule_restart();
  return ESP_OK;
}

static esp_err_t h_export(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  cJSON *v = cfg_values_json(true);
  char *s = cJSON_Print(v);
  cJSON_Delete(v);
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"portal-bridge-settings.json\"");
  httpd_resp_set_type(req, "application/json");
  esp_err_t e = httpd_resp_sendstr(req, s);
  free(s);
  return e;
}

static esp_err_t h_reset(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  cfg_reset_all();
  cfg_commit();
  send_result(req, true, "All settings reset to defaults. Restarting.");
  schedule_restart();
  return ESP_OK;
}

static esp_err_t h_sms(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char l[8];
  query(req, "limit", l, sizeof(l));
  int limit = atoi(l);
  if (limit <= 0 || limit > 1000) limit = 300;
  char box[8];
  query(req, "box", box, sizeof(box));
  return send_json(req, !strcmp(box, "out") ? smsstore_list_sent_json(limit) : smsstore_list_json(limit));
}

static esp_err_t h_sms_send(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *b = read_body(req, 8192);
  cJSON *o = b ? cJSON_Parse(b) : NULL;
  free(b);
  const char *to = o ? cJSON_GetStringValue(cJSON_GetObjectItem(o, "to")) : NULL;
  const char *text = o ? cJSON_GetStringValue(cJSON_GetObjectItem(o, "text")) : NULL;
  if (!to || !to[0] || !text || !text[0]) {
    cJSON_Delete(o);
    return send_result(req, false, "Enter a number and a message.");
  }
  char err[160] = "";
  int parts = modem_send_sms(to, text, err, sizeof(err));
  smsstore_add_sent(to, text, parts > 0 ? parts : 0, parts > 0, err);
  cJSON_Delete(o);
  char msg[200];
  if (parts > 0) snprintf(msg, sizeof(msg), "Sent (%d part%s).", parts, parts > 1 ? "s" : "");
  else snprintf(msg, sizeof(msg), "Not sent: %s", err);
  return send_result(req, parts > 0, msg);
}

static esp_err_t h_sms_delete(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *b = read_body(req, 16384);
  cJSON *o = b ? cJSON_Parse(b) : NULL;
  free(b);
  const char *box = o ? cJSON_GetStringValue(cJSON_GetObjectItem(o, "box")) : NULL;
  cJSON *ids = o ? cJSON_GetObjectItem(o, "ids") : NULL;
  int n = cJSON_GetArraySize(ids);
  if (!box || n <= 0) {
    cJSON_Delete(o);
    return send_result(req, false, "nothing selected");
  }
  uint32_t *v = calloc((size_t)n, sizeof(uint32_t));
  for (int i = 0; i < n; i++) v[i] = (uint32_t)cJSON_GetNumberValue(cJSON_GetArrayItem(ids, i));
  int removed = smsstore_delete(box, v, n);
  free(v);
  cJSON_Delete(o);
  char msg[60];
  snprintf(msg, sizeof(msg), "Deleted %d message%s.", removed, removed == 1 ? "" : "s");
  return send_result(req, removed > 0, msg);
}

static esp_err_t h_sms_seen(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char id[16];
  query(req, "id", id, sizeof(id));
  smsstore_set_seen((uint32_t)strtoul(id, NULL, 10));
  return send_result(req, true, "ok");
}

static esp_err_t h_sms_export(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  cJSON *o = cJSON_CreateObject();
  cJSON_AddItemToObject(o, "inbox", smsstore_list_json(5000));
  cJSON_AddItemToObject(o, "sent", smsstore_list_sent_json(5000));
  char *s = cJSON_Print(o);
  cJSON_Delete(o);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"portal-bridge-sms.json\"");
  esp_err_t e = send_text_chunked(req, s ? s : "{}", NULL);
  free(s);
  return e;
}

static esp_err_t h_at(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *b = read_body(req, 512);
  cJSON *o = b ? cJSON_Parse(b) : NULL;
  free(b);
  const char *cmd = o ? cJSON_GetStringValue(cJSON_GetObjectItem(o, "cmd")) : NULL;
  if (!cmd || !cmd[0]) {
    cJSON_Delete(o);
    return send_result(req, false, "empty command");
  }
  bool ok = false;
  char *r = modem_cmd(cmd, 20000, &ok);
  cJSON_Delete(o);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", ok);
  cJSON_AddStringToObject(out, "reply", r ? r : "");
  free(r);
  return send_json(req, out);
}

static esp_err_t h_log(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *s = devlog_dump(NULL);
  esp_err_t e = send_text_chunked(req, s ? s : "", NULL);
  free(s);
  return e;
}

static esp_err_t h_report(httpd_req_t *req) {
  if (auth(req) != ESP_OK) return ESP_OK;
  char *s = report_build();
  esp_err_t e = send_text_chunked(req, s ? s : "", "portal-bridge-report.txt");
  free(s);
  return e;
}

static void report_title(char *out, size_t cap) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  char ts[24];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);
  snprintf(out, cap, "Device report %s (%s)", ts, FW_VERSION);
}

static esp_err_t h_report_github(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char inc[4];
  query(req, "recording", inc, sizeof(inc));
  char *rep = report_build();
  char name[48] = "";
  char *rec = inc[0] == '1' ? report_latest_recording(name, sizeof(name)) : NULL;
  char title[96], result[300];
  report_title(title, sizeof(title));
  bool ok = github_create_issue(title, rep ? rep : "", rec && rec[0] ? rec : NULL, result, sizeof(result));
  free(rep);
  free(rec);
  return send_result(req, ok, result);
}

static esp_err_t h_report_telegram(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *rep = report_build();
  char result[200];
  bool ok = telegram_send_document("portal-bridge-report.txt", rep ? rep : "", rep ? strlen(rep) : 0, result,
                                   sizeof(result));
  free(rep);
  return send_result(req, ok, result);
}

static esp_err_t h_recordings(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  return send_json(req, rec_list_json());
}

static esp_err_t h_recording(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char name[64], path[96];
  query(req, "name", name, sizeof(name));
  if (!rec_path(name, path, sizeof(path))) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such recording");
  FILE *f = fopen(path, "r");
  if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such recording");
  httpd_resp_set_type(req, "application/x-ndjson");
  char disp[100];
  snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
  httpd_resp_set_hdr(req, "Content-Disposition", disp);
  char *buf = malloc(4096);
  size_t n;
  while ((n = fread(buf, 1, 4096, f)) > 0)
    if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
  free(buf);
  fclose(f);
  return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_recording_delete(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char name[64];
  query(req, "name", name, sizeof(name));
  return send_result(req, rec_delete(name) == 0, "deleted");
}

static esp_err_t h_recipe_get(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *r = recipe_load();
  httpd_resp_set_type(req, "application/json");
  esp_err_t e = httpd_resp_sendstr(req, r ? r : "");
  free(r);
  return e;
}

static esp_err_t h_recipe_post(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *b = read_body(req, 60000);
  char err[120] = "";
  bool ok = b && recipe_save(b, err, sizeof(err));
  free(b);
  return send_result(req, ok, ok ? "Recipe saved." : err);
}

static esp_err_t h_recipe_delete(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  return send_result(req, recipe_delete(), "Recipe deleted.");
}

static esp_err_t h_recipe_from(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char name[64], err[160] = "";
  query(req, "name", name, sizeof(name));
  char *r = recipe_from_recording(name, err, sizeof(err));
  if (!r) return send_result(req, false, err);
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", true);
  cJSON_AddStringToObject(o, "recipe", r);
  free(r);
  return send_json(req, o);
}

static esp_err_t h_login_now(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  portal_trigger();
  return send_result(req, true, "Login attempt started. Watch the status and events.");
}

static esp_err_t h_forward_test(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char ch[4], result[160];
  query(req, "ch", ch, sizeof(ch));
  bool ok = forward_test(ch, result, sizeof(result));
  return send_result(req, ok, result);
}

static esp_err_t h_tg_chats(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char *out = malloc(4096);
  telegram_find_chats(out, 4096);
  httpd_resp_set_type(req, "application/json");
  esp_err_t e = httpd_resp_sendstr(req, out);
  free(out);
  return e;
}

static esp_err_t h_scan(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  wifi_scan_config_t sc = {.show_hidden = false};
  cJSON *arr = cJSON_CreateArray();
  if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
    uint16_t n = 30;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
      for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(o, "channel", recs[i].primary);
        cJSON_AddBoolToObject(o, "open", recs[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
      }
    }
    free(recs);
  }
  return send_json(req, arr);
}

static esp_err_t h_update(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  esp_ota_handle_t h = 0;
  if (!part || esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
    return send_result(req, false, "cannot start update");
  char *buf = malloc(4096);
  size_t left = req->content_len, total = 0;
  bool ok = true, first = true;
  const char *why = "";
  while (left > 0) {
    int n = httpd_req_recv(req, buf, left > 4096 ? 4096 : left);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) {
      ok = false;
      why = "upload interrupted";
      break;
    }
    if (first && (unsigned char)buf[0] != 0xE9) {
      ok = false;
      why = "not an app image: upload the -ota.bin file, not -full.bin";
      break;
    }
    first = false;
    if (esp_ota_write(h, buf, (size_t)n) != ESP_OK) {
      ok = false;
      why = "flash write failed";
      break;
    }
    left -= (size_t)n;
    total += (size_t)n;
  }
  free(buf);
  if (ok && esp_ota_end(h) != ESP_OK) {
    ok = false;
    why = "image check failed (corrupt or wrong file)";
  } else if (!ok) {
    esp_ota_abort(h);
  }
  if (ok && esp_ota_set_boot_partition(part) != ESP_OK) {
    ok = false;
    why = "could not select the new firmware";
  }
  devlog("ota: %s (%u bytes) %s", ok ? "installed" : "failed", (unsigned)total, why);
  send_result(req, ok, ok ? "Installed. Restarting; reconnect in about 30 seconds." : why);
  if (ok) schedule_restart();
  return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req) {
  if (auth(req) != ESP_OK) return ESP_OK;
  send_result(req, true, "Restarting.");
  schedule_restart();
  return ESP_OK;
}

static esp_err_t h_wifi_reconnect(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  netmgr_reconnect();
  return send_result(req, true, "Reconnecting to the upstream Wi-Fi list.");
}

// Phones probing for internet through the board get a fast answer only from
// upstream; anything unknown goes to the admin app.
static esp_err_t h_any(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_send(req, NULL, 0);
}

void web_start(void) {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.stack_size = 24576;
  c.max_uri_handlers = 48;
  c.max_open_sockets = 7;
  c.lru_purge_enable = true;
  c.recv_wait_timeout = 30;
  c.send_wait_timeout = 30;
  c.uri_match_fn = httpd_uri_match_wildcard;
  c.core_id = 1;
  if (httpd_start(&server, &c) != ESP_OK) {
    devlog("web: server failed to start");
    return;
  }
  struct {
    const char *uri;
    httpd_method_t m;
    esp_err_t (*h)(httpd_req_t *);
  } routes[] = {
      {"/", HTTP_GET, h_index},
      {"/api/status", HTTP_GET, h_status},
      {"/api/settings", HTTP_GET, h_schema},
      {"/api/settings", HTTP_POST, h_settings_post},
      {"/api/setup", HTTP_POST, h_setup},
      {"/api/settings/export", HTTP_GET, h_export},
      {"/api/settings/reset", HTTP_POST, h_reset},
      {"/api/sms", HTTP_GET, h_sms},
      {"/api/sms/send", HTTP_POST, h_sms_send},
      {"/api/sms/delete", HTTP_POST, h_sms_delete},
      {"/api/sms/seen", HTTP_POST, h_sms_seen},
      {"/api/sms/export", HTTP_GET, h_sms_export},
      {"/api/at", HTTP_POST, h_at},
      {"/api/log", HTTP_GET, h_log},
      {"/api/report", HTTP_GET, h_report},
      {"/api/report/github", HTTP_POST, h_report_github},
      {"/api/report/telegram", HTTP_POST, h_report_telegram},
      {"/api/recordings", HTTP_GET, h_recordings},
      {"/api/recording", HTTP_GET, h_recording},
      {"/api/recording/delete", HTTP_POST, h_recording_delete},
      {"/api/recipe", HTTP_GET, h_recipe_get},
      {"/api/recipe", HTTP_POST, h_recipe_post},
      {"/api/recipe/delete", HTTP_POST, h_recipe_delete},
      {"/api/recipe/from", HTTP_POST, h_recipe_from},
      {"/api/portal/login", HTTP_POST, h_login_now},
      {"/api/forward/test", HTTP_POST, h_forward_test},
      {"/api/telegram/chats", HTTP_GET, h_tg_chats},
      {"/api/wifi/scan", HTTP_GET, h_scan},
      {"/api/wifi/reconnect", HTTP_POST, h_wifi_reconnect},
      {"/api/update", HTTP_POST, h_update},
      {"/api/reboot", HTTP_POST, h_reboot},
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    httpd_uri_t u = {.uri = routes[i].uri, .method = routes[i].m, .handler = routes[i].h};
    httpd_register_uri_handler(server, &u);
  }
  mirror_register(server, auth);
  httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = h_any};
  httpd_register_uri_handler(server, &any);
  char ip[16];
  netmgr_ap_ip(ip, sizeof(ip));
  devlog("web: admin app on http://%s/", ip);
}
