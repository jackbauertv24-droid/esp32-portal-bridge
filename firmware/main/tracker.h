// GPS + signal tracker: logs position (GNSS) and 4G radio readings at an
// interval to daily files in flash, and answers "where are you" SMS from
// allowed numbers with a map link.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

void tracker_start(void);
// Called for every saved incoming SMS; only queues work, never blocks.
void tracker_on_sms(const char *from, const char *text);
cJSON *tracker_json(void);
// Full path of a day's file ("/data/trk_YYYYMMDD.csv"), false if the name is invalid.
bool tracker_day_path(const char *day, char *out, size_t cap);
bool tracker_delete_day(const char *day);
void tracker_log_now(void);
void tracker_summary(char *out, size_t cap);
