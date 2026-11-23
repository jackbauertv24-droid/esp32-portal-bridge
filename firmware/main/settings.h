// Every setting the device has, declared once in settings.c. The admin page
// forms, JSON backup/restore and the diagnostics report are generated from that
// table, so nothing is configured anywhere else.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

void cfg_init(void);

// Copies the current value of `key` into `out` (always NUL-terminated).
void cfg_get(const char *key, char *out, size_t cap);
int cfg_int(const char *key);
bool cfg_bool(const char *key);
// True if `key` equals `value`.
bool cfg_is(const char *key, const char *value);

// Validates and stores (in RAM) one value. Returns ESP_OK or an error with a
// message in `err`. For secrets, the masked value "********" means "unchanged".
esp_err_t cfg_set(const char *key, const char *value, char *err, size_t errcap);
// Writes all values to flash.
esp_err_t cfg_commit(void);
// Restores every setting to its default (in RAM; call cfg_commit to persist).
void cfg_reset_all(void);

// Schema for the admin page: [{key, group, label, help, type, def, min, max, options, reboot}]
cJSON *cfg_schema_json(void);
// Current values {key: value}; secrets are masked unless include_secrets.
cJSON *cfg_values_json(bool include_secrets);
// Applies {key: value} from a form or a backup; stops at the first invalid
// value. `*reboot` is set when any changed key needs a restart.
esp_err_t cfg_apply_json(const cJSON *obj, bool *reboot, char *err, size_t errcap);

// Hotspot SSID default depends on the MAC address, so it is filled in at boot.
void cfg_set_runtime_default(const char *key, const char *value);

#define CFG_SECRET_MASK "********"
