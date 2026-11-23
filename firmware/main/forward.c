#include "forward.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devlog.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "netmgr.h"
#include "nvs.h"
#include "settings.h"
#include "smsstore.h"

typedef struct {
  const char *key;  // "tg", "wh", "mq"
  const char *en;   // settings key of the enable flag
  uint32_t last;    // last delivered SMS id
  int64_t next_try_us;
  int backoff_s;
  char last_error[120];
  unsigned sent;
} chan_t;

static chan_t chans[3] = {{"tg", "tg_en"}, {"wh", "wh_en"}, {"mq", "mq_en"}};
static TaskHandle_t task;

// ---------- HTTP ----------

typedef struct {
  char *buf;
  size_t len, cap;
} rbuf_t;

// Sends a request with up to 3 extra headers (name/value pairs, NULL-terminated).
// Returns the HTTP status (or -1) and the start of the response in `resp`.
static int http_send(const char *method, const char *url, const char *ct, const char *body, size_t len,
                     const char *const *hdrs, char *resp, size_t rcap) {
  esp_http_client_config_t cfg = {
      .url = url,
      .method = !strcmp(method, "GET") ? HTTP_METHOD_GET : HTTP_METHOD_POST,
      .timeout_ms = 20000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 2048,
      .buffer_size_tx = 2048,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return -1;
  if (ct) esp_http_client_set_header(c, "Content-Type", ct);
  for (int i = 0; hdrs && hdrs[i] && hdrs[i + 1]; i += 2) esp_http_client_set_header(c, hdrs[i], hdrs[i + 1]);
  int status = -1;
  if (resp && rcap) resp[0] = 0;
  esp_err_t e = esp_http_client_open(c, (int)len);
  if (e == ESP_OK && len) e = esp_http_client_write(c, body, (int)len) == (int)len ? ESP_OK : ESP_FAIL;
  if (e == ESP_OK) {
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
    size_t got = 0;
    while (resp && got + 1 < rcap) {
      int n = esp_http_client_read(c, resp + got, (int)(rcap - got - 1));
      if (n <= 0) break;
      got += (size_t)n;
    }
    if (resp && rcap) resp[got] = 0;
  } else if (resp) {
    snprintf(resp, rcap, "%s", esp_err_to_name(e));
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return status;
}

// ---------- channels ----------

static bool send_telegram(const char *text, char *err, size_t ecap) {
  char token[128], chat[40];
  cfg_get("tg_token", token, sizeof(token));
  cfg_get("tg_chat", chat, sizeof(chat));
  if (!token[0] || !chat[0]) {
    snprintf(err, ecap, "bot token or chat id missing");
    return false;
  }
  char url[200];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "chat_id", chat);
  char *t = strndup(text, 4000);
  cJSON_AddStringToObject(o, "text", t ? t : "");
  free(t);
  cJSON_AddBoolToObject(o, "disable_web_page_preview", true);
  char *body = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  char resp[300];
  int st = http_send("POST", url, "application/json", body, strlen(body), NULL, resp, sizeof(resp));
  free(body);
  if (st == 200) return true;
  snprintf(err, ecap, "Telegram HTTP %d %.90s", st, resp);
  return false;
}

static bool send_webhook(const char *json, char *err, size_t ecap) {
  char url[256], hn[64], hv[256];
  cfg_get("wh_url", url, sizeof(url));
  cfg_get("wh_hdr", hn, sizeof(hn));
  cfg_get("wh_secret", hv, sizeof(hv));
  if (!url[0]) {
    snprintf(err, ecap, "webhook URL missing");
    return false;
  }
  const char *hdrs[] = {hn, hv, NULL};
  char resp[160];
  int st = http_send("POST", url, "application/json", json, strlen(json), (hn[0] && hv[0]) ? hdrs : NULL, resp,
                     sizeof(resp));
  if (st >= 200 && st < 300) return true;
  snprintf(err, ecap, "webhook HTTP %d %.90s", st, resp);
  return false;
}

static EventGroupHandle_t mq_ev;
#define MQ_CONNECTED BIT0
#define MQ_PUBLISHED BIT1
#define MQ_FAILED BIT2
static int mq_wait_id;

static void mq_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = data;
  if (id == MQTT_EVENT_CONNECTED) xEventGroupSetBits(mq_ev, MQ_CONNECTED);
  else if (id == MQTT_EVENT_PUBLISHED && e->msg_id == mq_wait_id) xEventGroupSetBits(mq_ev, MQ_PUBLISHED);
  else if (id == MQTT_EVENT_ERROR || id == MQTT_EVENT_DISCONNECTED) xEventGroupSetBits(mq_ev, MQ_FAILED);
}

