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

// ESP-IDF log hook: formats each line once into a shared buffer (not on the
// calling task's stack: library tasks such as the USB modem daemon have only
// 3 KB), prints it to the console and keeps a copy without colour codes.
static char hook_line[384];
static int log_hook(const char *fmt, va_list ap) {
  if (!ring || !mtx || xPortInIsrContext() || xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) != pdTRUE)
    return orig_vprintf ? orig_vprintf(fmt, ap) : vprintf(fmt, ap);
  int n = vsnprintf(hook_line, sizeof(hook_line), fmt, ap);
  if (n > 0) {
    size_t len = (size_t)n < sizeof(hook_line) ? (size_t)n : sizeof(hook_line) - 1;
    fwrite(hook_line, 1, len, stdout);
    // Keep a copy without ANSI colour sequences.
    for (size_t i = 0; i < len; i++) {
      if (hook_line[i] == 0x1b) {
        while (i < len && hook_line[i] != 'm') i++;
        continue;
      }
      ring_put(&hook_line[i], 1);
    }
  }
  xSemaphoreGive(mtx);
  return n;
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
