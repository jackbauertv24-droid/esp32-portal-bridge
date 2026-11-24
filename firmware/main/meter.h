// Data meter and budget control. Counts every byte through the 4G and Wi-Fi
// uplinks at the IP layer (hotspot clients and the board itself), keeps daily
// and billing-period totals in flash, estimates cost, raises alerts and can
// stop the hotspot from using 4G once a limit is reached.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_netif.h"

void meter_start(void);
// Tells the meter which interfaces are the uplinks (called every network cycle).
void meter_set_uplinks(esp_netif_t *wifi, esp_netif_t *lte);
// True while a 4G limit is reached, the action is "stop", and no override is active.
bool meter_lte_blocked(void);
// Lets the hotspot use 4G again until local midnight (or 6 h without a clock).
void meter_allow_until_midnight(void);
// Clears counters: "period" (current billing period and today) or "all".
void meter_reset(const char *scope);
void meter_status_json(cJSON *root);
void meter_summary(char *out, size_t cap);
