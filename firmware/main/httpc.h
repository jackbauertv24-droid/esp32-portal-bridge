// Browser-like HTTP client for login pages: cookie jar, manual redirects,
// optional binding to the upstream Wi-Fi interface, and recording of every
// request/response to a recording file with the phone number and code masked.
#pragma once
#include <stdbool.h>
#include <stdio.h>

#include <net/if.h>

#define HTTPC_MAX_COOKIES 32
#define HTTPC_BODY_CAP (256 * 1024)

typedef struct {
  char host[96];
  char name[64];
  char value[600];
} httpc_cookie_t;

typedef struct {
  httpc_cookie_t cookies[HTTPC_MAX_COOKIES];
  int n_cookies;
  bool bind;               // send through `ifr` instead of the default route
  struct ifreq ifr;
  bool verify_tls;
  char ua[200];
  FILE *rec;               // recording file or NULL
  const char *via;         // label written into each record
  bool quiet;              // no log line per request (internet checks)
  int seq;
  char mask_phone[3][24];  // strings replaced by {phone} in recordings
  char mask_code[16];      // replaced by {code}
} httpc_session_t;

typedef struct {
  int status;              // HTTP status, or -1 on connection error
  char content_type[96];
  char location[1024];     // raw Location header of the last response
  char final_url[1024];    // URL that produced `body`
  char *body;              // malloc'd, NUL-terminated (may be NULL)
  size_t body_len;
  char err[96];
  char date[40];           // Date header of the last response ("" if none)
} httpc_resp_t;

// Initialises a session from the settings (user agent, TLS verification).
// `bind_sta` routes it through the upstream Wi-Fi interface.
void httpc_session_init(httpc_session_t *s, bool bind_sta);

// Performs a request, following up to `max_redirects` redirects.
// `extra_headers` is "Name: value\r\n..." or NULL. Free resp->body afterwards.
void httpc_request(httpc_session_t *s, const char *method, const char *url, const char *content_type,
                   const char *body, size_t body_len, const char *extra_headers, int max_redirects,
                   httpc_resp_t *resp);

void httpc_resp_free(httpc_resp_t *r);

// Copies the cookie header value for `url` into out.
void httpc_cookie_header(httpc_session_t *s, const char *url, char *out, size_t cap);