// Publishes up to n payloads in one connection; returns how many were acknowledged.
static int send_mqtt(const char *const *payloads, int n, char *err, size_t ecap) {
  char uri[210], user[70], pass[70], topic[130];
  cfg_get("mq_uri", uri, sizeof(uri));
  cfg_get("mq_user", user, sizeof(user));
  cfg_get("mq_pass", pass, sizeof(pass));
  cfg_get("mq_topic", topic, sizeof(topic));
  if (!uri[0]) {
    snprintf(err, ecap, "broker URI missing");
    return 0;
  }
  if (!mq_ev) mq_ev = xEventGroupCreate();
  xEventGroupClearBits(mq_ev, 0xFF);
  esp_mqtt_client_config_t mc = {0};
  mc.broker.address.uri = uri;
  mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  mc.credentials.username = user[0] ? user : NULL;
  mc.credentials.authentication.password = pass[0] ? pass : NULL;
  mc.network.timeout_ms = 15000;
  mc.session.keepalive = 30;
  esp_mqtt_client_handle_t c = esp_mqtt_client_init(&mc);
  if (!c) {
    snprintf(err, ecap, "MQTT init failed (check the URI)");
    return 0;
  }
  esp_mqtt_client_register_event(c, ESP_EVENT_ANY_ID, mq_handler, NULL);
  esp_mqtt_client_start(c);
  int done = 0;
  EventBits_t b = xEventGroupWaitBits(mq_ev, MQ_CONNECTED | MQ_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
  if (!(b & MQ_CONNECTED)) {
    snprintf(err, ecap, "MQTT: could not connect to the broker");
  } else {
    for (int i = 0; i < n; i++) {
      xEventGroupClearBits(mq_ev, MQ_PUBLISHED);
      mq_wait_id = esp_mqtt_client_publish(c, topic, payloads[i], 0, 1, 0);
      b = xEventGroupWaitBits(mq_ev, MQ_PUBLISHED, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
      if (!(b & MQ_PUBLISHED)) {
        snprintf(err, ecap, "MQTT: publish not acknowledged");
        break;
      }
      done++;
    }
  }
  esp_mqtt_client_stop(c);
  esp_mqtt_client_destroy(c);
  return done;
}

// ---------- message formatting ----------

static void device_name(char *out, size_t cap) { cfg_get("ap_ssid", out, cap); }

static char *sms_text(const sms_rec_t *r) {
  char dev[40];
  device_name(dev, sizeof(dev));
  size_t cap = strlen(r->text) + 200;
  char *t = malloc(cap);
  snprintf(t, cap, "SMS from %s\n%s · %s\n\n%s", r->from, r->sent, dev, r->text);
  return t;
}

static char *sms_json(const sms_rec_t *r) {
  char dev[40];
  device_name(dev, sizeof(dev));
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "type", "sms");
  cJSON_AddNumberToObject(o, "id", r->id);
  cJSON_AddStringToObject(o, "from", r->from);
  cJSON_AddStringToObject(o, "sent", r->sent);
  cJSON_AddNumberToObject(o, "received", (double)r->rx);
  cJSON_AddStringToObject(o, "text", r->text);
  cJSON_AddBoolToObject(o, "login_code", r->otp);
  cJSON_AddStringToObject(o, "device", dev);
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  return s;
}

static char *event_json(const char *text) {
  char dev[40];
  device_name(dev, sizeof(dev));
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "type", "event");
  cJSON_AddStringToObject(o, "text", text);
  cJSON_AddStringToObject(o, "device", dev);
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  return s;
}

// ---------- worker ----------

static void save_last(chan_t *c) {
  nvs_handle_t h;
  if (nvs_open("fwd", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, c->key, c->last);
    nvs_commit(h);
    nvs_close(h);
  }
}

