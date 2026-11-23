#define _GNU_SOURCE
#include "mirror.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "devlog.h"
#include "esp_timer.h"
#include "events.h"
#include "httpc.h"
#include "netmgr.h"
#include "portal.h"
#include "recorder.h"
#include "settings.h"
#include "smsstore.h"
#include "textutil.h"
#include "urlutil.h"

static esp_err_t (*check_auth)(httpd_req_t *);
static httpc_session_t *sess;
static char rec_name[48];
static int64_t asked_us, last_use_us;
static unsigned requests, phone_subs, code_subs;

static void session_end(void) {
  if (!sess) return;
  rec_close(sess->rec);
  free(sess);
  sess = NULL;
  devlog("mirror: session ended (%u requests, %u phone and %u code substitutions)", requests, phone_subs, code_subs);
}

static bool session_begin(void) {
  session_end();
  sess = calloc(1, sizeof(httpc_session_t));
  if (!sess) return false;
  httpc_session_init(sess, true);
  sess->via = "mirror";
  char phone[24], local[24], cc[8];
  cfg_get("ph_cc", cc, sizeof(cc));
  if (portal_phone(phone, sizeof(phone), local, sizeof(local))) {
    snprintf(sess->mask_phone[0], sizeof(sess->mask_phone[0]), "%s", phone);
    snprintf(sess->mask_phone[1], sizeof(sess->mask_phone[1]), "%s", local);
    snprintf(sess->mask_phone[2], sizeof(sess->mask_phone[2]), "%s%s", cc, local);
  }
  sess->rec = rec_open("mirror", rec_name, sizeof(rec_name));
  asked_us = esp_timer_get_time();
  requests = phone_subs = code_subs = 0;
  event_post("Assisted login started (recording %s)", rec_name);
  return true;
}

// ---------- rewriting ----------

typedef struct {
  char *b;
  size_t len, cap;
} sb_t;

static void sb_put(sb_t *s, const char *p, size_t n) {
  if (s->len + n + 1 > s->cap) {
    s->cap = (s->len + n + 1) * 2;
    s->b = realloc(s->b, s->cap);
  }
  memcpy(s->b + s->len, p, n);
  s->len += n;
  s->b[s->len] = 0;
}
static void sb_puts(sb_t *s, const char *p) { sb_put(s, p, strlen(p)); }

// Rewrites a URL as the browser would request it, into the mirror's namespace.
static void rw_url(const char *v, size_t vl, const char *scheme, const char *host, sb_t *out) {
  if (vl >= 8 && !strncasecmp(v, "https://", 8)) {
    sb_puts(out, "/p/https/");
    sb_put(out, v + 8, vl - 8);
  } else if (vl >= 7 && !strncasecmp(v, "http://", 7)) {
    sb_puts(out, "/p/http/");
    sb_put(out, v + 7, vl - 7);
  } else if (vl >= 2 && v[0] == '/' && v[1] == '/') {
    sb_puts(out, "/p/");
    sb_puts(out, scheme);
    sb_puts(out, "/");
    sb_put(out, v + 2, vl - 2);
  } else if (vl >= 1 && v[0] == '/' && !(vl >= 3 && !strncmp(v, "/p/", 3))) {
    sb_puts(out, "/p/");
    sb_puts(out, scheme);
    sb_puts(out, "/");
    sb_puts(out, host);
    sb_put(out, v, vl);
  } else {
    sb_put(out, v, vl);
  }
}

