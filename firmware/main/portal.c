#define _GNU_SOURCE
#include "portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "devlog.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "html_form.h"
#include "httpc.h"
#include "modem_at.h"
#include "netmgr.h"
#include "recorder.h"
#include "settings.h"
#include "smsstore.h"
#include "textutil.h"
#include "urlutil.h"

#define RECIPE_FILE "/data/recipe.json"

static volatile bool trigger;
static char state[48] = "idle";
static char last_result[200] = "";
static char last_rec[48] = "";
static time_t last_attempt_t;
static int64_t last_attempt_us;
static unsigned attempts, successes;

static void set_state(const char *s) {
  snprintf(state, sizeof(state), "%s", s);
  devlog("portal: %s", s);
}

bool portal_phone(char *formatted, size_t cap, char *local, size_t lcap) {
  char cc[8], fmt[12], raw[24];
  cfg_get("ph_cc", cc, sizeof(cc));
  cfg_get("ph_fmt", fmt, sizeof(fmt));
  cfg_get("ph_local", raw, sizeof(raw));
  if (!raw[0]) modem_own_number(raw, sizeof(raw));
  if (!raw[0]) return false;
  phone_local_part(raw, cc, local, lcap);
  phone_format(cc, local, fmt, formatted, cap);
  return local[0] != 0;
}

// ---------- form helpers ----------

static bool agree_like(const hf_input_t *in) {
  static const char *const kw[] = {"agree", "accept", "term", "consent", "policy", "tnc", "同意", "条款", "條款", NULL};
  for (int i = 0; kw[i]; i++)
    if (str_contains_ci(in->name, kw[i]) || str_contains_ci(in->id, kw[i]) || str_contains_ci(in->hint, kw[i]))
      return true;
  return false;
}

static void append_pair(char **buf, size_t *len, size_t *cap, const char *k, const char *v) {
  char ek[200], ev[700];
  url_encode(k, ek, sizeof(ek));
  url_encode(v, ev, sizeof(ev));
  size_t need = *len + strlen(ek) + strlen(ev) + 3;
  if (need > *cap) {
    *cap = need * 2;
    *buf = realloc(*buf, *cap);
  }
  *len += (size_t)sprintf(*buf + *len, "%s%s=%s", *len ? "&" : "", ek, ev);
}

// Builds a urlencoded body for `f`, putting `phone` and `code` into their fields.
static char *build_body(const hf_form_t *f, int phone_i, const char *phone, int code_i, const char *code) {
  size_t cap = 512, len = 0;
  char *b = malloc(cap);
  b[0] = 0;
  bool submit_added = false;
  for (int i = 0; i < f->n_inputs; i++) {
    const hf_input_t *in = &f->inputs[i];
    if (!in->name[0]) continue;
    const char *v = in->value;
    if (i == phone_i) v = phone;
    else if (i == code_i) v = code ? code : "";
    else if (!strcmp(in->type, "checkbox")) {
      if (!in->checked && !agree_like(in)) continue;
      if (!v[0]) v = "on";
    } else if (!strcmp(in->type, "radio")) {
      if (!in->checked) continue;
    } else if (!strcmp(in->type, "submit") || !strcmp(in->type, "image")) {
      if (submit_added) continue;
      submit_added = true;
    } else if (!strcmp(in->type, "button") || !strcmp(in->type, "reset") || !strcmp(in->type, "file")) {
      continue;
    }
    append_pair(&b, &len, &cap, in->name, v);
  }
  return b;
}

// Submits form `f` found on page `base`. Returns the response in r.
static void submit(httpc_session_t *s, const char *base, const hf_form_t *f, const char *body, httpc_resp_t *r) {
  char action[1024];
  if (url_resolve(base, f->action, action, sizeof(action))) snprintf(action, sizeof(action), "%s", base);
  if (!strcmp(f->method, "get")) {
    char *u = malloc(strlen(action) + strlen(body) + 2);
    action[strcspn(action, "?")] = 0;
    sprintf(u, "%s?%s", action, body);
    httpc_request(s, "GET", u, NULL, NULL, 0, NULL, 6, r);
    free(u);
  } else {
    httpc_request(s, "POST", action, "application/x-www-form-urlencoded", body, strlen(body), NULL, 6, r);
  }
}