static void fail(chan_t *c, const char *err) {
  snprintf(c->last_error, sizeof(c->last_error), "%s", err);
  c->backoff_s = c->backoff_s ? (c->backoff_s * 2 > 600 ? 600 : c->backoff_s * 2) : 15;
  c->next_try_us = esp_timer_get_time() + (int64_t)c->backoff_s * 1000000;
  devlog("forward[%s]: %s; retry in %d s", c->key, err, c->backoff_s);
}

static void ok(chan_t *c) {
  c->backoff_s = 0;
  c->last_error[0] = 0;
}

// Sends the next pending SMS on channel c. Returns true if one was delivered or skipped.
static bool pump(chan_t *c) {
  uint32_t last = smsstore_last_id();
  if (c->last >= last || esp_timer_get_time() < c->next_try_us) return false;
  sms_rec_t r;
  uint32_t id = c->last + 1;
  if (!smsstore_get(id, &r)) {
    // Rotated away or never written: skip rather than block forever.
    devlog("forward[%s]: message #%lu not in the store, skipping", c->key, (unsigned long)id);
    c->last = id;
    save_last(c);
    return true;
  }
  bool delivered = true;
  char err[120] = "";
  if (r.otp && !cfg_bool("fw_otp")) {
    // Skipped by setting.
  } else if (!strcmp(c->key, "tg")) {
    char *t = sms_text(&r);
    delivered = send_telegram(t, err, sizeof(err));
    free(t);
  } else if (!strcmp(c->key, "wh")) {
    char *j = sms_json(&r);
    delivered = send_webhook(j, err, sizeof(err));
    free(j);
  } else {
    char *j = sms_json(&r);
    const char *p[1] = {j};
    delivered = send_mqtt(p, 1, err, sizeof(err)) == 1;
    free(j);
  }
  free(r.text);
  if (!delivered) {
    fail(c, err);
    return false;
  }
  ok(c);
  c->last = id;
  c->sent++;
  save_last(c);
  devlog("forward[%s]: delivered SMS #%lu", c->key, (unsigned long)id);
  return true;
}

static void send_events(void) {
  char text[240];
  int id;
  if (!events_next_unsent(text, sizeof(text), &id)) return;
  if (!cfg_bool("fw_events")) {
    events_mark_sent(id);
    return;
  }
  bool any_enabled = false, any_ok = false;
  for (int i = 0; i < 3; i++) {
    chan_t *c = &chans[i];
    if (!cfg_bool(c->en)) continue;
    any_enabled = true;
    char err[120];
    char dev[40], msg[300];
    device_name(dev, sizeof(dev));
    snprintf(msg, sizeof(msg), "[%s] %s", dev, text);
    bool d;
    if (!strcmp(c->key, "tg")) d = send_telegram(msg, err, sizeof(err));
    else {
      char *j = event_json(text);
      if (!strcmp(c->key, "wh")) d = send_webhook(j, err, sizeof(err));
      else {
        const char *p[1] = {j};
        d = send_mqtt(p, 1, err, sizeof(err)) == 1;
      }
      free(j);
    }
    any_ok |= d;
  }
  if (!any_enabled || any_ok) events_mark_sent(id);
}

static void fwd_task(void *arg) {
  nvs_handle_t h;
  if (nvs_open("fwd", NVS_READONLY, &h) == ESP_OK) {
    for (int i = 0; i < 3; i++) nvs_get_u32(h, chans[i].key, &chans[i].last);
    nvs_close(h);
  }
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    if (!netmgr_online()) continue;
    for (int round = 0; round < 20; round++) {
      bool any = false;
      for (int i = 0; i < 3; i++)
        if (cfg_bool(chans[i].en)) any |= pump(&chans[i]);
      if (!any) break;
    }
    send_events();
  }
}

void forward_start(void) { xTaskCreatePinnedToCore(fwd_task, "forward", 12288, NULL, 3, &task, 1); }

void forward_kick(void) {
  if (task) xTaskNotifyGive(task);
}

void forward_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "forward");
  uint32_t last = smsstore_last_id();
  for (int i = 0; i < 3; i++) {
    cJSON *c = cJSON_AddObjectToObject(o, chans[i].key);
    cJSON_AddBoolToObject(c, "enabled", cfg_bool(chans[i].en));
    cJSON_AddNumberToObject(c, "pending", last > chans[i].last ? last - chans[i].last : 0);
    cJSON_AddNumberToObject(c, "sent", chans[i].sent);
    cJSON_AddStringToObject(c, "error", chans[i].last_error);
  }
}

