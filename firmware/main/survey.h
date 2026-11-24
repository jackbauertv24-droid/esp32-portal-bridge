// Wi-Fi survey: a full scan of nearby 2.4 GHz networks with the details the
// admin page needs to compare them and pick a channel.
#pragma once
#include "cJSON.h"

// Scans (about 2 s; hotspot devices pause briefly) and returns
// {networks:[...], upstream_bssid, ap_channel, saved:[ssid,...]} or {error}.
cJSON *survey_json(void);
