// Status snapshot (for the admin page) and the diagnostics report (for sending
// to GitHub/Telegram or downloading). The report masks long numbers (phone,
// IMEI, ICCID, IMSI, chat ids) and never includes SMS text or secrets.
#pragma once
#include "cJSON.h"

cJSON *status_json(void);
// malloc'd, NUL-terminated text.
char *report_build(void);
// Latest recording, masked, malloc'd ("" if none). `name` receives its name.
char *report_latest_recording(char *name, size_t cap);
