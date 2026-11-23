// Device log: every line goes to the USB serial port and into a RAM ring that
// the admin page shows and the diagnostics report includes.
#pragma once
#include <Arduino.h>

void devlog_begin();
void devlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Appends the whole ring, oldest first, to `out`.
void devlog_dump(String &out);
