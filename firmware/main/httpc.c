#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "httpc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "devlog.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "netmgr.h"
#include "settings.h"
#include "urlutil.h"

typedef struct {
  httpc_session_t *s;
  const char *host;
  httpc_resp_t *resp;
} ev_ctx_t;

static void host_of(const char *url, char *out, size_t cap) {
  url_parts_t u;
  out[0] = 0;
  if (url_split(url, &u) == 0) {
    snprintf(out, cap, "%s", u.host);
    char *colon = strchr(out, ':');
    if (colon) *colon = 0;
  }
}

static bool host_match(const char *cookie_host, const char *host) {
  size_t cl = strlen(cookie_host), hl = strlen(host);
  if (cl > hl) return false;
  if (strcasecmp(host + hl - cl, cookie_host)) return false;
  return cl == hl || host[hl - cl - 1] == '.';
}

static void store_cookie(httpc_session_t *s, const char *host, const char *hdr) {
  char name[64], value[600], domain[96];
  size_t nl = strcspn(hdr, "=;");
  if (hdr[nl] != '=' || nl == 0 || nl >= sizeof(name)) return;
  memcpy(name, hdr, nl);
  name[nl] = 0;
  const char *v = hdr + nl + 1;
  size_t vl = strcspn(v, ";");
  if (vl >= sizeof(value)) vl = sizeof(value) - 1;
  memcpy(value, v, vl);
  value[vl] = 0;
  snprintf(domain, sizeof(domain), "%s", host);
  const char *d = strcasestr(hdr, "domain=");
  if (d) {
    d += 7;
    while (*d == '.') d++;
    size_t dl = strcspn(d, "; ");
    if (dl && dl < sizeof(domain)) {
      memcpy(domain, d, dl);
      domain[dl] = 0;
    }
  }
  bool expired = strcasestr(hdr, "max-age=0") != NULL;
  for (int i = 0; i < s->n_cookies; i++) {
    if (!strcmp(s->cookies[i].name, name) && !strcasecmp(s->cookies[i].host, domain)) {
      if (expired) {
        s->cookies[i] = s->cookies[--s->n_cookies];
      } else {
        snprintf(s->cookies[i].value, sizeof(s->cookies[i].value), "%s", value);
      }
      return;
    }
  }
  if (expired || s->n_cookies >= HTTPC_MAX_COOKIES) return;
  httpc_cookie_t *c = &s->cookies[s->n_cookies++];
  snprintf(c->host, sizeof(c->host), "%s", domain);
  snprintf(c->name, sizeof(c->name), "%s", name);
  snprintf(c->value, sizeof(c->value), "%s", value);
}

void httpc_cookie_header(httpc_session_t *s, const char *url, char *out, size_t cap) {
  char host[96];
  host_of(url, host, sizeof(host));
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < s->n_cookies; i++) {
    if (!host_match(s->cookies[i].host, host)) continue;
    int n = snprintf(out + o, cap - o, "%s%s=%s", o ? "; " : "", s->cookies[i].name, s->cookies[i].value);
    if (n < 0 || (size_t)n >= cap - o) break;
    o += (size_t)n;
  }
}

static esp_err_t on_event(esp_http_client_event_t *e) {
  ev_ctx_t *c = (ev_ctx_t *)e->user_data;
  if (e->event_id != HTTP_EVENT_ON_HEADER || !c) return ESP_OK;
  if (!strcasecmp(e->header_key, "Set-Cookie")) store_cookie(c->s, c->host, e->header_value);
  else if (!strcasecmp(e->header_key, "Location"))
    snprintf(c->resp->location, sizeof(c->resp->location), "%s", e->header_value);
  else if (!strcasecmp(e->header_key, "Content-Type"))
    snprintf(c->resp->content_type, sizeof(c->resp->content_type), "%s", e->header_value);
  return ESP_OK;
}

void httpc_session_init(httpc_session_t *s, bool bind_sta) {
  memset(s, 0, sizeof(*s));
  cfg_get("po_ua", s->ua, sizeof(s->ua));
  s->verify_tls = cfg_bool("po_tls");
  s->via = "auto";
  if (bind_sta) {
    char name[8];
    if (netmgr_sta_ifname(name, sizeof(name))) {
      s->bind = true;
      snprintf(s->ifr.ifr_name, sizeof(s->ifr.ifr_name), "%s", name);
    }
  }
}

