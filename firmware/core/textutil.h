// Small text helpers shared by the portal engine, the mirror and the SMS code.
// Plain C, no ESP-IDF dependency, unit-tested on the host (tests/test_core.c).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reduces a number as typed or as read from the SIM to its local part:
// strips spaces/dashes, then a leading "+<cc>", "00<cc>" or "<cc>" (the last only
// when the remainder is still long enough to be a subscriber number).
void phone_local_part(const char *raw, const char *cc, char *out, size_t cap);

// Formats a local number for a web form. `fmt` is one of:
//   "local" -> 91234567, "plus_cc" -> +85291234567, "cc" -> 85291234567, "00cc" -> 0085291234567
void phone_format(const char *cc, const char *local, const char *fmt, char *out, size_t cap);

// Finds a one-time code: the first run of digits of length min..max that is not
// part of a longer digit run. Returns 1 and writes it to `out` if found.
int otp_extract(const char *text, int min_len, int max_len, char *out, size_t cap);

// Case-insensitive substring test.
int str_contains_ci(const char *hay, const char *needle);

// Same phone number, ignoring formatting and country code: the last 8 digits
// match (and there are at least 6).
int phone_same(const char *a, const char *b);
// Parses an HTTP Date header ("Tue, 24 Nov 2026 15:42:54 GMT") to Unix
// seconds; -1 if it is not one.
long long http_date_parse(const char *s);
// `num` matches one entry of a comma/semicolon/space separated list.
int phone_in_list(const char *num, const char *list);

// Replaces every spelling of the placeholder <name> in `in`: raw "<name>",
// URL-encoded "%3Cname%3E", JSON "<name>" and HTML "&lt;name&gt;".
// Returns a malloc'd string (caller frees) and stores the number of
// replacements in *count. Returns NULL only when out of memory.
char *placeholder_replace(const char *in, const char *name, const char *value, int *count);

// Reports whether `in` contains any spelling of <name>.
int placeholder_present(const char *in, const char *name);

// application/x-www-form-urlencoded encoding of one component.
void url_encode(const char *in, char *out, size_t cap);
// Decodes %XX and '+' in place.
void url_decode_inplace(char *s);

#ifdef __cplusplus
}
#endif
