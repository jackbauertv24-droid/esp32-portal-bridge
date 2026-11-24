#include "events.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "devlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_EVENTS 40

typedef struct {
  int id;
  time_t t;
  char text[200];
  char detail[80];  // forwarded only; kept out of the log, the event list and reports
  int sent;
} ev_t;

static ev_t evs[MAX_EVENTS];
static int next_id = 1;
static SemaphoreHandle_t mtx;

void events_init(void) { mtx = xSemaphoreCreateMutex(); }

static void post(const char *detail, const char *text) {
  devlog("EVENT %s", text);
  xSemaphoreTake(mtx, portMAX_DELAY);
  ev_t *slot = &evs[0];
  for (int i = 0; i < MAX_EVENTS; i++)
    if (evs[i].id < slot->id) slot = &evs[i];
  slot->id = next_id++;
  slot->t = time(NULL);
  slot->sent = 0;
  snprintf(slot->text, sizeof(slot->text), "%s", text);
  snprintf(slot->detail, sizeof(slot->detail), "%s", detail ? detail : "");
  xSemaphoreGive(mtx);
}

void event_post(const char *fmt, ...) {
  char text[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  post(NULL, text);
}

void event_post_detail(const char *detail, const char *fmt, ...) {
  char text[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  post(detail, text);
}

cJSON *events_json(void) {
  cJSON *arr = cJSON_CreateArray();
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int id = next_id - 1; id > 0 && id >= next_id - MAX_EVENTS; id--)
    for (int i = 0; i < MAX_EVENTS; i++)
      if (evs[i].id == id) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "t", (double)evs[i].t);
        cJSON_AddStringToObject(o, "text", evs[i].text);
        cJSON_AddItemToArray(arr, o);
      }
  xSemaphoreGive(mtx);
  return arr;
}

int events_next_unsent(char *out, int cap, int *id) {
  int found = 0, best = 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < MAX_EVENTS; i++)
    if (evs[i].id && !evs[i].sent && (!best || evs[i].id < best)) {
      best = evs[i].id;
      char ts[24] = "";
      struct tm tm;
      localtime_r(&evs[i].t, &tm);
      if (evs[i].t > 1600000000) strftime(ts, sizeof(ts), "%m-%d %H:%M ", &tm);
      snprintf(out, cap, "%s%s%s", ts, evs[i].text, evs[i].detail);
      found = 1;
    }
  xSemaphoreGive(mtx);
  if (id) *id = best;
  return found;
}

void events_mark_sent(int id) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < MAX_EVENTS; i++)
    if (evs[i].id == id) evs[i].sent = 1;
  xSemaphoreGive(mtx);
}
