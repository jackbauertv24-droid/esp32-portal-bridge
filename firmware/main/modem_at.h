// A7670E control channel over UART (GPIO17/18). Used for status, SIM and SMS;
// 4G data runs separately over USB (lte.c), so this channel stays free.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cJSON.h"

void modem_uart_start(void);

// Sends one AT command and returns all response lines (malloc'd, caller frees),
// joined by '\n', with the final ERROR line included on failure. `ok` reports
// whether the final result was OK.
char *modem_cmd(const char *cmd, int timeout_ms, bool *ok);

bool modem_alive(void);
// Copies a file from the modem's flash (e.g. "C:/memo.wav") into `out`,
// in chunks so other modem work can run in between. Returns bytes copied or
// -1 with a reason in err.
long modem_file_read(const char *path, FILE *out, char *err, size_t errcap);
// Deletes a file on the modem's flash (errors ignored).
void modem_file_delete(const char *path);
// Latest radio readings from the periodic status refresh: CSQ (99 = unknown),
// operator name and the raw AT+CPSI line (any pointer may be NULL).
void modem_radio(int *csq, char *oper, size_t ocap, char *cpsi, size_t ccap);
// Own number as read from the SIM (+CNUM), "" if unknown.
void modem_own_number(char *out, size_t cap);
void modem_status_json(cJSON *root);
// Short summary for the report.
void modem_summary(char *out, size_t cap);

// Sends an SMS (split into parts if long). Returns the number of parts sent,
// or a negative value with the reason in `err`.
int modem_send_sms(const char *to, const char *text, char *err, size_t errcap);