static const char SHIM[] =
    "<script>(function(){var S='%s',H='%s';function rw(u){try{u=String(u);if(u.indexOf('/p/')==0)return u;"
    "var m=u.match(/^(https?):\\/\\/(.*)$/i);if(m)return '/p/'+m[1].toLowerCase()+'/'+m[2];"
    "if(u.indexOf('//')==0)return '/p/'+S+'/'+u.slice(2);if(u.charAt(0)=='/')return '/p/'+S+'/'+H+u;}catch(e){}"
    "return u}var f=window.fetch;if(f)window.fetch=function(i,o){if(typeof i=='string')i=rw(i);"
    "else if(i&&i.url)i=new Request(rw(i.url),i);return f.call(this,i,o)};var xo=XMLHttpRequest.prototype.open;"
    "XMLHttpRequest.prototype.open=function(m,u){arguments[1]=rw(u);return xo.apply(this,arguments)};"
    "document.addEventListener('submit',function(e){var a=e.target.getAttribute('action');if(a)"
    "e.target.setAttribute('action',rw(a))},true);var ps=HTMLFormElement.prototype.submit;"
    "HTMLFormElement.prototype.submit=function(){var a=this.getAttribute('action');if(a)this.setAttribute('action',"
    "rw(a));return ps.call(this)};window.addEventListener('DOMContentLoaded',function(){var d=document.createElement"
    "('div');d.style.cssText='position:fixed;left:0;right:0;bottom:0;z-index:2147483647;background:#1f6feb;color:"
    "#fff;font:13px system-ui;padding:6px 10px;text-align:center';d.innerHTML='Through Portal Bridge \\u00b7 "
    "type <b>&lt;phone&gt;</b> or <b>&lt;code&gt;</b> \\u00b7 <a style=\"color:#fff\" href=\"/#portal\">finish"
    "</a>';document.body.appendChild(d)})})();</script>";

static char *rewrite(const char *body, size_t len, bool html, const char *scheme, const char *host) {
  sb_t out = {0};
  out.cap = len + len / 4 + 2048;
  out.b = malloc(out.cap);
  out.b[0] = 0;
  bool injected = !html;
  const char *p = body, *end = body + len;
  while (p < end) {
    if (!injected && *p == '<' && (end - p > 5) && !strncasecmp(p, "<head", 5) && (p[5] == '>' || isspace((unsigned char)p[5]))) {
      const char *gt = memchr(p, '>', (size_t)(end - p));
      if (gt) {
        sb_put(&out, p, (size_t)(gt - p + 1));
        char shim[sizeof(SHIM) + 200];
        snprintf(shim, sizeof(shim), SHIM, scheme, host);
        sb_puts(&out, shim);
        injected = true;
        p = gt + 1;
        continue;
      }
    }
    // Attribute values: href= src= action= formaction=
    if (html && (*p == ' ' || *p == '\t' || *p == '\n')) {
      static const char *const attrs[] = {"href=", "src=", "action=", "formaction=", NULL};
      bool did = false;
      for (int a = 0; attrs[a] && !did; a++) {
        size_t al = strlen(attrs[a]);
        if ((size_t)(end - p - 1) > al && !strncasecmp(p + 1, attrs[a], al) && (p[1 + al] == '"' || p[1 + al] == '\'')) {
          char q = p[1 + al];
          const char *vs = p + 2 + al;
          const char *ve = memchr(vs, q, (size_t)(end - vs));
          if (!ve) break;
          sb_put(&out, p, (size_t)(vs - p));
          rw_url(vs, (size_t)(ve - vs), scheme, host, &out);
          p = ve;
          did = true;
        }
      }
      if (did) continue;
    }
    // Absolute URLs anywhere else (inline scripts, JS files), except XML namespaces.
    if ((*p == 'h' || *p == 'H') && (end - p > 8) &&
        (!strncasecmp(p, "https://", 8) || !strncasecmp(p, "http://", 7)) && strncmp(p + (p[4] == ':' ? 7 : 8), "www.w3.org", 10)) {
      bool tls = p[4] != ':';
      sb_puts(&out, tls ? "/p/https/" : "/p/http/");
      p += tls ? 8 : 7;
      continue;
    }
    sb_put(&out, p, 1);
    p++;
  }
  if (!injected) {
    char shim[sizeof(SHIM) + 200];
    snprintf(shim, sizeof(shim), SHIM, scheme, host);
    sb_t o2 = {0};
    sb_puts(&o2, shim);
    sb_put(&o2, out.b, out.len);
    free(out.b);
    return o2.b;
  }
  return out.b;
}

