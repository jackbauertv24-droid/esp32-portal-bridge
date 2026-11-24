// Speed test: latency, download and upload through one uplink (Wi-Fi or 4G),
// measured on the board, with a history kept in flash.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

// via: "wifi", "4g" or "both". Returns false (with a reason) if a test is
// already running or the uplink is not available.
bool speedtest_start(const char *via, char *err, size_t errcap);
void speedtest_stop(void);
// {running, phase, via, live_mbps, bytes, history:[...]}
cJSON *speedtest_json(void);
void speedtest_clear(void);
