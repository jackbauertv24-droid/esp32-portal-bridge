// Portal Bridge: 4G hotspot that can also share an upstream Wi-Fi, logs into
// SMS-verified login pages with its own SIM, and forwards every SMS.
// Waveshare ESP32-S3-A7670E-4G, ESP-IDF.
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "build_info.h"
#include "calls.h"
#include "devices.h"
#include "devlog.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "events.h"
#include "forward.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "lte.h"
#include "meter.h"
#include "modem_at.h"
#include "netmgr.h"
#include "nvs_flash.h"
#include "portal.h"
#include "settings.h"
#include "smsstore.h"
#include "tracker.h"
#include "web.h"

void app_main(void) {
  devlog_init();
  devlog("=== portal-bridge %s commit %s built %s ===", FW_VERSION, FW_COMMIT, FW_BUILT);

  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_vfs_spiffs_conf_t fs = {.base_path = "/data", .partition_label = "storage", .max_files = 8,
                              .format_if_mount_failed = true};
  e = esp_vfs_spiffs_register(&fs);
  devlog("storage: %s", esp_err_to_name(e));

  cfg_init();
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char ssid[33];
  snprintf(ssid, sizeof(ssid), "PortalBridge-%02X%02X", mac[4], mac[5]);
  cfg_set_runtime_default("ap_ssid", ssid);

  events_init();
  meter_start();
  smsstore_init();
  battery_init();
  led_start();
  netmgr_start();
  devices_start();
  web_start();
  modem_uart_start();
  lte_start();
  portal_start();
  forward_start();
  tracker_start();
  calls_start();
  event_post("Started %s%s", FW_VERSION, cfg_bool("setup_done") ? "" : " (first-run setup pending)");

  // Keep this firmware only if it survives a minute; otherwise the bootloader
  // returns to the previous one on the next reset.
  vTaskDelay(pdMS_TO_TICKS(60000));
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    devlog("ota: new firmware confirmed");
  }
}