// ---------- handlers ----------

static const char *status_text(int s) {
  switch (s) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 204: return "204 No Content";
    case 301: return "301 Moved Permanently";
    case 302: return "302 Found";
    case 303: return "303 See Other";
    case 304: return "304 Not Modified";
    case 307: return "307 Temporary Redirect";
    case 308: return "308 Permanent Redirect";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 500: return "500 Internal Server Error";
    default: return s >= 400 ? "502 Bad Gateway" : "200 OK";
  }
}

// Replaces <phone>/<code> placeholders; may wait for the SMS code.
static char *substitute(char *s, bool *failed) {
  int n = 0;
  if (placeholder_present(s, "phone")) {
    char phone[24], local[24];
    if (!portal_phone(phone, sizeof(phone), local, sizeof(local))) {
      *failed = true;
      return s;
    }
    char *r = placeholder_replace(s, "phone", phone, &n);
    free(s);
    s = r;
    phone_subs += (unsigned)n;
    asked_us = esp_timer_get_time();
    devlog("mirror: <phone> substituted %d time(s)", n);
  }
  if (s && placeholder_present(s, "code")) {
    char code[16];
    if (!sms_wait_code(asked_us, cfg_int("otp_wait_s"), code, sizeof(code))) {
      *failed = true;
      return s;
    }
    if (sess) snprintf(sess->mask_code, sizeof(sess->mask_code), "%s", code);
    char *r = placeholder_replace(s, "code", code, &n);
    free(s);
    s = r;
    code_subs += (unsigned)n;
    devlog("mirror: <code> substituted");
  }
  return s;
}

static esp_err_t p_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK) return ESP_OK;
  if (!sess || esp_timer_get_time() - last_use_us > 15LL * 60 * 1000000) session_begin();
  last_use_us = esp_timer_get_time();
  requests++;
  // /p/<scheme>/<host>/<rest>
  const char *u = req->uri + 3;
  char scheme[8] = "";
  size_t sl = strcspn(u, "/");
  if (sl == 0 || sl >= sizeof(scheme)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mirror URL");
  memcpy(scheme, u, sl);
  const char *h = u + sl + (u[sl] ? 1 : 0);
  char host[128] = "";
  size_t hl = strcspn(h, "/?");
  if (!hl || hl >= sizeof(host)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mirror host");
  memcpy(host, h, hl);
  const char *rest = h + hl;
  char *target = malloc(strlen(rest) + 160);
  sprintf(target, "%s://%s%s%s", scheme, host, rest[0] == '/' ? "" : "/", rest);

  // Request body.
  char *body = NULL;
  size_t blen = 0;
  if (req->content_len > 0 && req->content_len < 128 * 1024) {
    body = malloc(req->content_len + 1);
    while (blen < req->content_len) {
      int n = httpd_req_recv(req, body + blen, req->content_len - blen);
      if (n <= 0) break;
      blen += (size_t)n;
    }
    body[blen] = 0;
  }
  bool failed = false;
  target = substitute(target, &failed);
  if (body && !failed) {
    body = substitute(body, &failed);
    blen = body ? strlen(body) : 0;
  }
  if (failed) {
    free(target);
    free(body);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "Portal Bridge: no phone number configured, or no SMS code arrived in time. "
                                   "Check Settings > Phone number and the SMS page, then go back and retry.");
  }
  char ct[96] = "";
  httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
  const char *method = req->method == HTTP_POST ? "POST" : req->method == HTTP_PUT ? "PUT" : "GET";
  httpc_resp_t r = {0};
  httpc_request(sess, method, target, ct[0] ? ct : NULL, body, blen, NULL, 0, &r);
  free(body);

  if (r.status < 0) {
    free(target);
    httpd_resp_set_status(req, "502 Bad Gateway");
    httpd_resp_set_type(req, "text/plain");
    char msg[200];
    snprintf(msg, sizeof(msg), "Portal Bridge could not reach the login page through the upstream Wi-Fi: %s", r.err);
    httpc_resp_free(&r);
    return httpd_resp_sendstr(req, msg);
  }
  httpd_resp_set_status(req, status_text(r.status));
  if (r.content_type[0]) httpd_resp_set_type(req, r.content_type);
  sb_t loc = {0};
  if (r.location[0]) {
    char abs[1024];
    if (url_resolve(target, r.location, abs, sizeof(abs)) == 0) rw_url(abs, strlen(abs), scheme, host, &loc);
    if (loc.b) httpd_resp_set_hdr(req, "Location", loc.b);
  }
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t e;
  bool html = strcasestr(r.content_type, "html") != NULL;
  bool js = strcasestr(r.content_type, "javascript") != NULL || strcasestr(r.content_type, "json") != NULL;
  if (r.body && (html || js)) {
    url_parts_t up;
    url_split(r.final_url, &up);
    char *rb = rewrite(r.body, r.body_len, html, up.scheme, up.host);
    e = httpd_resp_send(req, rb, rb ? HTTPD_RESP_USE_STRLEN : 0);
    free(rb);
  } else {
    e = httpd_resp_send(req, r.body ? r.body : "", (ssize_t)r.body_len);
  }
  free(loc.b);
  free(target);
  httpc_resp_free(&r);
  return e;
}

