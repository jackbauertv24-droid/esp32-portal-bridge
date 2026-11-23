#include "led.h"

#include <string.h>

#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "modem_at.h"
#include "netmgr.h"
#include "settings.h"

static led_strip_handle_t strip;

static void set(int r, int g, int b) {
  int bri = cfg_int("led_bri");
  led_strip_set_pixel(strip, 0, r * bri / 255, g * bri / 255, b * bri / 255);
  led_strip_refresh(strip);
}

static void led_task(void *arg) {
  set(0, 0, 255);
  vTaskDelay(pdMS_TO_TICKS(3000));
  for (int tick = 0;; tick++) {
    if (!cfg_bool("setup_done")) set(255, 160, 0);
    else if (netmgr_online()) set(0, 255, 0);
    else if (netmgr_wifi_state() == NET_CAPTIVE) set(0, (tick & 1) ? 200 : 60, 255);
    else set(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(700));
  }
}

void led_start(void) {
  led_strip_config_t sc = {
      .strip_gpio_num = RGB_LED_PIN,
      .max_leds = 1,
      .led_model = LED_MODEL_WS2812,
  };
  led_strip_rmt_config_t rc = {
      .resolution_hz = 10 * 1000 * 1000,
  };
  if (led_strip_new_rmt_device(&sc, &rc, &strip) != ESP_OK) return;
  led_strip_clear(strip);
  xTaskCreate(led_task, "led", 3072, NULL, 1, NULL);
}