static int best_form(hf_form_t *forms, int n, int (*finder)(const hf_form_t *), int *field) {
  for (int i = 0; i < n; i++) {
    int k = finder(&forms[i]);
    if (k >= 0) {
      *field = k;
      return i;
    }
  }
  *field = -1;
  return -1;
}

// ---------- method: form ----------

static bool run_form(httpc_session_t *s, const char *portal, const char *phone, char *why, size_t wcap) {
  hf_form_t *forms = calloc(HF_MAX_FORMS, sizeof(hf_form_t));
  httpc_resp_t r = {0};
  bool ok = false;
  char base[1024];
  httpc_request(s, "GET", portal, NULL, NULL, 0, NULL, 6, &r);
  for (int hop = 0; hop < 3; hop++) {
    if (r.status < 0 || !r.body) {
      snprintf(why, wcap, "login page did not load (%s)", r.err[0] ? r.err : "no body");
      goto out;
    }
    int n = hf_parse(r.body, forms, HF_MAX_FORMS);
    int pf, fi = best_form(forms, n, hf_find_phone, &pf);
    char next[1024];
    if (fi < 0 && hf_find_redirect(r.body, next, sizeof(next))) {
      char abs[1024];
      if (url_resolve(r.final_url, next, abs, sizeof(abs)) == 0) {
        httpc_resp_free(&r);
        httpc_request(s, "GET", abs, NULL, NULL, 0, NULL, 6, &r);
        continue;
      }
    }
    if (fi < 0) {
      snprintf(why, wcap, "no phone-number field found on the page (%d form(s)); see the recording", n);
      goto out;
    }
    snprintf(base, sizeof(base), "%s", r.final_url);
    hf_form_t f1 = forms[fi];
    int cf1 = hf_find_code(&f1);
    devlog("portal(form): phone field \"%s\" in form %d (%s %s)%s", f1.inputs[pf].name, fi, f1.method,
           f1.action[0] ? f1.action : "(same page)", cf1 >= 0 ? ", code field on the same form" : "");
    char *body = build_body(&f1, pf, phone, cf1, "");
    int64_t asked_us = esp_timer_get_time();
    httpc_resp_free(&r);
    submit(s, base, &f1, body, &r);
    free(body);
    if (r.status < 0 || r.status >= 400) {
      snprintf(why, wcap, "sending the number failed (HTTP %d %s)", r.status, r.err);
      goto out;
    }
    // Find where the code goes: a form on the reply page, else the first form again.
    hf_form_t f2;
    int cf = -1;
    char base2[1024];
    snprintf(base2, sizeof(base2), "%s", r.final_url);
    if (r.body) {
      int n2 = hf_parse(r.body, forms, HF_MAX_FORMS);
      int fi2 = best_form(forms, n2, hf_find_code, &cf);
      if (fi2 >= 0) f2 = forms[fi2];
    }
    if (cf < 0 && cf1 >= 0) {
      f2 = f1;
      cf = cf1;
      snprintf(base2, sizeof(base2), "%s", base);
    }
    if (cf < 0) {
      snprintf(why, wcap, "number sent, but no code field found afterwards; see the recording");
      goto out;
    }
    set_state("waiting for SMS code");
    char code[16];
    if (!sms_wait_code(asked_us, cfg_int("otp_wait_s"), code, sizeof(code))) {
      snprintf(why, wcap, "no SMS code arrived within %d s", cfg_int("otp_wait_s"));
      goto out;
    }
    snprintf(s->mask_code, sizeof(s->mask_code), "%s", code);
    int pf2 = hf_find_phone(&f2);
    if (pf2 == cf) pf2 = -1;
    body = build_body(&f2, pf2, phone, cf, code);
    httpc_resp_free(&r);
    set_state("submitting code");
    submit(s, base2, &f2, body, &r);
    free(body);
    if (r.status < 0 || r.status >= 400) {
      snprintf(why, wcap, "submitting the code failed (HTTP %d %s)", r.status, r.err);
      goto out;
    }
    ok = true;
    snprintf(why, wcap, "form submitted (phone field \"%s\", code field \"%s\")", f1.inputs[pf].name,
             f2.inputs[cf].name);
    goto out;
  }
  snprintf(why, wcap, "too many redirects before a form");
out:
  httpc_resp_free(&r);
  free(forms);
  return ok;
}