void forward_summary(char *out, size_t cap) {
  uint32_t last = smsstore_last_id();
  snprintf(out, cap, "forward: store last id %lu; tg en=%d last=%lu err=%s; wh en=%d last=%lu err=%s; mq en=%d "
           "last=%lu err=%s\n",
           (unsigned long)last, cfg_bool("tg_en"), (unsigned long)chans[0].last, chans[0].last_error, cfg_bool("wh_en"),
           (unsigned long)chans[1].last, chans[1].last_error, cfg_bool("mq_en"), (unsigned long)chans[2].last,
           chans[2].last_error);
}

bool forward_test(const char *ch, char *result, size_t cap) {
  if (!netmgr_online()) {
    snprintf(result, cap, "No internet on the current uplink yet.");
    return false;
  }
  char err[120] = "", dev[40];
  device_name(dev, sizeof(dev));
  char msg[120];
  snprintf(msg, sizeof(msg), "[%s] Test message from Portal Bridge", dev);
  bool okk;
  if (!strcmp(ch, "tg")) okk = send_telegram(msg, err, sizeof(err));
  else if (!strcmp(ch, "wh")) {
    char *j = event_json("test message");
    okk = send_webhook(j, err, sizeof(err));
    free(j);
  } else {
    char *j = event_json("test message");
    const char *p[1] = {j};
    okk = send_mqtt(p, 1, err, sizeof(err)) == 1;
    free(j);
  }
  snprintf(result, cap, "%s", okk ? "Delivered." : err);
  return okk;
}

bool telegram_find_chats(char *result, size_t cap) {
  char token[128], url[220];
  cfg_get("tg_token", token, sizeof(token));
  if (!token[0]) {
    snprintf(result, cap, "{\"error\":\"Save the bot token first.\"}");
    return false;
  }
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?limit=50", token);
  char *resp = malloc(32768);
  int st = http_send("GET", url, NULL, NULL, 0, NULL, resp, 32768);
  cJSON *out = cJSON_CreateArray();
  cJSON *j = st == 200 ? cJSON_Parse(resp) : NULL;
  cJSON *res = j ? cJSON_GetObjectItem(j, "result") : NULL;
  cJSON *u;
  cJSON_ArrayForEach(u, res) {
    cJSON *m = cJSON_GetObjectItem(u, "message");
    cJSON *chat = m ? cJSON_GetObjectItem(m, "chat") : NULL;
    if (!chat) continue;
    double id = cJSON_GetNumberValue(cJSON_GetObjectItem(chat, "id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(chat, "first_name"));
    if (!name) name = cJSON_GetStringValue(cJSON_GetObjectItem(chat, "title"));
    char ids[32];
    snprintf(ids, sizeof(ids), "%.0f", id);
    bool dup = false;
    cJSON *e;
    cJSON_ArrayForEach(e, out) dup |= !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(e, "id")), ids);
    if (dup) continue;
    cJSON *e2 = cJSON_CreateObject();
    cJSON_AddStringToObject(e2, "id", ids);
    cJSON_AddStringToObject(e2, "name", name ? name : "");
    cJSON_AddItemToArray(out, e2);
  }
  cJSON *wrap = cJSON_CreateObject();
  if (st != 200) {
    char e[80];
    snprintf(e, sizeof(e), "Telegram answered HTTP %d (check the token)", st);
    cJSON_AddStringToObject(wrap, "error", e);
  }
  cJSON_AddItemToObject(wrap, "chats", out);
  char *s = cJSON_PrintUnformatted(wrap);
  snprintf(result, cap, "%s", s);
  free(s);
  cJSON_Delete(wrap);
  cJSON_Delete(j);
  free(resp);
  return st == 200;
}

