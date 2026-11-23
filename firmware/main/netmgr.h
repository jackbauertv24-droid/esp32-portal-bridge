// Network manager: the hotspot (always on), the upstream Wi-Fi client, the
// uplink choice between Wi-Fi and 4G, NAT, internet checks and login-page
// detection.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

typedef enum { NET_UNKNOWN = 0, NET_ONLINE, NET_CAPTIVE, NET_OFFLINE } net_state_t;

void netmgr_start(void);
// Interface name of the upstream Wi-Fi client, for binding sockets to it.
bool netmgr_sta_ifname(char *out, size_t cap);
bool netmgr_sta_connected(void);
// Result of the last internet check through the upstream Wi-Fi.
net_state_t netmgr_wifi_state(void);
// Login page URL found by the last check ("" if none).
void netmgr_portal_url(char *out, size_t cap);
// Is there internet through the selected uplink?
bool netmgr_online(void);
// Ask for an immediate re-check (e.g. after a login attempt).
void netmgr_recheck(void);
// Re-read the upstream Wi-Fi list and reconnect.
void netmgr_reconnect(void);
void netmgr_status_json(cJSON *root);
void netmgr_summary(char *out, size_t cap);
// Current hotspot gateway address, e.g. "192.168.0.1".
void netmgr_ap_ip(char *out, size_t cap);