// ---------- method: recipe ----------

typedef struct {
  httpc_session_t *s;
  const char *portal;
  const char *phone;
  char code[16];
  int64_t asked_us;
  char *prev_body;
  char prev_url[1024];
  hf_form_t *forms;
  int n_forms;
  cJSON *want;  // fields of the current step, to pick the right form for {form_action}
} rctx_t;

// Expands {phone} {code} {portal_url} {form_action} {field:NAME} {cookie:NAME} in `in`.
static char *expand(rctx_t *c, const char *in, bool urlenc, char *why, size_t wcap) {
  size_t cap = strlen(in) + 256, len = 0;
  char *out = malloc(cap);
  const char *p = in;
  while (*p) {
    char val[1100];
    const char *end = NULL;
    bool have = false;
    if (*p == '{' && (end = strchr(p, '}'))) {
      char tok[80];
      snprintf(tok, sizeof(tok), "%.*s", (int)(end - p - 1), p + 1);
      have = true;
      if (!strcmp(tok, "phone")) snprintf(val, sizeof(val), "%s", c->phone);
      else if (!strcmp(tok, "code")) {
        if (!c->code[0]) {
          set_state("waiting for SMS code");
          if (!sms_wait_code(c->asked_us, cfg_int("otp_wait_s"), c->code, sizeof(c->code))) {
            snprintf(why, wcap, "no SMS code arrived within %d s", cfg_int("otp_wait_s"));
            free(out);
            return NULL;
          }
          snprintf(c->s->mask_code, sizeof(c->s->mask_code), "%s", c->code);
        }
        snprintf(val, sizeof(val), "%s", c->code);
      } else if (!strcmp(tok, "portal_url")) snprintf(val, sizeof(val), "%s", c->portal);
      else if (!strcmp(tok, "form_action")) {
        val[0] = 0;
        int best = -1, best_hits = -1;
        for (int i = 0; i < c->n_forms; i++) {
          int hits = 0;
          for (int k = 0; k < c->forms[i].n_inputs; k++)
            if (c->want && c->forms[i].inputs[k].name[0] && cJSON_GetObjectItem(c->want, c->forms[i].inputs[k].name))
              hits++;
          if (hits > best_hits) best_hits = hits, best = i;
        }
        if (best >= 0 && url_resolve(c->prev_url, c->forms[best].action, val, sizeof(val))) val[0] = 0;
        if (!val[0]) snprintf(val, sizeof(val), "%s", c->prev_url);
      } else if (!strncmp(tok, "field:", 6)) {
        const char *v = hf_value_of(c->forms, c->n_forms, tok + 6);
        if (!v) devlog("recipe: field \"%s\" not on the previous page; using empty", tok + 6);
        snprintf(val, sizeof(val), "%s", v ? v : "");
      } else if (!strncmp(tok, "cookie:", 7)) {
        val[0] = 0;
        for (int i = 0; i < c->s->n_cookies; i++)
          if (!strcmp(c->s->cookies[i].name, tok + 7)) snprintf(val, sizeof(val), "%s", c->s->cookies[i].value);
      } else {
        have = false;
      }
    }
    if (have) {
      char enc[3300];
      const char *v = val;
      if (urlenc) {
        url_encode(val, enc, sizeof(enc));
        v = enc;
      }
      size_t vl = strlen(v);
      if (len + vl + 1 > cap) {
        cap = (len + vl + 1) * 2;
        out = realloc(out, cap);
      }
      memcpy(out + len, v, vl);
      len += vl;
      p = end + 1;
    } else {
      if (len + 2 > cap) {
        cap *= 2;
        out = realloc(out, cap);
      }
      out[len++] = *p++;
    }
  }
  out[len] = 0;
  return out;
}