void httpc_resp_free(httpc_resp_t *r) {
  free(r->body);
  r->body = NULL;
}

static char *mask(httpc_session_t *s, const char *in, size_t max) {
  size_t n = strlen(in);
  if (n > max) n = max;
  char *t = strndup(in, n);
  if (!t) return NULL;
  struct {
    const char *find;
    const char *repl;
  } subs[4] = {{s->mask_phone[0], "{phone}"}, {s->mask_phone[1], "{phone}"}, {s->mask_phone[2], "{phone}"},
               {s->mask_code, "{code}"}};
  for (int k = 0; k < 4; k++) {
    if (!subs[k].find[0] || strlen(subs[k].find) < 4) continue;
    char *p;
    while ((p = strstr(t, subs[k].find))) {
      size_t fl = strlen(subs[k].find), rl = strlen(subs[k].repl), tl = strlen(t);
      char *u = malloc(tl - fl + rl + 1);
      if (!u) break;
      memcpy(u, t, (size_t)(p - t));
      memcpy(u + (p - t), subs[k].repl, rl);
      strcpy(u + (p - t) + rl, p + fl);
      free(t);
      t = u;
    }
  }
  return t;
}

static bool is_text(const char *ct) {
  return !ct[0] || strcasestr(ct, "text") || strcasestr(ct, "json") || strcasestr(ct, "javascript") ||
         strcasestr(ct, "xml") || strcasestr(ct, "form");
}

static void record(httpc_session_t *s, const char *method, const char *url, const char *req_ct, const char *body,
                   size_t body_len, const httpc_resp_t *r) {
  if (!s->rec) return;
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "t", (double)time(NULL));
  cJSON_AddStringToObject(o, "via", s->via ? s->via : "");
  cJSON_AddNumberToObject(o, "n", ++s->seq);
  cJSON_AddStringToObject(o, "method", method);
  char *mu = mask(s, url, 2048);
  cJSON_AddStringToObject(o, "url", mu ? mu : "");
  free(mu);
  if (req_ct) cJSON_AddStringToObject(o, "req_ct", req_ct);
  if (body && body_len) {
    char *tmp = strndup(body, body_len);
    char *mb = tmp ? mask(s, tmp, 8192) : NULL;
    cJSON_AddStringToObject(o, "req_body", mb ? mb : "");
    free(mb);
    free(tmp);
  }
  cJSON_AddNumberToObject(o, "status", r->status);
  if (r->err[0]) cJSON_AddStringToObject(o, "err", r->err);
  cJSON_AddStringToObject(o, "resp_ct", r->content_type);
  if (r->location[0]) cJSON_AddStringToObject(o, "location", r->location);
  cJSON_AddNumberToObject(o, "resp_len", (double)r->body_len);
  if (r->body && is_text(r->content_type)) {
    char *mb = mask(s, r->body, 48 * 1024);
    cJSON_AddStringToObject(o, "resp_body", mb ? mb : "");
    free(mb);
  }
  char *line = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  if (line) {
    fputs(line, s->rec);
    fputc('\n', s->rec);
    fflush(s->rec);
    free(line);
  }
}

