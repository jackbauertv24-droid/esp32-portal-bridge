#include "smsstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "devlog.h"
#include "forward.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings.h"
#include "textutil.h"

#define FILE_CUR "/data/sms.jsonl"
#define FILE_OLD "/data/sms.old.jsonl"
#define ROTATE_BYTES (512 * 1024)

static SemaphoreHandle_t mtx;
static uint32_t last_id;
static int count;

// Recent arrivals in RAM for the login-code waiter.
typedef struct {
  int64_t at_us;
  char from[48];
  char text[400];
} recent_t;
static recent_t recent[12];
static int recent_next;

void smsstore_init(void) {
  mtx = xSemaphoreCreateMutex();
  nvs_handle_t h;
  if (nvs_open("sms", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, "last_id", &last_id);
    nvs_close(h);
  }
  FILE *f = fopen(FILE_CUR, "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f))
      if (strchr(line, '\n')) count++;
    fclose(f);
  }
  devlog("sms store: last id %lu, %d in current file", (unsigned long)last_id, count);
}

bool sms_match_code(const char *from, const char *text, char *code, size_t cap) {
  char snd[48], kw[48];
  cfg_get("otp_sender", snd, sizeof(snd));
  cfg_get("otp_kw", kw, sizeof(kw));
  if (snd[0] && !str_contains_ci(from, snd)) return false;
  if (kw[0] && !str_contains_ci(text, kw)) return false;
  return otp_extract(text, cfg_int("otp_min"), cfg_int("otp_max"), code, cap) == 1;
}

uint32_t smsstore_add(const char *from, const char *sent, const char *text, int parts) {
  char code[16];
  bool otp = sms_match_code(from, text, code, sizeof(code));
  xSemaphoreTake(mtx, portMAX_DELAY);
  struct stat sb;
  if (stat(FILE_CUR, &sb) == 0 && sb.st_size > ROTATE_BYTES) {
    unlink(FILE_OLD);
    rename(FILE_CUR, FILE_OLD);
    count = 0;
  }
  uint32_t id = last_id + 1;
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "id", id);
  cJSON_AddNumberToObject(o, "rx", (double)time(NULL));
  cJSON_AddStringToObject(o, "from", from);
  cJSON_AddStringToObject(o, "sent", sent);
  cJSON_AddStringToObject(o, "text", text);
  cJSON_AddNumberToObject(o, "parts", parts);
  cJSON_AddBoolToObject(o, "otp", otp);
  char *line = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  bool ok = false;
  FILE *f = fopen(FILE_CUR, "a");
  if (f && line) {
    ok = fputs(line, f) >= 0 && fputc('\n', f) != EOF;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
  }
  if (f) ok = (fclose(f) == 0) && ok;
  free(line);
  if (ok) {
    nvs_handle_t h;
    if (nvs_open("sms", NVS_READWRITE, &h) == ESP_OK) {
      ok = nvs_set_u32(h, "last_id", id) == ESP_OK && nvs_commit(h) == ESP_OK;
      nvs_close(h);
    } else {
      ok = false;
    }
  }
  if (ok) {
    last_id = id;
    count++;
    recent_t *r = &recent[recent_next++ % 12];
    r->at_us = esp_timer_get_time();
    snprintf(r->from, sizeof(r->from), "%s", from);
    snprintf(r->text, sizeof(r->text), "%s", text);
  }
  xSemaphoreGive(mtx);
  if (ok) forward_kick();
  if (ok) devlog("sms store: saved #%lu (%d part(s)%s)", (unsigned long)id, parts, otp ? ", login code" : "");
  else devlog("sms store: WRITE FAILED, message stays on the SIM");
  return ok ? id : 0;
}

uint32_t smsstore_last_id(void) { return last_id; }
int smsstore_count(void) { return count; }

