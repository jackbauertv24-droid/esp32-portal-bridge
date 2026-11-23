// Masks identifiers (phone numbers, IMEI, ICCID, IMSI) in text that may leave
// the device, such as the diagnostics report attached to a public GitHub issue.
// Plain C so it can be tested on a PC.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// In place: every run of 7 or more digits keeps its last 3 digits and has the
// rest replaced by '*'. Shorter runs (times, counts, signal levels) are kept.
void redact_long_numbers(char *s);

#ifdef __cplusplus
}
#endif
