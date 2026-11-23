// Device log. Captures every ESP-IDF log line (all components) plus our own
// lines into a RAM ring that the admin page shows and reports include, while
// still printing to the USB serial console.
#pragma once
#include <stddef.h>

void devlog_init(void);
void devlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Copies the ring (oldest first) into a new malloc'd, NUL-terminated buffer.
char *devlog_dump(size_t *len);
