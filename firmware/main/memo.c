#include "memo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "devlog.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "events.h"
#include "forward.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "modem_at.h"
#include "netmgr.h"
#include "settings.h"

#define MAX_MEMO 30
#define INDEX "/data/memos.json"

typedef struct {
  uint32_t id;
  time_t t;
  char number[24];
  int secs;
  long bytes;
  char state[12];  // copying | saved | failed
  bool delivered;
  int tries;
  char result[96];
} memo_t;

typedef struct {
  char path[40];
  char number[24];
  int secs;
} job_t;

static memo_t m[MAX_MEMO];
static int n_memo;
static SemaphoreHandle_t mtx;
static QueueHandle_t jobs;
static volatile uint32_t resend_id;

static void file_of(uint32_t id, char *out, size_t cap) { snprintf(out, cap, "/data/memo_%lu.wav", (unsigned long)id); }

static void save_index(void) {
  cJSON *a = cJSON_CreateArray();
  for (int i = 0; i < n_memo; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", m[i].id);
    cJSON_AddNumberToObject(o, "t", (double)m[i].t);
    cJSON_AddStringToObject(o, "number", m[i].number);
    cJSON_AddNumberToObject(o, "secs", m[i].secs);
    cJSON_AddNumberToObject(o, "bytes", m[i].bytes);
    cJSON_AddStringToObject(o, "state", m[i].state);
    cJSON_AddBoolToObject(o, "delivered", m[i].delivered);
    cJSON_AddNumberToObject(o, "tries", m[i].tries);
    cJSON_AddStringToObject(o, "result", m[i].result);
    cJSON_AddItemToArray(a, o);
  }
  char *s = cJSON_PrintUnformatted(a);
  cJSON_Delete(a);
  FILE *f = fopen(INDEX, "w");
  if (f && s) fputs(s, f);
  if (f) fclose(f);
  free(s);
}

static void load_index(void) {
  FILE *f = fopen(INDEX, "r");
  if (!f) return;
  char *b = malloc(16384);
  size_t n = b ? fread(b, 1, 16383, f) : 0;
  fclose(f);
  if (!b) return;
  b[n] = 0;
  cJSON *a = cJSON_Parse(b);
  free(b);
  cJSON *o;
  cJSON_ArrayForEach(o, a) {
    if (n_memo >= MAX_MEMO) break;
    memo_t *x = &m[n_memo++];
    memset(x, 0, sizeof(*x));
    x->id = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "id"));
    x->t = (time_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "t"));
    snprintf(x->number, sizeof(x->number), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "number")) ?: "");
    x->secs = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "secs"));
    x->bytes = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "bytes"));
    snprintf(x->state, sizeof(x->state), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "state")) ?: "failed");
    x->delivered = cJSON_IsTrue(cJSON_GetObjectItem(o, "delivered"));
    x->tries = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "tries"));
    snprintf(x->result, sizeof(x->result), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "result")) ?: "");
    // A copy interrupted by a restart cannot be resumed.
    if (!strcmp(x->state, "copying")) snprintf(x->state, sizeof(x->state), "failed");
  }
  cJSON_Delete(a);
}

static memo_t *find(uint32_t id) {
  for (int i = 0; i < n_memo; i++)
    if (m[i].id == id) return &m[i];
  return NULL;
}

static void remove_at(int i) {
  char p[40];
  file_of(m[i].id, p, sizeof(p));
  unlink(p);
  memmove(&m[i], &m[i + 1], sizeof(memo_t) * (size_t)(n_memo - i - 1));
  n_memo--;
}

// Keeps the newest cm_keep memos (and always room for one more).
static void trim(void) {
  int keep = cfg_int("cm_keep");
  while (n_memo > 0 && (n_memo >= MAX_MEMO || n_memo > keep)) remove_at(0);
}