// /mirror/start?url=<encoded>  -> begins a session and redirects into /p/...
static esp_err_t start_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK) return ESP_OK;
  char q[1200] = "", url[1100] = "";
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "url", url, sizeof(url));
  url_decode_inplace(url);
  if (!url[0]) netmgr_portal_url(url, sizeof(url));
  if (!url[0]) cfg_get("probe_url", url, sizeof(url));
  if (!netmgr_sta_connected()) {
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Not connected to an upstream Wi-Fi yet. Add one under Settings > Upstream Wi-Fi.");
  }
  session_begin();
  last_use_us = esp_timer_get_time();
  sb_t loc = {0};
  url_parts_t up;
  if (url_split(url, &up) == 0) rw_url(url, strlen(url), up.scheme, up.host, &loc);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", loc.b ? loc.b : "/");
  esp_err_t e = httpd_resp_send(req, NULL, 0);
  free(loc.b);
  return e;
}

static esp_err_t stop_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK) return ESP_OK;
  char name[48];
  snprintf(name, sizeof(name), "%s", rec_name);
  session_end();
  netmgr_recheck();
  httpd_resp_set_type(req, "application/json");
  char out[120];
  snprintf(out, sizeof(out), "{\"recording\":\"%s\"}", name);
  return httpd_resp_sendstr(req, out);
}

void mirror_register(httpd_handle_t h, esp_err_t (*auth)(httpd_req_t *)) {
  check_auth = auth;
  httpd_method_t ms[] = {HTTP_GET, HTTP_POST, HTTP_PUT};
  for (int i = 0; i < 3; i++) {
    httpd_uri_t u = {.uri = "/p/*", .method = ms[i], .handler = p_handler};
    httpd_register_uri_handler(h, &u);
  }
  httpd_uri_t s1 = {.uri = "/mirror/start", .method = HTTP_GET, .handler = start_handler};
  httpd_uri_t s2 = {.uri = "/mirror/stop", .method = HTTP_POST, .handler = stop_handler};
  httpd_register_uri_handler(h, &s1);
  httpd_register_uri_handler(h, &s2);
}

void mirror_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "mirror");
  cJSON_AddBoolToObject(o, "active", sess != NULL);
  cJSON_AddStringToObject(o, "recording", rec_name);
  cJSON_AddNumberToObject(o, "requests", requests);
  cJSON_AddNumberToObject(o, "phone_subs", phone_subs);
  cJSON_AddNumberToObject(o, "code_subs", code_subs);
}
