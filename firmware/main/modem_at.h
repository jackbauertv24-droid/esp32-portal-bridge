// A7670E control channel over UART (GPIO17/18). Used for status, SIM and SMS;
// 4G data runs separately over USB (lte.c), so this channel stays free.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

void modem_uart_start(void);

// Sends one AT command and returns all response lines (malloc'd, caller frees),
// joined by '\n', with the final ERROR line included on failure. `ok` reports
// whether the final result was OK.
char *modem_cmd(const char *cmd, int timeout_ms, bool *ok);

bool modem_alive(void);
// Own number as read from the SIM (+CNUM), "" if unknown.
void modem_own_number(char *out, size_t cap);
void modem_status_json(cJSON *root);
// Short summary for the report.
void modem_summary(char *out, size_t cap);
