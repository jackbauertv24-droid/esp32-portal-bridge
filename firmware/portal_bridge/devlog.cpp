#include "devlog.h"

#include <stdarg.h>

static const size_t RING_SIZE = 24 * 1024;
static char *ring = nullptr;
static size_t head = 0;      // next write position
static bool wrapped = false;
static SemaphoreHandle_t mtx = nullptr;

void devlog_begin() {
  mtx = xSemaphoreCreateMutex();
  ring = (char *)(psramFound() ? ps_malloc(RING_SIZE) : malloc(RING_SIZE));
}

static void ring_put(const char *s, size_t n) {
  if (!ring) return;
  for (size_t i = 0; i < n; i++) {
    ring[head++] = s[i];
    if (head == RING_SIZE) {
      head = 0;
      wrapped = true;
    }
  }
}

void devlog(const char *fmt, ...) {
  char line[512];
  uint32_t ms = millis();
  int n = snprintf(line, sizeof(line), "[%6lu.%03lu] ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\n';
  line[len] = 0;
  Serial.print(line);
  if (mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
    ring_put(line, len);
    xSemaphoreGive(mtx);
  }
}

void devlog_dump(String &out) {
  if (!ring || !mtx) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (wrapped) {
    // Skip the partial first line of the overwritten region.
    size_t start = head;
    while (start < RING_SIZE && ring[start] != '\n') start++;
    if (start < RING_SIZE) out.concat(ring + start + 1, RING_SIZE - start - 1);
  }
  out.concat(ring, head);
  xSemaphoreGive(mtx);
}
