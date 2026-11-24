// Voice memos: callers who do not enter the PIN can leave a message. The
// modem records it; this module copies it to the board's flash and delivers
// it (Telegram and/or webhook), retrying until it gets through.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

void memo_start(void);
// Queues a finished recording on the modem for copying and delivery.
void memo_add(const char *modem_path, const char *number, int secs);
cJSON *memo_json(void);
bool memo_file(uint32_t id, char *path, size_t cap);
bool memo_delete(uint32_t id);
bool memo_resend(uint32_t id);
void memo_summary(char *out, size_t cap);
