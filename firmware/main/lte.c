#include "lte.h"

#include <stdio.h>
#include <string.h>

#include "devlog.h"
#include "esp_event.h"
#include "esp_netif_ppp.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_modem.h"
#include "settings.h"

// Espressif lists the A7670E (VID 1E0E, PID 9011, modem interface 5) as tested.
static const usb_modem_id_t MODEMS[] = {
    {.match_id = {USB_DEVICE_ID_MATCH_VID_PID, 0x1E0E, 0x9011}, 5, -1, "SIMCOM A7670E"},
    {.match_id = {0}},
};

static volatile bool up;
static volatile bool enabled;
static volatile bool installed;  // usbh_modem_get_netif() is only valid after install
static char ip[16];
static int64_t up_since_us;
static unsigned up_count, lost_count;
static char apn[64];

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (id == IP_EVENT_PPP_GOT_IP) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    up = true;
    up_since_us = esp_timer_get_time();
    up_count++;
    event_post("4G data connected (%s)", ip);
  } else if (id == IP_EVENT_PPP_LOST_IP) {
    if (up) event_post("4G data disconnected");
    up = false;
    ip[0] = 0;
    lost_count++;
  }
}

static void lte_task(void *arg) {
  usbh_cdc_driver_config_t dc = {
      .task_stack_size = 4096,
      .task_priority = configMAX_PRIORITIES - 1,
      .task_coreid = 0,
      .skip_init_usb_host_driver = false,
  };
  esp_err_t e = usbh_cdc_driver_install(&dc);
  if (e != ESP_OK) {
    devlog("lte: USB host driver install failed: %s", esp_err_to_name(e));
    vTaskDelete(NULL);
  }
  usbh_modem_config_t mc = {
      .modem_id_list = MODEMS,
      .at_tx_buffer_size = 512,
      .at_rx_buffer_size = 1024,
  };
  if (apn[0]) {
    mc.pdp.enable = true;
    mc.pdp.cid = 1;
    mc.pdp.type = "IP";
    mc.pdp.apn = apn;
  }
  e = usbh_modem_install(&mc);
  installed = (e == ESP_OK);
  devlog("lte: usb modem driver installed (%s), APN %s", esp_err_to_name(e), apn[0] ? apn : "(modem default)");
  // Report once if the modem never shows up on USB.
  vTaskDelay(pdMS_TO_TICKS(60000));
  if (!up)
    event_post("4G data not up after 60 s. Check the USB DIP switch routes the modem to the ESP32, the SIM has "
               "data, and the APN.");
  vTaskDelete(NULL);
}

void lte_start(void) {
  enabled = cfg_bool("lte_en");
  if (!enabled) {
    devlog("lte: disabled in settings");
    return;
  }
  cfg_get("apn", apn, sizeof(apn));
  esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_ip, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, on_ip, NULL);
  xTaskCreatePinnedToCore(lte_task, "lte", 6144, NULL, 5, NULL, 0);
}

bool lte_up(void) { return up; }

esp_netif_t *lte_netif(void) { return installed ? usbh_modem_get_netif() : NULL; }

void lte_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "lte");
  cJSON_AddBoolToObject(o, "enabled", enabled);
  cJSON_AddBoolToObject(o, "up", up);
  cJSON_AddStringToObject(o, "ip", ip);
  cJSON_AddNumberToObject(o, "up_s", up ? (double)((esp_timer_get_time() - up_since_us) / 1000000) : 0);
  cJSON_AddNumberToObject(o, "connects", up_count);
  cJSON_AddNumberToObject(o, "drops", lost_count);
}