// One request without following redirects.
static void one(httpc_session_t *s, const char *method, const char *url, const char *ct, const char *body,
                size_t body_len, const char *extra, httpc_resp_t *r) {
  char host[96];
  host_of(url, host, sizeof(host));
  ev_ctx_t ctx = {s, host, r};
  r->status = -1;
  r->content_type[0] = r->location[0] = r->err[0] = 0;
  r->body = NULL;
  r->body_len = 0;
  snprintf(r->final_url, sizeof(r->final_url), "%s", url);

  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = 20000,
      .disable_auto_redirect = true,
      .event_handler = on_event,
      .user_data = &ctx,
      .buffer_size = 4096,
      .buffer_size_tx = 2048,
      .user_agent = s->ua,
      .keep_alive_enable = false,
  };
  if (!strncasecmp(url, "https:", 6) && s->verify_tls) cfg.crt_bundle_attach = esp_crt_bundle_attach;
  if (s->bind) cfg.if_name = &s->ifr;
  if (!strcasecmp(method, "POST")) cfg.method = HTTP_METHOD_POST;
  else if (!strcasecmp(method, "HEAD")) cfg.method = HTTP_METHOD_HEAD;
  else if (!strcasecmp(method, "PUT")) cfg.method = HTTP_METHOD_PUT;
  else cfg.method = HTTP_METHOD_GET;

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    snprintf(r->err, sizeof(r->err), "client init failed");
    return;
  }
  char cookie[1536];
  httpc_cookie_header(s, url, cookie, sizeof(cookie));
  if (cookie[0]) esp_http_client_set_header(c, "Cookie", cookie);
  esp_http_client_set_header(c, "Accept", "text/html,application/xhtml+xml,application/json,*/*;q=0.8");
  esp_http_client_set_header(c, "Accept-Language", "en-US,en;q=0.9,zh-HK;q=0.8");
  if (ct) esp_http_client_set_header(c, "Content-Type", ct);
  if (extra) {
    // "Name: value\r\n" pairs
    char *copy = strdup(extra), *save = NULL;
    for (char *ln = strtok_r(copy, "\r\n", &save); ln; ln = strtok_r(NULL, "\r\n", &save)) {
      char *colon = strchr(ln, ':');
      if (!colon) continue;
      *colon = 0;
      char *v = colon + 1;
      while (*v == ' ') v++;
      esp_http_client_set_header(c, ln, v);
    }
    free(copy);
  }
  esp_err_t err = esp_http_client_open(c, (int)body_len);
  if (err == ESP_OK && body_len) {
    int w = esp_http_client_write(c, body, (int)body_len);
    if (w < 0) err = ESP_FAIL;
  }
  if (err == ESP_OK) {
    esp_http_client_fetch_headers(c);
    r->status = esp_http_client_get_status_code(c);
    size_t cap = 8192;
    r->body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->body) r->body = malloc(cap);
    for (;;) {
      if (r->body_len + 2048 + 1 > cap) {
        if (cap >= HTTPC_BODY_CAP) break;
        cap *= 2;
        char *nb = heap_caps_realloc(r->body, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) break;
        r->body = nb;
      }
      int n = esp_http_client_read(c, r->body + r->body_len, 2048);
      if (n <= 0) break;
      r->body_len += (size_t)n;
    }
    if (r->body) r->body[r->body_len] = 0;
  } else {
    snprintf(r->err, sizeof(r->err), "%s", esp_err_to_name(err));
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  if (!s->quiet)
    devlog("http: %s %.120s -> %d %s (%u bytes)%s%s", method, url, r->status, r->content_type,
         (unsigned)r->body_len, r->err[0] ? " " : "", r->err);
  record(s, method, url, ct, body, body_len, r);
}

void httpc_request(httpc_session_t *s, const char *method, const char *url, const char *content_type,
                   const char *body, size_t body_len, const char *extra_headers, int max_redirects,
                   httpc_resp_t *resp) {
  char cur[1024], meth[8];
  snprintf(cur, sizeof(cur), "%s", url);
  snprintf(meth, sizeof(meth), "%s", method);
  const char *b = body, *ct = content_type;
  size_t bl = body_len;
  for (int hop = 0;; hop++) {
    one(s, meth, cur, ct, b, bl, extra_headers, resp);
    bool redirect = resp->status == 301 || resp->status == 302 || resp->status == 303 || resp->status == 307 ||
                    resp->status == 308;
    if (!redirect || !resp->location[0] || hop >= max_redirects) return;
    char next[1024];
    if (url_resolve(cur, resp->location, next, sizeof(next))) return;
    if (resp->status != 307 && resp->status != 308) {
      snprintf(meth, sizeof(meth), "GET");
      b = NULL;
      bl = 0;
      ct = NULL;
    }
    snprintf(cur, sizeof(cur), "%s", next);
    httpc_resp_free(resp);
  }
}