static bool run_recipe(httpc_session_t *s, const char *json, const char *portal, const char *phone, char *why,
                       size_t wcap) {
  cJSON *root = cJSON_Parse(json);
  cJSON *steps = root ? cJSON_GetObjectItem(root, "steps") : NULL;
  if (!cJSON_IsArray(steps)) {
    snprintf(why, wcap, "recipe is not valid JSON with a \"steps\" array");
    cJSON_Delete(root);
    return false;
  }
  rctx_t c = {.s = s, .portal = portal, .phone = phone, .asked_us = esp_timer_get_time()};
  c.forms = calloc(HF_MAX_FORMS, sizeof(hf_form_t));
  snprintf(c.prev_url, sizeof(c.prev_url), "%s", portal);
  bool ok = true;
  int i = 0;
  cJSON *st;
  cJSON_ArrayForEach(st, steps) {
    i++;
    if (cJSON_IsTrue(cJSON_GetObjectItem(st, "wait_code"))) {
      char *x = expand(&c, "{code}", false, why, wcap);
      if (!x) {
        ok = false;
        break;
      }
      free(x);
      continue;
    }
    c.want = cJSON_GetObjectItem(st, "form");
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(st, "method"));
    const char *url_t = cJSON_GetStringValue(cJSON_GetObjectItem(st, "url"));
    if (!url_t) {
      snprintf(why, wcap, "step %d has no url", i);
      ok = false;
      break;
    }
    if (!method) method = "GET";
    char *url = expand(&c, url_t, false, why, wcap);
    if (!url) {
      ok = false;
      break;
    }
    char *body = NULL;
    const char *ct = NULL;
    cJSON *form = cJSON_GetObjectItem(st, "form");
    const char *raw = cJSON_GetStringValue(cJSON_GetObjectItem(st, "body"));
    if (cJSON_IsObject(form)) {
      size_t cap = 256, len = 0;
      body = malloc(cap);
      body[0] = 0;
      cJSON *kv;
      cJSON_ArrayForEach(kv, form) {
        const char *vt = cJSON_GetStringValue(kv);
        char *v = expand(&c, vt ? vt : "", false, why, wcap);
        if (!v) {
          ok = false;
          break;
        }
        append_pair(&body, &len, &cap, kv->string, v);
        free(v);
      }
      ct = "application/x-www-form-urlencoded";
    } else if (raw) {
      body = expand(&c, raw, false, why, wcap);
      if (!body) ok = false;
      ct = cJSON_GetStringValue(cJSON_GetObjectItem(st, "content_type"));
      if (!ct) ct = "application/json";
    }
    if (!ok) {
      free(url);
      free(body);
      break;
    }
    const char *extra = cJSON_GetStringValue(cJSON_GetObjectItem(st, "headers"));
    httpc_resp_t r = {0};
    if (form || raw) {
      // A step that sends the phone number restarts the code clock.
      char *dump = cJSON_PrintUnformatted(st);
      if (dump && strstr(dump, "{phone}")) c.asked_us = esp_timer_get_time();
      free(dump);
    }
    httpc_request(s, method, url, body ? ct : NULL, body, body ? strlen(body) : 0, extra, 6, &r);
    free(url);
    free(body);
    const char *expect = cJSON_GetStringValue(cJSON_GetObjectItem(st, "expect"));
    if (r.status < 0 || r.status >= 400) {
      snprintf(why, wcap, "step %d: HTTP %d %s", i, r.status, r.err);
      ok = false;
    } else if (expect && (!r.body || !strstr(r.body, expect))) {
      snprintf(why, wcap, "step %d: reply does not contain \"%s\"", i, expect);
      ok = false;
    }
    if (ok) {
      snprintf(c.prev_url, sizeof(c.prev_url), "%s", r.final_url);
      c.n_forms = r.body ? hf_parse(r.body, c.forms, HF_MAX_FORMS) : 0;
    }
    httpc_resp_free(&r);
    if (!ok) break;
  }
  if (ok) snprintf(why, wcap, "recipe completed (%d steps)", i);
  free(c.forms);
  cJSON_Delete(root);
  return ok;
}

