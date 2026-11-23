// URL splitting and relative-reference resolution (RFC 3986, simplified).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char scheme[8];   // "http" / "https"
  char host[128];   // host[:port]
  char path[1024];  // starts with '/', includes the query
} url_parts_t;

// Returns 0 on success, -1 if `url` is not an absolute http(s) URL.
int url_split(const char *url, url_parts_t *out);

// Resolves `ref` (absolute, scheme-relative "//h/p", root-relative "/p",
// relative "p", query-only "?q" or empty) against absolute `base`.
// Returns 0 on success.
int url_resolve(const char *base, const char *ref, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
