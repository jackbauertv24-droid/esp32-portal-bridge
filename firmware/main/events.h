// Device events: notable things that happened (login result, uplink change,
// restart). Kept in a short RAM list for the admin page and, if enabled,
// forwarded like SMS.
#pragma once
#include "cJSON.h"

void events_init(void);
void event_post(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Like event_post, but `detail` (e.g. a device name) is appended only to the
// forwarded message, never to the device log, the admin list or reports.
void event_post_detail(const char *detail, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
cJSON *events_json(void);
// Takes the oldest not-yet-forwarded event text into `out`; returns 0 if none.
int events_next_unsent(char *out, int cap, int *id);
void events_mark_sent(int id);
