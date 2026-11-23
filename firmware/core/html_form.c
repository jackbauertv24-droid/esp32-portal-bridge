#include "html_form.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "textutil.h"

static void copy_n(char *out, size_t cap, const char *s, size_t n) {
  if (!cap) return;
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

static void lower(char *s) {
  for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

// Decodes common HTML entities in place.
static void entity_decode(char *s) {
  char *o = s;
  for (char *p = s; *p;) {
    if (*p == '&') {
      struct { const char *e; const char *r; } T[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
                                                      {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
                                                      {"&nbsp;", " "}};
      int done = 0;
      for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        size_t l = strlen(T[i].e);
        if (!strncasecmp(p, T[i].e, l)) {
          *o++ = T[i].r[0];
          p += l;
          done = 1;
          break;
        }
      }
      if (!done && p[1] == '#') {
        char *q = p + 2;
        unsigned long cp = 0;
        int ok = 0;
        if (*q == 'x' || *q == 'X') {
          q++;
          while (isxdigit((unsigned char)*q)) {
            cp = cp * 16 + (isdigit((unsigned char)*q) ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10));
            q++;
            ok = 1;
          }
        } else {
          while (isdigit((unsigned char)*q)) {
            cp = cp * 10 + (unsigned long)(*q - '0');
            q++;
            ok = 1;
          }
        }
        if (ok && *q == ';' && cp > 0 && cp < 0x80) {
          *o++ = (char)cp;
          p = q + 1;
          done = 1;
        }
      }
      if (!done) *o++ = *p++;
    } else {
      *o++ = *p++;
    }
  }
  *o = 0;
}

// Reads attributes of a tag starting after its name; stops at '>'. Calls back
// for each attribute. Returns a pointer just past the '>' (or end of string).
typedef void (*attr_cb)(void *ctx, const char *name, const char *val);

static const char *parse_attrs(const char *p, attr_cb cb, void *ctx) {
  char name[32], val[600];
  for (;;) {
    while (*p && (isspace((unsigned char)*p) || *p == '/')) p++;
    if (!*p) return p;
    if (*p == '>') return p + 1;
    const char *ns = p;
    while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/') p++;
    copy_n(name, sizeof(name), ns, (size_t)(p - ns));
    lower(name);
    while (isspace((unsigned char)*p)) p++;
    val[0] = 0;
    if (*p == '=') {
      p++;
      while (isspace((unsigned char)*p)) p++;
      if (*p == '"' || *p == '\'') {
        char q = *p++;
        const char *vs = p;
        while (*p && *p != q) p++;
        copy_n(val, sizeof(val), vs, (size_t)(p - vs));
        if (*p) p++;
      } else {
        const char *vs = p;
        while (*p && !isspace((unsigned char)*p) && *p != '>') p++;
        copy_n(val, sizeof(val), vs, (size_t)(p - vs));
      }
      entity_decode(val);
    }
    if (name[0]) cb(ctx, name, val);
    if (p == ns) p++;  // never stall on junk
  }
}

typedef struct {
  hf_input_t *in;
  char for_id[64];
  char label[128];
  char http_equiv[32];
  char content[600];
  int selected;
} attr_ctx_t;

static void input_attr(void *vctx, const char *n, const char *v) {
  attr_ctx_t *c = (attr_ctx_t *)vctx;
  hf_input_t *in = c->in;
  if (!strcmp(n, "type")) {
    copy_n(in->type, sizeof(in->type), v, strlen(v));
    lower(in->type);
  } else if (!strcmp(n, "name")) copy_n(in->name, sizeof(in->name), v, strlen(v));
  else if (!strcmp(n, "id")) copy_n(in->id, sizeof(in->id), v, strlen(v));
  else if (!strcmp(n, "value")) copy_n(in->value, sizeof(in->value), v, strlen(v));
  else if (!strcmp(n, "placeholder")) copy_n(in->placeholder, sizeof(in->placeholder), v, strlen(v));
  else if (!strcmp(n, "checked")) in->checked = 1;
  else if (!strcmp(n, "aria-label") || !strcmp(n, "autocomplete") || !strcmp(n, "title")) {
    size_t l = strlen(in->hint);
    if (l + 2 < sizeof(in->hint)) {
      snprintf(in->hint + l, sizeof(in->hint) - l, "%s%s", l ? " " : "", v);
      lower(in->hint);
    }
  }
}

typedef struct {
  hf_form_t *f;
} form_ctx_t;

static void form_attr(void *vctx, const char *n, const char *v) {
  hf_form_t *f = ((form_ctx_t *)vctx)->f;
  if (!strcmp(n, "action")) copy_n(f->action, sizeof(f->action), v, strlen(v));
  else if (!strcmp(n, "method")) {
    copy_n(f->method, sizeof(f->method), v, strlen(v));
    lower(f->method);
  } else if (!strcmp(n, "enctype")) copy_n(f->enctype, sizeof(f->enctype), v, strlen(v));
}

static void misc_attr(void *vctx, const char *n, const char *v) {
  attr_ctx_t *c = (attr_ctx_t *)vctx;
  if (!strcmp(n, "for")) copy_n(c->for_id, sizeof(c->for_id), v, strlen(v));
  else if (!strcmp(n, "http-equiv")) copy_n(c->http_equiv, sizeof(c->http_equiv), v, strlen(v));
  else if (!strcmp(n, "content")) copy_n(c->content, sizeof(c->content), v, strlen(v));
  else if (!strcmp(n, "value") && c->in) copy_n(c->in->value, sizeof(c->in->value), v, strlen(v));
  else if (!strcmp(n, "selected")) c->selected = 1;
}

static int tag_is(const char *p, const char *name) {
  size_t l = strlen(name);
  return !strncasecmp(p, name, l) && !isalnum((unsigned char)p[l]);
}

// Skips to just after the closing tag `</name>`; returns end of string if absent.
static const char *skip_past_close(const char *p, const char *name) {
  size_t l = strlen(name);
  while ((p = strchr(p, '<')) != NULL) {
    if (p[1] == '/' && !strncasecmp(p + 2, name, l)) {
      const char *e = strchr(p, '>');
      return e ? e + 1 : p + strlen(p);
    }
    p++;
  }
  return NULL;
}

typedef struct {
  char id[64];
  char text[128];
} label_t;

int hf_parse(const char *html, hf_form_t *forms, int max) {
  int nf = 0;
  hf_form_t *cur = NULL;
  hf_form_t *loose = (max > 0) ? &forms[max - 1] : NULL;  // fields outside any form
  int loose_used = 0;
  label_t labels[16];
  int nl = 0;
  char lasttext[128] = "";
  if (loose) memset(loose, 0, sizeof(*loose));
  const char *p = html;
  while (p && *p) {
    if (*p != '<') {
      // Text node: remember the latest short text as a hint for the next field.
      const char *s = p;
      while (*p && *p != '<') p++;
      while (s < p && isspace((unsigned char)*s)) s++;
      const char *e = p;
      while (e > s && isspace((unsigned char)e[-1])) e--;
      if (e > s) {
        copy_n(lasttext, sizeof(lasttext), s, (size_t)(e - s));
        entity_decode(lasttext);
        lower(lasttext);
      }
      continue;
    }
    p++;
    if (!strncmp(p, "!--", 3)) {
      const char *e = strstr(p, "-->");
      p = e ? e + 3 : NULL;
      continue;
    }
    int closing = (*p == '/');
    if (closing) p++;
    if (tag_is(p, "script") || tag_is(p, "style")) {
      const char *name = tag_is(p, "script") ? "script" : "style";
      p = closing ? strchr(p, '>') : skip_past_close(p, name);
      if (p && closing) p++;
      continue;
    }
    if (tag_is(p, "form")) {
      if (closing) {
        cur = NULL;
        p = strchr(p, '>');
        if (p) p++;
        continue;
      }
      if (nf < max - 1 || (nf < max && !loose_used)) {
        cur = &forms[nf++];
        if (cur == loose) loose = NULL;
        memset(cur, 0, sizeof(*cur));
        strcpy(cur->method, "get");
        form_ctx_t fc = {cur};
        p = parse_attrs(p + 4, form_attr, &fc);
      } else {
        cur = NULL;
        p = strchr(p, '>');
        if (p) p++;
      }
      continue;
    }
    if (!closing && (tag_is(p, "input") || tag_is(p, "select") || tag_is(p, "textarea") || tag_is(p, "button"))) {
      hf_form_t *target = cur;
      if (!target && loose) {
        target = loose;
        loose_used = 1;
      }
      hf_input_t scratch;
      hf_input_t *in = (target && target->n_inputs < HF_MAX_INPUTS) ? &target->inputs[target->n_inputs] : &scratch;
      memset(in, 0, sizeof(*in));
      const char *tagname = tag_is(p, "input") ? "input" : tag_is(p, "select") ? "select"
                            : tag_is(p, "textarea") ? "textarea" : "button";
      strcpy(in->tag, tagname);
      attr_ctx_t c;
      memset(&c, 0, sizeof(c));
      c.in = in;
      p = parse_attrs(p + strlen(tagname), input_attr, &c);
      if (!in->type[0]) strcpy(in->type, !strcmp(tagname, "button") ? "submit" : "text");
      if (!strcmp(tagname, "select")) {
        strcpy(in->type, "select");
        const char *end = skip_past_close(p, "select");
        int have = 0;
        const char *q = p;
        while (q && (!end || q < end) && (q = strstr(q, "<option")) != NULL && (!end || q < end)) {
          attr_ctx_t oc;
          memset(&oc, 0, sizeof(oc));
          hf_input_t tmp;
          memset(&tmp, 0, sizeof(tmp));
          oc.in = &tmp;
          q = parse_attrs(q + 7, misc_attr, &oc);
          if (!have || oc.selected) {
            copy_n(in->value, sizeof(in->value), tmp.value, strlen(tmp.value));
            have = 1;
          }
        }
        p = end ? end : p;
      } else if (!strcmp(tagname, "textarea")) {
        const char *e = p ? strstr(p, "</") : NULL;
        if (e) {
          copy_n(in->value, sizeof(in->value), p, (size_t)(e - p));
          entity_decode(in->value);
        }
      }
      if (!in->hint[0] || strlen(in->hint) < 3) {
        size_t l = strlen(in->hint);
        snprintf(in->hint + l, sizeof(in->hint) - l, "%s%s", l ? " " : "", lasttext);
      }
      if (in != &scratch) target->n_inputs++;
      continue;
    }
    if (!closing && tag_is(p, "label")) {
      attr_ctx_t c;
      memset(&c, 0, sizeof(c));
      p = parse_attrs(p + 5, misc_attr, &c);
      if (c.for_id[0] && nl < 16 && p) {
        const char *e = strstr(p, "</");
        label_t *L = &labels[nl++];
        copy_n(L->id, sizeof(L->id), c.for_id, strlen(c.for_id));
        // Label text, tags stripped.
        size_t o = 0;
        int intag = 0;
        for (const char *q = p; *q && (!e || q < e) && o + 1 < sizeof(L->text); q++) {
          if (*q == '<') intag = 1;
          else if (*q == '>') intag = 0;
          else if (!intag) L->text[o++] = (char)tolower((unsigned char)*q);
        }
        L->text[o] = 0;
      }
      continue;
    }
    // Any other tag: skip it.
    p = strchr(p, '>');
    if (p) p++;
  }
  // Use the pseudo-form only if there was no real form with fields.
  int real_fields = 0;
  for (int i = 0; i < nf; i++) real_fields += forms[i].n_inputs;
  if (loose && loose_used && loose->n_inputs && !real_fields && nf < max) {
    if (&forms[nf] != loose) memcpy(&forms[nf], loose, sizeof(*loose));
    strcpy(forms[nf].method, "post");
    nf++;
  }
  // Attach <label for=...> texts.
  for (int i = 0; i < nf; i++)
    for (int k = 0; k < forms[i].n_inputs; k++) {
      hf_input_t *in = &forms[i].inputs[k];
      for (int l = 0; l < nl; l++)
        if (in->id[0] && !strcmp(in->id, labels[l].id)) {
          size_t hl = strlen(in->hint);
          snprintf(in->hint + hl, sizeof(in->hint) - hl, " %s", labels[l].text);
        }
    }
  return nf;
}

int hf_is_text_entry(const hf_input_t *in) {
  static const char *const ok[] = {"text", "tel", "number", "email", "search", "password", NULL};
  if (strcmp(in->tag, "input") && strcmp(in->tag, "textarea")) return 0;
  if (!strcmp(in->tag, "textarea")) return 1;
  for (int i = 0; ok[i]; i++)
    if (!strcmp(in->type, ok[i])) return 1;
  return 0;
}

static int any_kw(const hf_input_t *in, const char *const *kws) {
  const char *fields[] = {in->name, in->id, in->placeholder, in->hint};
  for (int f = 0; f < 4; f++)
    for (int k = 0; kws[k]; k++)
      if (str_contains_ci(fields[f], kws[k])) return 1;
  return 0;
}

static const char *const PHONE_KW[] = {"phone", "mobile", "msisdn", "cell", "tel", "電話", "手機", "手提", "电话",
                                       "手机", "號碼", "号码", NULL};
static const char *const CODE_KW[] = {"code", "otp", "verif", "pin", "token", "sms", "passcode", "驗證", "验证",
                                      "認證", "认证", "密碼", "密码", NULL};

int hf_phone_score(const hf_input_t *in) {
  if (!hf_is_text_entry(in) || !strcmp(in->type, "password") || !strcmp(in->type, "email")) return 0;
  int s = 1;
  if (!strcmp(in->type, "tel")) s += 50;
  if (any_kw(in, PHONE_KW)) s += 40;
  if (str_contains_ci(in->hint, "one-time-code")) s -= 60;
  if (any_kw(in, CODE_KW) && strcmp(in->type, "tel")) s -= 30;
  return s > 0 ? s : 0;
}

int hf_code_score(const hf_input_t *in) {
  if (!hf_is_text_entry(in) || !strcmp(in->type, "email")) return 0;
  int s = 0;
  if (str_contains_ci(in->hint, "one-time-code")) s += 60;
  if (any_kw(in, CODE_KW)) s += 50;
  if (any_kw(in, PHONE_KW) && !str_contains_ci(in->hint, "one-time-code")) s -= 40;
  if (s > 0 && (!strcmp(in->type, "number") || !strcmp(in->type, "tel") || !strcmp(in->type, "text"))) s += 5;
  return s > 0 ? s : 0;
}

int hf_find_phone(const hf_form_t *f) {
  int best = -1, bs = 0, entries = 0, only = -1;
  for (int i = 0; i < f->n_inputs; i++) {
    int s = hf_phone_score(&f->inputs[i]);
    if (hf_is_text_entry(&f->inputs[i]) && strcmp(f->inputs[i].type, "password")) {
      entries++;
      only = i;
    }
    if (s > bs) {
      bs = s;
      best = i;
    }
  }
  if (bs >= 30) return best;
  if (entries == 1 && hf_code_score(&f->inputs[only]) < 30) return only;
  return -1;
}

int hf_find_code(const hf_form_t *f) {
  int best = -1, bs = 0;
  for (int i = 0; i < f->n_inputs; i++) {
    int s = hf_code_score(&f->inputs[i]);
    if (s > bs) {
      bs = s;
      best = i;
    }
  }
  return bs >= 30 ? best : -1;
}

const char *hf_value_of(const hf_form_t *forms, int n, const char *name) {
  for (int i = 0; i < n; i++)
    for (int k = 0; k < forms[i].n_inputs; k++)
      if (!strcmp(forms[i].inputs[k].name, name)) return forms[i].inputs[k].value;
  return NULL;
}

static int quoted_after(const char *p, char *out, size_t cap) {
  while (*p && (isspace((unsigned char)*p) || *p == '=' || *p == '(')) p++;
  if (*p != '"' && *p != '\'') return 0;
  char q = *p++;
  const char *s = p;
  while (*p && *p != q && *p != '\n') p++;
  if (*p != q || p == s) return 0;
  copy_n(out, cap, s, (size_t)(p - s));
  return 1;
}

int hf_find_redirect(const char *html, char *out, size_t cap) {
  // <meta http-equiv="refresh" content="0; url=...">
  for (const char *p = html; (p = strchr(p, '<')) != NULL; p++) {
    if (!tag_is(p + 1, "meta")) continue;
    attr_ctx_t c;
    memset(&c, 0, sizeof(c));
    parse_attrs(p + 5, misc_attr, &c);
    if (!strcasecmp(c.http_equiv, "refresh")) {
      const char *u = c.content;
      while (*u && strncasecmp(u, "url", 3)) u++;
      if (*u) {
        u += 3;
        while (*u == ' ' || *u == '=') u++;
        if (*u == '\'' || *u == '"') u++;
        copy_n(out, cap, u, strcspn(u, "'\""));
        if (out[0]) return 1;
      }
    }
  }
  static const char *const pats[] = {"location.href", "location.replace", "location.assign", "window.location",
                                     "document.location", "top.location", NULL};
  for (int i = 0; pats[i]; i++) {
    const char *p = html;
    size_t pl = strlen(pats[i]);
    while ((p = strstr(p, pats[i])) != NULL) {
      p += pl;
      const char *q = p;
      if (!strncmp(q, ".href", 5)) q += 5;
      while (*q == ' ') q++;
      if (*q == '=' && q[1] == '=') continue;
      if (quoted_after(q, out, cap)) return 1;
    }
  }
  return 0;
}