static bool deliver(memo_t *x) {
  char dest[12];
  cfg_get("cm_dest", dest, sizeof(dest));
  if (!strcmp(dest, "board")) {
    snprintf(x->result, sizeof(x->result), "kept on the board only");
    return true;
  }
  if (!netmgr_online()) {
    snprintf(x->result, sizeof(x->result), "waiting for internet");
    return false;
  }
  char path[40];
  file_of(x->id, path, sizeof(path));
  FILE *f = fopen(path, "r");
  if (!f) {
    snprintf(x->result, sizeof(x->result), "file missing");
    return true;  // nothing left to send
  }
  char *buf = heap_caps_malloc((size_t)x->bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t got = buf ? fread(buf, 1, (size_t)x->bytes, f) : 0;
  fclose(f);
  if (!buf || got != (size_t)x->bytes) {
    free(buf);
    snprintf(x->result, sizeof(x->result), "could not read the file");
    return false;
  }
  char when[24] = "", caption[160], fname[48], r1[96] = "", r2[96] = "";
  if (x->t > 1600000000) {
    struct tm tm;
    localtime_r(&x->t, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
  }
  snprintf(caption, sizeof(caption), "Voice memo from %s, %d s%s%s", x->number[0] ? x->number : "a hidden number",
           x->secs, when[0] ? ", " : "", when);
  snprintf(fname, sizeof(fname), "voice-memo-%lu.wav", (unsigned long)x->id);
  bool ok = true;
  if (!strcmp(dest, "telegram") || !strcmp(dest, "both")) {
    char chat[40];
    cfg_get("cm_tg_chat", chat, sizeof(chat));
    ok = telegram_send_file(chat, fname, "audio/wav", caption, buf, got, r1, sizeof(r1)) && ok;
  }
  if (!strcmp(dest, "webhook") || !strcmp(dest, "both")) {
    char url[256];
    cfg_get("cm_wh_url", url, sizeof(url));
    if (!url[0]) cfg_get("wh_url", url, sizeof(url));
    if (!url[0]) {
      snprintf(r2, sizeof(r2), "no webhook URL set");
      ok = false;
    } else {
      ok = webhook_send_file(url, "audio/wav", x->number, buf, got, r2, sizeof(r2)) && ok;
    }
  }
  free(buf);
  snprintf(x->result, sizeof(x->result), "%s%s%s", r1, r1[0] && r2[0] ? " " : "", r2);
  return ok;
}

static void copy_job(const job_t *j) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  trim();
  uint32_t id = 1;
  for (int i = 0; i < n_memo; i++)
    if (m[i].id >= id) id = m[i].id + 1;
  memo_t *x = &m[n_memo++];
  memset(x, 0, sizeof(*x));
  x->id = id;
  x->t = time(NULL);
  snprintf(x->number, sizeof(x->number), "%s", j->number);
  x->secs = j->secs;
  snprintf(x->state, sizeof(x->state), "copying");
  save_index();
  xSemaphoreGive(mtx);

  char path[40], err[96] = "";
  file_of(id, path, sizeof(path));
  FILE *f = fopen(path, "w");
  long n = f ? modem_file_read(j->path, f, err, sizeof(err)) : -1;
  if (f) fclose(f);
  if (!f) snprintf(err, sizeof(err), "cannot create the file on the board");
  if (n > 0) modem_file_delete(j->path);
  devlog("memo: #%lu copied from the modem: %ld bytes%s%s", (unsigned long)id, n, err[0] ? " - " : "", err);

  xSemaphoreTake(mtx, portMAX_DELAY);
  x = find(id);
  if (x) {
    if (n > 44) {  // more than a WAV header
      x->bytes = n;
      snprintf(x->state, sizeof(x->state), "saved");
    } else {
      snprintf(x->state, sizeof(x->state), "failed");
      snprintf(x->result, sizeof(x->result), "%s", err[0] ? err : "empty recording");
      unlink(path);
    }
    save_index();
  }
  xSemaphoreGive(mtx);
}

static void deliver_pending(bool only_id, uint32_t id) {
  for (int i = 0; i < MAX_MEMO; i++) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (i >= n_memo) {
      xSemaphoreGive(mtx);
      break;
    }
    memo_t x = m[i];
    xSemaphoreGive(mtx);
    if (strcmp(x.state, "saved") || (only_id ? x.id != id : (x.delivered || x.tries >= 20))) continue;
    x.tries++;
    bool ok = deliver(&x);
    x.delivered = ok;
    devlog("memo: #%lu delivery %s: %s", (unsigned long)x.id, ok ? "done" : "failed", x.result);
    xSemaphoreTake(mtx, portMAX_DELAY);
    memo_t *p = find(x.id);
    if (p) {
      p->delivered = x.delivered;
      p->tries = x.tries;
      snprintf(p->result, sizeof(p->result), "%s", x.result);
      save_index();
    }
    xSemaphoreGive(mtx);
  }
}