// ---------- attempt ----------

static void attempt(void) {
  attempts++;
  last_attempt_us = esp_timer_get_time();
  last_attempt_t = time(NULL);
  char portal[1024];
  netmgr_portal_url(portal, sizeof(portal));
  if (!portal[0]) cfg_get("probe_url", portal, sizeof(portal));
  char phone[24], local[24];
  if (!portal_phone(phone, sizeof(phone), local, sizeof(local))) {
    snprintf(last_result, sizeof(last_result), "no phone number: set it under Settings > Phone number");
    event_post("Login skipped: %s", last_result);
    set_state("idle");
    return;
  }
  if (!netmgr_sta_connected()) {
    snprintf(last_result, sizeof(last_result), "not connected to an upstream Wi-Fi");
    set_state("idle");
    return;
  }
  httpc_session_t *s = calloc(1, sizeof(httpc_session_t));
  httpc_session_init(s, true);
  s->via = "auto";
  char cc[8];
  cfg_get("ph_cc", cc, sizeof(cc));
  snprintf(s->mask_phone[0], sizeof(s->mask_phone[0]), "%s", phone);
  snprintf(s->mask_phone[1], sizeof(s->mask_phone[1]), "%s", local);
  snprintf(s->mask_phone[2], sizeof(s->mask_phone[2]), "%s%s", cc, local);
  s->rec = rec_open("auto", last_rec, sizeof(last_rec));

  char strat[24], why[200] = "";
  cfg_get("po_strat", strat, sizeof(strat));
  char *recipe = strcmp(strat, "form") ? recipe_load() : NULL;
  bool ok;
  set_state("logging in");
  if (recipe) {
    devlog("portal: using saved recipe");
    ok = run_recipe(s, recipe, portal, phone, why, sizeof(why));
  } else if (!strcmp(strat, "recipe")) {
    ok = false;
    snprintf(why, sizeof(why), "method is 'recipe' but no recipe is saved");
  } else {
    devlog("portal: filling the form automatically");
    ok = run_form(s, portal, phone, why, sizeof(why));
  }
  free(recipe);
  rec_close(s->rec);
  free(s);

  // Did it work? Ask for a fresh internet check and wait for it.
  bool online = false;
  if (ok) {
    set_state("checking internet");
    for (int i = 0; i < 4 && !online; i++) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      netmgr_recheck();
      vTaskDelay(pdMS_TO_TICKS(2500));
      online = netmgr_wifi_state() == NET_ONLINE;
    }
  }
  if (online) {
    successes++;
    snprintf(last_result, sizeof(last_result), "logged in: %s", why);
    event_post("Login page passed: %s", why);
  } else {
    snprintf(last_result, sizeof(last_result), "%s%s", why, ok ? " - but the internet check still fails" : "");
    event_post("Login failed: %s (recording %s)", last_result, last_rec);
  }
  set_state("idle");
}

static void portal_task(void *arg) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    bool now = trigger;
    if (!now) {
      if (!cfg_is("po_mode", "auto") || netmgr_wifi_state() != NET_CAPTIVE) continue;
      int64_t gap = (int64_t)cfg_int("po_retry_s") * 1000000;
      if (last_attempt_us && esp_timer_get_time() - last_attempt_us < gap) continue;
    }
    trigger = false;
    attempt();
  }
}

