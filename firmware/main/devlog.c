#include "devlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static size_t RING_SIZE = 96 * 1024;

static char *ring;
static size_t head;
static int wrapped;
static SemaphoreHandle_t mtx;
static vprintf_like_t orig_vprintf;

static void ring_put(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    ring[head++] = s[i];
    if (head == RING_SIZE) {
      head = 0;
      wrapped = 1;
    }
  }
}

static void store(const char *s, size_t n) {
  if (!ring || !mtx || xPortInIsrContext()) return;
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
    ring_put(s, n);
    xSemaphoreGive(mtx);
  }
}

// ESP-IDF log hook: print as usual, and keep a copy without colour codes.
static int log_hook(const char *fmt, va_list ap) {
  char line[384];
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  if (n > 0) {
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    // Strip ANSI colour sequences.
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
      if (line[i] == 0x1b) {
        while (i < len && line[i] != 'm') i++;
        continue;
      }
      line[o++] = line[i];
    }
    store(line, o);
  }
  return orig_vprintf ? orig_vprintf(fmt, ap) : vprintf(fmt, ap);
}

void devlog_init(void) {
  ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
  if (!ring) {
    RING_SIZE = 16 * 1024;
    ring = malloc(RING_SIZE);
  }
  mtx = xSemaphoreCreateMutex();
  orig_vprintf = esp_log_set_vprintf(log_hook);
}

void devlog(const char *fmt, ...) {
  char line[512];
  int64_t us = esp_timer_get_time();
  int n = snprintf(line, sizeof(line), "[%6lld.%03lld] ", us / 1000000, (us / 1000) % 1000);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\n';
  line[len] = 0;
  fputs(line, stdout);
  store(line, len);
}

char *devlog_dump(size_t *out_len) {
  size_t cap = RING_SIZE + 1;
  char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!out) out = malloc(cap);
  if (!out) return NULL;
  size_t n = 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (wrapped) {
    size_t start = head;
    while (start < RING_SIZE && ring[start] != '\n') start++;
    if (start < RING_SIZE) {
      memcpy(out, ring + start + 1, RING_SIZE - start - 1);
      n = RING_SIZE - start - 1;
    }
  }
  memcpy(out + n, ring, head);
  n += head;
  xSemaphoreGive(mtx);
  out[n] = 0;
  if (out_len) *out_len = n;
  return out;
}
