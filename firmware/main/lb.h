// Load balancer: with both uplinks online, spreads hotspot traffic over Wi-Fi
// and 4G per (device, destination) pair, so one site always sees the same
// address while different sites and devices use both links.
#pragma once
#include "cJSON.h"
#include "esp_netif.h"

// Called by the network manager every cycle with the current state.
void lb_update(esp_netif_t *wifi, bool wifi_online, esp_netif_t *lte, bool lte_online, const char *ap_ip);
// The DNS server handed to hotspot clients and the interface it belongs to;
// queries to it are never balanced.
void lb_set_dns(uint32_t dns_addr);
void lb_status_json(cJSON *root);
