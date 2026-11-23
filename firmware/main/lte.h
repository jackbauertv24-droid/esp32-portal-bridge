// 4G data: PPP over the modem's USB port (Espressif iot_usbh_modem).
#pragma once
#include <stdbool.h>

#include "cJSON.h"
#include "esp_netif.h"

void lte_start(void);
bool lte_up(void);
esp_netif_t *lte_netif(void);
void lte_status_json(cJSON *root);