static void task(void *arg) {
  int64_t next_retry = 0;
  for (;;) {
    job_t j;
    if (xQueueReceive(jobs, &j, pdMS_TO_TICKS(5000)) == pdTRUE) {
      copy_job(&j);
      deliver_pending(false, 0);
      continue;
    }
    if (resend_id) {
      uint32_t id = resend_id;
      resend_id = 0;
      deliver_pending(true, id);
    }
    if (esp_timer_get_time() > next_retry) {
      next_retry = esp_timer_get_time() + 300LL * 1000000;  // retry undelivered memos every 5 minutes
      deliver_pending(false, 0);
    }
  }
}

void memo_start(void) {
  mtx = xSemaphoreCreateMutex();
  jobs = xQueueCreate(4, sizeof(job_t));
  load_index();
  xTaskCreatePinnedToCore(task, "memo", 8192, NULL, 2, NULL, 1);
}

void memo_add(const char *modem_path, const char *number, int secs) {
  job_t j = {.secs = secs};
  snprintf(j.path, sizeof(j.path), "%s", modem_path);
  snprintf(j.number, sizeof(j.number), "%s", number ? number : "");
  if (!jobs || xQueueSend(jobs, &j, 0) != pdTRUE) devlog("memo: queue full, recording left on the modem");
}

cJSON *memo_json(void) {
  cJSON *a = cJSON_CreateArray();
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = n_memo - 1; i >= 0; i--) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", m[i].id);
    cJSON_AddNumberToObject(o, "t", (double)m[i].t);
    cJSON_AddStringToObject(o, "number", m[i].number);
    cJSON_AddNumberToObject(o, "secs", m[i].secs);
    cJSON_AddNumberToObject(o, "bytes", m[i].bytes);
    cJSON_AddStringToObject(o, "state", m[i].state);
    cJSON_AddBoolToObject(o, "delivered", m[i].delivered);
    cJSON_AddStringToObject(o, "result", m[i].result);
    cJSON_AddItemToArray(a, o);
  }
  xSemaphoreGive(mtx);
  return a;
}

bool memo_file(uint32_t id, char *path, size_t cap) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  memo_t *x = find(id);
  bool ok = x && !strcmp(x->state, "saved");
  xSemaphoreGive(mtx);
  if (ok) file_of(id, path, cap);
  return ok;
}

bool memo_delete(uint32_t id) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool ok = false;
  for (int i = 0; i < n_memo; i++)
    if (m[i].id == id && strcmp(m[i].state, "copying")) {
      remove_at(i);
      save_index();
      ok = true;
      break;
    }
  xSemaphoreGive(mtx);
  return ok;
}

bool memo_resend(uint32_t id) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  memo_t *x = find(id);
  bool ok = x && !strcmp(x->state, "saved");
  if (ok) x->tries = 0;
  xSemaphoreGive(mtx);
  if (ok) resend_id = id;
  return ok;
}

// Counts only: caller numbers stay out of reports.
void memo_summary(char *out, size_t cap) {
  int saved = 0, delivered = 0, failed = 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < n_memo; i++) {
    saved += !strcmp(m[i].state, "saved");
    delivered += m[i].delivered;
    failed += !strcmp(m[i].state, "failed");
  }
  xSemaphoreGive(mtx);
  snprintf(out, cap, "memos: %d on board, %d delivered, %d failed to copy\n", saved, delivered, failed);
}
