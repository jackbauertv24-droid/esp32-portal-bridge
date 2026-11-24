#include "callparse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_key(char c) { return (c >= '0' && c <= '9') || c == '*' || c == '#' || (c >= 'A' && c <= 'D'); }

static bool starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

static bool has_dtmf_word(const char *l) {
  char up[48];
  size_t i = 0;
  for (; l[i] && l[i] != ':' && i < sizeof(up) - 1; i++) up[i] = (char)toupper((unsigned char)l[i]);
  up[i] = 0;
  return l[0] == '+' && (strstr(up, "DTMF") || strstr(up, "TONEDET") || strstr(up, "DDET"));
}

call_urc_t call_urc_parse(const char *l, char *arg, size_t cap) {
  if (cap) arg[0] = 0;
  if (!strcmp(l, "RING")) return CU_RING;
  if (starts(l, "+CLIP:")) {
    const char *q = strchr(l, '"');
    if (q) {
      const char *e = strchr(q + 1, '"');
      size_t n = e ? (size_t)(e - q - 1) : 0;
      if (n >= cap) n = cap - 1;
      memcpy(arg, q + 1, n);
      arg[n] = 0;
    }
    return CU_CLIP;
  }
  if (starts(l, "VOICE CALL: BEGIN") || !strcmp(l, "CONNECT")) return CU_BEGIN;
  if (starts(l, "VOICE CALL: END") || !strcmp(l, "NO CARRIER") || !strcmp(l, "BUSY") || !strcmp(l, "NO ANSWER"))
    return CU_END;
  if (starts(l, "MISSED_CALL:") || starts(l, "+MISSED_CALL")) return CU_MISSED;
  if (starts(l, "+CREC:") && strstr(l, "full")) return CU_REC_FULL;
  if (has_dtmf_word(l)) {
    const char *v = strchr(l, ':');
    if (!v) return CU_NONE;
    v++;
    while (*v == ' ' || *v == '"') v++;
    char tok[8];
    size_t n = 0;
    while (v[n] && v[n] != '"' && v[n] != ',' && v[n] != ' ' && n < sizeof(tok) - 1) {
      tok[n] = v[n];
      n++;
    }
    tok[n] = 0;
    char key = 0;
    if (n == 1 && is_key(tok[0])) key = tok[0];
    else if (n == 2 || n == 3) {
      // ASCII code form, e.g. 53 for '5', 35 for '#', 42 for '*'.
      int c = atoi(tok);
      if (c > 0 && c < 128 && is_key((char)c)) key = (char)c;
    }
    if (!key || cap < 2) return CU_NONE;
    arg[0] = key;
    arg[1] = 0;
    return CU_DTMF;
  }
  return CU_NONE;
}

bool call_urc_maybe_dtmf(const char *l) {
  char a[4];
  return has_dtmf_word(l) && call_urc_parse(l, a, sizeof(a)) != CU_DTMF;
}

void keypad_init(keypad_t *k, const char *pin, const char *unmute, const char *mute) {
  memset(k, 0, sizeof(*k));
  snprintf(k->pin, sizeof(k->pin), "%s", pin ? pin : "");
  snprintf(k->unmute, sizeof(k->unmute), "%s", unmute ? unmute : "");
  snprintf(k->mute, sizeof(k->mute), "%s", mute ? mute : "");
}

static bool ends_with(const char *buf, size_t n, const char *seq) {
  size_t s = strlen(seq);
  return s && n >= s && !memcmp(buf + n - s, seq, s);
}

kp_event_t keypad_feed(keypad_t *k, char key) {
  if (!is_key(key)) return KP_NONE;
  if (!k->unlocked) {
    if (key == '*' || key == '#') {
      k->n = 0;  // start over
      return KP_NONE;
    }
    if (!(key >= '0' && key <= '9')) return KP_NONE;
    k->buf[k->n++] = key;
    size_t want = strlen(k->pin);
    if (want == 0) {
      k->n = 0;
      return KP_PIN_BAD;
    }
    if (k->n < want) return KP_NONE;
    bool ok = !memcmp(k->buf, k->pin, want);
    k->n = 0;
    if (ok) {
      k->unlocked = true;
      return KP_PIN_OK;
    }
    k->bad_tries++;
    return KP_PIN_BAD;
  }
  if (k->n == sizeof(k->buf)) {
    memmove(k->buf, k->buf + 1, sizeof(k->buf) - 1);
    k->n--;
  }
  k->buf[k->n++] = key;
  if (ends_with(k->buf, k->n, k->unmute)) {
    k->n = 0;
    return KP_UNMUTE;
  }
  if (ends_with(k->buf, k->n, k->mute)) {
    k->n = 0;
    return KP_MUTE;
  }
  return KP_NONE;
}