static bool parse_line(const char *line, sms_rec_t *rec) {
  cJSON *o = cJSON_Parse(line);
  if (!o) return false;
  memset(rec, 0, sizeof(*rec));
  rec->id = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "id"));
  rec->rx = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "rx"));
  const char *s;
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(o, "from")))) snprintf(rec->from, sizeof(rec->from), "%s", s);
  if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(o, "sent")))) snprintf(rec->sent, sizeof(rec->sent), "%s", s);
  s = cJSON_GetStringValue(cJSON_GetObjectItem(o, "text"));
  rec->text = strdup(s ? s : "");
  rec->parts = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "parts"));
  rec->otp = cJSON_IsTrue(cJSON_GetObjectItem(o, "otp"));
  cJSON_Delete(o);
  return true;
}

// Reads every line of both files (old first) and calls cb; stops if cb returns false.
static void scan(bool (*cb)(const char *line, void *ctx), void *ctx) {
  const char *files[2] = {FILE_OLD, FILE_CUR};
  size_t cap = 24 * 1024;
  char *line = malloc(cap);
  for (int i = 0; i < 2 && line; i++) {
    FILE *f = fopen(files[i], "r");
    if (!f) continue;
    bool go = true;
    while (go && fgets(line, cap, f)) go = cb(line, ctx);
    fclose(f);
    if (!go) break;
  }
  free(line);
}

typedef struct {
  uint32_t id;
  sms_rec_t *rec;
  bool found;
} find_ctx_t;

static bool find_cb(const char *line, void *vctx) {
  find_ctx_t *c = vctx;
  const char *p = strstr(line, "\"id\":");
  if (!p || (uint32_t)strtoul(p + 5, NULL, 10) != c->id) return true;
  c->found = parse_line(line, c->rec);
  return !c->found;
}

bool smsstore_get(uint32_t id, sms_rec_t *rec) {
  find_ctx_t c = {id, rec, false};
  xSemaphoreTake(mtx, portMAX_DELAY);
  scan(find_cb, &c);
  xSemaphoreGive(mtx);
  return c.found;
}

typedef struct {
  cJSON *arr;
  int limit;
} list_ctx_t;

static bool list_cb(const char *line, void *vctx) {
  list_ctx_t *c = vctx;
  cJSON *o = cJSON_Parse(line);
  if (!o) return true;
  cJSON_InsertItemInArray(c->arr, 0, o);  // newest first
  while (cJSON_GetArraySize(c->arr) > c->limit) cJSON_DeleteItemFromArray(c->arr, c->limit);
  return true;
}

cJSON *smsstore_list_json(int limit) {
  list_ctx_t c = {cJSON_CreateArray(), limit};
  xSemaphoreTake(mtx, portMAX_DELAY);
  scan(list_cb, &c);
  xSemaphoreGive(mtx);
  return c.arr;
}

bool sms_wait_code(int64_t since_us, int timeout_s, char *code, size_t cap) {
  int64_t until = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
  devlog("otp: waiting up to %d s for a login-code SMS", timeout_s);
  while (esp_timer_get_time() < until) {
    bool got = false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    int64_t best = 0;
    for (int i = 0; i < 12; i++) {
      recent_t *r = &recent[i];
      if (r->at_us > since_us && (!best || r->at_us < best) && sms_match_code(r->from, r->text, code, cap)) {
        best = r->at_us;
        got = true;
      }
    }
    xSemaphoreGive(mtx);
    if (got) {
      // Re-extract from the chosen message (the loop may have overwritten `code`).
      xSemaphoreTake(mtx, portMAX_DELAY);
      for (int i = 0; i < 12; i++)
        if (recent[i].at_us == best) sms_match_code(recent[i].from, recent[i].text, code, cap);
      xSemaphoreGive(mtx);
      devlog("otp: code received (%u digits)", (unsigned)strlen(code));
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  devlog("otp: no matching SMS within %d s", timeout_s);
  return false;
}

// ---------- SMS manager ----------

#define SENT_FILE "/data/sms_sent.jsonl"
static uint32_t sent_last, seen_id;
static bool sent_loaded;

static uint32_t nvs_u32(const char *k) {
  uint32_t v = 0;
  nvs_handle_t h;
  if (nvs_open("sms", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, k, &v);
    nvs_close(h);
  }
  return v;
}

static void nvs_put_u32(const char *k, uint32_t v) {
  nvs_handle_t h;
  if (nvs_open("sms", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, k, v);
    nvs_commit(h);
    nvs_close(h);
  }
}

uint32_t smsstore_add_sent(const char *to, const char *text, int parts, bool ok, const char *error) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (!sent_loaded) {
    sent_last = nvs_u32("sent_id");
    sent_loaded = true;
  }
  struct stat sb;
  if (stat(SENT_FILE, &sb) == 0 && sb.st_size > ROTATE_BYTES) unlink(SENT_FILE);
  uint32_t id = sent_last + 1;
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "id", id);
  cJSON_AddNumberToObject(o, "rx", (double)time(NULL));
  cJSON_AddStringToObject(o, "to", to);
  cJSON_AddStringToObject(o, "text", text);
  cJSON_AddNumberToObject(o, "parts", parts);
  cJSON_AddBoolToObject(o, "ok", ok);
  if (error && error[0]) cJSON_AddStringToObject(o, "error", error);
  char *line = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  FILE *f = fopen(SENT_FILE, "a");
  bool w = f && line && fputs(line, f) >= 0 && fputc('\n', f) != EOF;
  if (f) fclose(f);
  free(line);
  if (w) {
    sent_last = id;
    nvs_put_u32("sent_id", id);
  }
  xSemaphoreGive(mtx);
  return w ? id : 0;
}