bool telegram_send_document(const char *filename, const char *data, size_t len, char *result, size_t cap) {
  char token[128], chat[40], url[200];
  cfg_get("tg_token", token, sizeof(token));
  cfg_get("tg_chat", chat, sizeof(chat));
  if (!token[0] || !chat[0]) {
    snprintf(result, cap, "Set the Telegram bot token and chat id first.");
    return false;
  }
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendDocument", token);
  const char *bd = "----portalbridge7f3a";
  char head[400], tail[64];
  int hl = snprintf(head, sizeof(head),
                    "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n--%s\r\nContent-Disposition: "
                    "form-data; name=\"document\"; filename=\"%s\"\r\nContent-Type: text/plain\r\n\r\n",
                    bd, chat, bd, filename);
  int tl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", bd);
  size_t total = (size_t)hl + len + (size_t)tl;
  char *body = malloc(total);
  if (!body) {
    snprintf(result, cap, "out of memory");
    return false;
  }
  memcpy(body, head, (size_t)hl);
  memcpy(body + hl, data, len);
  memcpy(body + hl + len, tail, (size_t)tl);
  char ct[80];
  snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", bd);
  char resp[200];
  int st = http_send("POST", url, ct, body, total, NULL, resp, sizeof(resp));
  free(body);
  snprintf(result, cap, st == 200 ? "Sent to Telegram." : "Telegram HTTP %d", st);
  return st == 200;
}

static int gh_post(const char *path, const char *json, char *resp, size_t rcap) {
  char token[220], repo[110], url[260], auth[240];
  cfg_get("gh_token", token, sizeof(token));
  cfg_get("gh_repo", repo, sizeof(repo));
  snprintf(url, sizeof(url), "https://api.github.com/repos/%s%s", repo, path);
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  const char *hdrs[] = {"Authorization", auth, "Accept", "application/vnd.github+json", "User-Agent", "portal-bridge",
                        NULL};
  return http_send("POST", url, "application/json", json, strlen(json), hdrs, resp, rcap);
}

bool github_create_issue(const char *title, const char *body, const char *extra, char *result, size_t cap) {
  char token[8];
  cfg_get("gh_token", token, sizeof(token));
  if (!token[0]) {
    snprintf(result, cap, "Set the GitHub token under Settings > Reports first.");
    return false;
  }
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "title", title);
  size_t bl = strlen(body);
  if (bl > 60000) bl = 60000;
  char *fenced = malloc(bl + 32);
  snprintf(fenced, bl + 32, "```text\n%.*s\n```", (int)bl, body);
  cJSON_AddStringToObject(o, "body", fenced);
  free(fenced);
  char *j = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  char *resp = malloc(8192);
  int st = gh_post("/issues", j, resp, 8192);
  free(j);
  cJSON *r = st == 201 ? cJSON_Parse(resp) : NULL;
  const char *html = r ? cJSON_GetStringValue(cJSON_GetObjectItem(r, "html_url")) : NULL;
  int number = r ? (int)cJSON_GetNumberValue(cJSON_GetObjectItem(r, "number")) : 0;
  if (!html) {
    snprintf(result, cap, "GitHub answered HTTP %d: %.120s", st, resp);
    cJSON_Delete(r);
    free(resp);
    return false;
  }
  snprintf(result, cap, "%s", html);
  cJSON_Delete(r);
  // Extra material (a recording) as comments, 60000 characters each.
  size_t el = extra ? strlen(extra) : 0, off = 0;
  char path[48];
  snprintf(path, sizeof(path), "/issues/%d/comments", number);
  for (int part = 1; off < el && part <= 10; part++) {
    size_t n = el - off > 60000 ? 60000 : el - off;
    // Do not cut a UTF-8 sequence in half.
    while (n > 0 && off + n < el && ((unsigned char)extra[off + n] & 0xC0) == 0x80) n--;
    char *c = malloc(n + 64);
    snprintf(c, n + 64, "part %d\n```json\n%.*s\n```", part, (int)n, extra + off);
    cJSON *co = cJSON_CreateObject();
    cJSON_AddStringToObject(co, "body", c);
    free(c);
    char *cj = cJSON_PrintUnformatted(co);
    cJSON_Delete(co);
    int cs = gh_post(path, cj, resp, 512);
    free(cj);
    if (cs != 201) {
      size_t rl = strlen(result);
      snprintf(result + rl, cap - rl, " (comment %d failed: HTTP %d)", part, cs);
      break;
    }
    off += n;
  }
  free(resp);
  return true;
}
