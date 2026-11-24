// Tasks whose stack can live in PSRAM, saving scarce internal RAM (which the
// Wi-Fi radio, DMA and the camera need). Only for tasks that never write or
// read flash themselves (NVS, SPIFFS, OTA): flash access turns PSRAM off
// briefly, which a running task with a PSRAM stack would not survive. Falls
// back to an internal stack if PSRAM stacks are refused.
#pragma once
#include "devlog.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static inline BaseType_t task_psram(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                    BaseType_t core) {
  BaseType_t r = xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, NULL, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (r == pdPASS) return r;
  devlog("task %s: PSRAM stack refused, using internal RAM", name);
  return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, NULL, core);
}