cJSON *smsstore_list_sent_json(int limit) {
  cJSON *arr = cJSON_CreateArray();
  xSemaphoreTake(mtx, portMAX_DELAY);
  FILE *f = fopen(SENT_FILE, "r");
  if (f) {
    char *line = malloc(16 * 1024);
    while (line && fgets(line, 16 * 1024, f)) {
      cJSON *o = cJSON_Parse(line);
      if (!o) continue;
      cJSON_InsertItemInArray(arr, 0, o);
      while (cJSON_GetArraySize(arr) > limit) cJSON_DeleteItemFromArray(arr, limit);
    }
    free(line);
    fclose(f);
  }
  xSemaphoreGive(mtx);
  return arr;
}

// Rewrites `path` without the listed ids. Returns removed count.
static int rewrite_without(const char *path, const uint32_t *ids, int n) {
  FILE *in = fopen(path, "r");
  if (!in) return 0;
  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *out = fopen(tmp, "w");
  if (!out) {
    fclose(in);
    return 0;
  }
  int removed = 0, kept_lines = 0;
  char *line = malloc(24 * 1024);
  while (line && fgets(line, 24 * 1024, in)) {
    const char *p = strstr(line, "\"id\":");
    uint32_t id = p ? (uint32_t)strtoul(p + 5, NULL, 10) : 0;
    bool drop = false;
    for (int i = 0; i < n && !drop; i++) drop = ids[i] == id;
    if (drop) removed++;
    else {
      fputs(line, out);
      kept_lines++;
    }
  }
  free(line);
  fclose(in);
  bool ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
  fclose(out);
  if (ok && removed) {
    unlink(path);
    rename(tmp, path);
  } else {
    unlink(tmp);
    if (!ok) removed = 0;
  }
  (void)kept_lines;
  return removed;
}

int smsstore_delete(const char *box, const uint32_t *ids, int n) {
  int removed = 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (!strcmp(box, "out")) {
    removed = rewrite_without(SENT_FILE, ids, n);
  } else {
    removed = rewrite_without(FILE_OLD, ids, n);
    int r2 = rewrite_without(FILE_CUR, ids, n);
    removed += r2;
    count -= r2;
    if (count < 0) count = 0;
  }
  xSemaphoreGive(mtx);
  devlog("sms store: deleted %d message(s) from %s", removed, box);
  return removed;
}

uint32_t smsstore_seen(void) {
  if (!seen_id) seen_id = nvs_u32("seen");
  return seen_id;
}

void smsstore_set_seen(uint32_t id) {
  if (id <= smsstore_seen()) return;
  seen_id = id;
  nvs_put_u32("seen", id);
}