void portal_start(void) { xTaskCreatePinnedToCore(portal_task, "portal", 20480, NULL, 3, NULL, 1); }
void portal_trigger(void) { trigger = true; }

void portal_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "portal");
  char mode[16];
  cfg_get("po_mode", mode, sizeof(mode));
  cJSON_AddStringToObject(o, "mode", mode);
  cJSON_AddStringToObject(o, "state", state);
  cJSON_AddStringToObject(o, "last_result", last_result);
  cJSON_AddStringToObject(o, "last_recording", last_rec);
  cJSON_AddNumberToObject(o, "last_attempt", (double)last_attempt_t);
  cJSON_AddNumberToObject(o, "attempts", attempts);
  cJSON_AddNumberToObject(o, "successes", successes);
  struct stat sb;
  cJSON_AddBoolToObject(o, "recipe_saved", stat(RECIPE_FILE, &sb) == 0);
  char phone[24], local[24];
  cJSON_AddStringToObject(o, "phone", portal_phone(phone, sizeof(phone), local, sizeof(local)) ? phone : "");
}

void portal_summary(char *out, size_t cap) {
  struct stat sb;
  snprintf(out, cap, "portal: state %s, attempts %u, successes %u, recipe %s\nlast result: %s\nlast recording: %s\n",
           state, attempts, successes, stat(RECIPE_FILE, &sb) == 0 ? "saved" : "none", last_result, last_rec);
}

// ---------- recipe files ----------

char *recipe_load(void) {
  FILE *f = fopen(RECIPE_FILE, "r");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 64 * 1024) {
    fclose(f);
    return NULL;
  }
  char *b = malloc((size_t)n + 1);
  size_t got = fread(b, 1, (size_t)n, f);
  b[got] = 0;
  fclose(f);
  return b;
}

bool recipe_save(const char *json, char *err, size_t errcap) {
  cJSON *r = cJSON_Parse(json);
  if (!r || !cJSON_IsArray(cJSON_GetObjectItem(r, "steps"))) {
    snprintf(err, errcap, "not valid JSON with a \"steps\" array");
    cJSON_Delete(r);
    return false;
  }
  char *pretty = cJSON_Print(r);
  cJSON_Delete(r);
  FILE *f = fopen(RECIPE_FILE, "w");
  bool ok = f && pretty && fputs(pretty, f) >= 0;
  if (f) fclose(f);
  free(pretty);
  if (!ok) snprintf(err, errcap, "could not write the recipe file");
  return ok;
}

bool recipe_delete(void) { return unlink(RECIPE_FILE) == 0; }

// ---------- recording -> recipe ----------

static bool is_asset(const char *url, const char *ct) {
  static const char *const ext[] = {".css", ".js", ".png", ".jpg", ".jpeg", ".gif", ".svg", ".ico", ".woff",
                                    ".woff2", ".ttf", ".webp", NULL};
  char path[512];
  snprintf(path, sizeof(path), "%s", url);
  path[strcspn(path, "?#")] = 0;
  size_t pl = strlen(path);
  for (int i = 0; ext[i]; i++) {
    size_t el = strlen(ext[i]);
    if (pl > el && !strcasecmp(path + pl - el, ext[i])) return true;
  }
  return ct && (strcasestr(ct, "image/") || strcasestr(ct, "font/") || strcasestr(ct, "text/css") ||
                strcasestr(ct, "javascript"));
}

