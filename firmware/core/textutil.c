#include "textutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy(char *out, size_t cap, const char *s) {
  if (!cap) return;
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

void phone_local_part(const char *raw, const char *cc, char *out, size_t cap) {
  char digits[40];
  size_t n = 0;
  int plus = 0;
  for (const char *p = raw; *p && n + 1 < sizeof(digits); p++) {
    if (*p == '+' && n == 0) plus = 1;
    if (isdigit((unsigned char)*p)) digits[n++] = *p;
  }
  digits[n] = 0;
  const char *d = digits;
  size_t ccl = cc ? strlen(cc) : 0;
  if (ccl) {
    if (plus && strncmp(d, cc, ccl) == 0) d += ccl;
    else if (!plus && strncmp(d, "00", 2) == 0 && strncmp(d + 2, cc, ccl) == 0) d += 2 + ccl;
    else if (!plus && strncmp(d, cc, ccl) == 0 && strlen(d) - ccl >= 7) d += ccl;
  }
  copy(out, cap, d);
}

void phone_format(const char *cc, const char *local, const char *fmt, char *out, size_t cap) {
  if (!fmt || !strcmp(fmt, "local")) snprintf(out, cap, "%s", local);
  else if (!strcmp(fmt, "plus_cc")) snprintf(out, cap, "+%s%s", cc, local);
  else if (!strcmp(fmt, "cc")) snprintf(out, cap, "%s%s", cc, local);
  else if (!strcmp(fmt, "00cc")) snprintf(out, cap, "00%s%s", cc, local);
  else snprintf(out, cap, "%s", local);
}

int otp_extract(const char *text, int min_len, int max_len, char *out, size_t cap) {
  const char *p = text;
  while (*p) {
    if (isdigit((unsigned char)*p)) {
      const char *s = p;
      while (isdigit((unsigned char)*p)) p++;
      int len = (int)(p - s);
      if (len >= min_len && len <= max_len && (size_t)len < cap) {
        memcpy(out, s, len);
        out[len] = 0;
        return 1;
      }
    } else {
      p++;
    }
  }
  return 0;
}

int str_contains_ci(const char *hay, const char *needle) {
  size_t nl = strlen(needle);
  if (!nl) return 1;
  for (const char *h = hay; *h; h++) {
    size_t i = 0;
    while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == nl) return 1;
  }
  return 0;
}

// The spellings of <name>, written into `forms`.
static int spellings(const char *name, char forms[4][64]) {
  snprintf(forms[0], 64, "<%s>", name);
  snprintf(forms[1], 64, "%%3C%s%%3E", name);
  snprintf(forms[2], 64, "\\u003c%s\\u003e", name);
  snprintf(forms[3], 64, "&lt;%s&gt;", name);
  return 4;
}

static int match_ci(const char *s, const char *pat) {
  size_t i = 0;
  for (; pat[i]; i++)
    if (!s[i] || tolower((unsigned char)s[i]) != tolower((unsigned char)pat[i])) return 0;
  return (int)i;
}

int placeholder_present(const char *in, const char *name) {
  char forms[4][64];
  int nf = spellings(name, forms);
  for (const char *p = in; *p; p++)
    for (int f = 0; f < nf; f++)
      if (match_ci(p, forms[f])) return 1;
  return 0;
}

char *placeholder_replace(const char *in, const char *name, const char *value, int *count) {
  char forms[4][64];
  int nf = spellings(name, forms);
  size_t vl = strlen(value);
  size_t cap = strlen(in) + 1;
  // Worst case every 7-byte "<x>" grows to the value; size generously in one pass.
  for (const char *p = in; *p; p++)
    for (int f = 0; f < nf; f++)
      if (match_ci(p, forms[f])) cap += vl;
  char *out = (char *)malloc(cap);
  if (!out) return NULL;
  size_t o = 0;
  int c = 0;
  const char *p = in;
  while (*p) {
    int m = 0;
    for (int f = 0; f < nf && !m; f++) m = match_ci(p, forms[f]);
    if (m) {
      memcpy(out + o, value, vl);
      o += vl;
      p += m;
      c++;
    } else {
      out[o++] = *p++;
    }
  }
  out[o] = 0;
  if (count) *count = c;
  return out;
}

void url_encode(const char *in, char *out, size_t cap) {
  static const char HX[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
      out[o++] = (char)*p;
    } else if (*p == ' ') {
      out[o++] = '+';
    } else {
      out[o++] = '%';
      out[o++] = HX[*p >> 4];
      out[o++] = HX[*p & 15];
    }
  }
  out[o] = 0;
}

static int hv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

void url_decode_inplace(char *s) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '+') {
      *o++ = ' ';
    } else if (*p == '%' && hv(p[1]) >= 0 && hv(p[2]) >= 0) {
      *o++ = (char)(hv(p[1]) * 16 + hv(p[2]));
      p += 2;
    } else {
      *o++ = *p;
    }
  }
  *o = 0;
}

// Digits only, at most the last `keep`.
static void tail_digits(const char *s, char *out, size_t keep) {
  char d[40];
  size_t n = 0;
  for (; *s && n < sizeof(d) - 1; s++)
    if (*s >= '0' && *s <= '9') d[n++] = *s;
  d[n] = 0;
  snprintf(out, keep + 1, "%s", n > keep ? d + n - keep : d);
}

int phone_same(const char *a, const char *b) {
  char x[9], y[9];
  tail_digits(a, x, 8);
  tail_digits(b, y, 8);
  return strlen(x) >= 6 && !strcmp(x, y);
}

int phone_in_list(const char *num, const char *list) {
  char item[40];
  while (list && *list) {
    size_t n = strcspn(list, ",; ");
    if (n && n < sizeof(item)) {
      memcpy(item, list, n);
      item[n] = 0;
      if (phone_same(num, item)) return 1;
    }
    list += n;
    while (*list == ',' || *list == ';' || *list == ' ') list++;
  }
  return 0;
}

// "Tue, 24 Nov 2026 15:42:54 GMT" (RFC 7231 IMF-fixdate) -> seconds since 1970, or -1.
long long http_date_parse(const char *s) {
  static const char *const MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  if (!s) return -1;
  const char *c = strchr(s, ',');
  if (!c) return -1;
  int d, y, hh, mm, ss;
  char mon[4] = {0};
  if (sscanf(c + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return -1;
  const char *m = strstr(MON, mon);
  if (strlen(mon) != 3 || !m || (m - MON) % 3) return -1;
  int mo = (int)(m - MON) / 3 + 1;
  if (y < 1970 || y > 2200 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60) return -1;
  // days from civil (proleptic Gregorian)
  int yy = y - (mo <= 2);
  long era = yy / 400;
  long yoe = yy - era * 400;
  long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + doe - 719468;
  return (long long)days * 86400 + hh * 3600 + mm * 60 + ss;
}