char *recipe_from_recording(const char *rec_name, char *err, size_t errcap) {
  char path[96];
  if (!rec_path(rec_name, path, sizeof(path))) {
    snprintf(err, errcap, "no such recording");
    return NULL;
  }
  FILE *f = fopen(path, "r");
  if (!f) {
    snprintf(err, errcap, "cannot open recording");
    return NULL;
  }
  size_t lcap = 160 * 1024;
  char *line = malloc(lcap);
  hf_form_t *forms = calloc(HF_MAX_FORMS, sizeof(hf_form_t));
  int nforms = 0;
  char prev_url[1024] = "";
  cJSON *root = cJSON_CreateObject();
  cJSON *steps = cJSON_AddArrayToObject(root, "steps");
  cJSON *first = cJSON_CreateObject();
  cJSON_AddStringToObject(first, "method", "GET");
  cJSON_AddStringToObject(first, "url", "{portal_url}");
  cJSON_AddItemToArray(steps, first);
  bool waited = false;
  int sends = 0;
  while (line && forms && fgets(line, lcap, f)) {
    cJSON *o = cJSON_Parse(line);
    if (!o) continue;
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(o, "method"));
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(o, "url"));
    const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(o, "req_body"));
    const char *req_ct = cJSON_GetStringValue(cJSON_GetObjectItem(o, "req_ct"));
    const char *resp_ct = cJSON_GetStringValue(cJSON_GetObjectItem(o, "resp_ct"));
    const char *resp = cJSON_GetStringValue(cJSON_GetObjectItem(o, "resp_body"));
    if (!method || !url) {
      cJSON_Delete(o);
      continue;
    }
    bool sends_data = strcasecmp(method, "GET") || strstr(url, "{phone}") || strstr(url, "{code}");
    if (sends_data && !is_asset(url, NULL)) {
      bool needs_code = strstr(url, "{code}") || (body && strstr(body, "{code}"));
      if (needs_code && !waited) {
        cJSON *w = cJSON_CreateObject();
        cJSON_AddBoolToObject(w, "wait_code", true);
        cJSON_AddItemToArray(steps, w);
        waited = true;
      }
      cJSON *st = cJSON_CreateObject();
      cJSON_AddStringToObject(st, "method", method);
      // Use {form_action} when the URL is the action of a form on the previous page.
      const char *url_t = url;
      char abs[1024];
      for (int i = 0; i < nforms; i++)
        if (url_resolve(prev_url, forms[i].action, abs, sizeof(abs)) == 0 && !strcmp(abs, url)) url_t = "{form_action}";
      cJSON_AddStringToObject(st, "url", url_t);
      if (body && (!req_ct || strcasestr(req_ct, "x-www-form-urlencoded"))) {
        cJSON *form = cJSON_AddObjectToObject(st, "form");
        char *copy = strdup(body), *save = NULL;
        for (char *kv = strtok_r(copy, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
          char *eq = strchr(kv, '=');
          char *k = kv, *v = (char *)"";
          if (eq) {
            *eq = 0;
            v = eq + 1;
          }
          url_decode_inplace(k);
          url_decode_inplace(v);
          const char *prev = hf_value_of(forms, nforms, k);
          char tmpl[96];
          if (strstr(v, "{phone}") || strstr(v, "{code}")) cJSON_AddStringToObject(form, k, v);
          else if (prev && prev[0] && !strcmp(prev, v)) {
            snprintf(tmpl, sizeof(tmpl), "{field:%s}", k);
            cJSON_AddStringToObject(form, k, tmpl);
          } else cJSON_AddStringToObject(form, k, v);
        }
        free(copy);
      } else if (body) {
        cJSON_AddStringToObject(st, "body", body);
        cJSON_AddStringToObject(st, "content_type", req_ct ? req_ct : "application/json");
      }
      cJSON_AddItemToArray(steps, st);
      sends++;
    }
    if (resp && resp_ct && strcasestr(resp_ct, "html") && !is_asset(url, resp_ct)) {
      nforms = hf_parse(resp, forms, HF_MAX_FORMS);
      snprintf(prev_url, sizeof(prev_url), "%s", url);
    }
    cJSON_Delete(o);
  }
  fclose(f);
  free(line);
  free(forms);
  if (!sends) {
    snprintf(err, errcap, "the recording has no form submissions to learn from");
    cJSON_Delete(root);
    return NULL;
  }
  char *out = cJSON_Print(root);
  cJSON_Delete(root);
  return out;
}
