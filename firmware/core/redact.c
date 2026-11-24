#include "redact.h"

#include <string.h>

void redact_long_numbers(char *s) {
  size_t i = 0;
  while (s[i]) {
    if (s[i] >= '0' && s[i] <= '9') {
      size_t start = i;
      while (s[i] >= '0' && s[i] <= '9') i++;
      size_t len = i - start;
      if (len >= 7) {
        for (size_t k = start; k < i - 3; k++) s[k] = '*';
      }
    } else {
      i++;
    }
  }
}

static int hexd(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

void redact_macs(char *s) {
  // aa:bb:cc:dd:ee:ff (or with '-') -> **:**:**:**:ee:ff
  size_t len = strlen(s);
  for (size_t i = 0; i + 17 <= len; i++) {
    char sep = s[i + 2];
    if (sep != ':' && sep != '-') continue;
    size_t k = 0;
    for (; k < 6; k++) {
      const char *p = s + i + k * 3;
      if (!hexd(p[0]) || !hexd(p[1])) break;
      if (k < 5 && p[2] != sep) break;
    }
    if (k != 6 || (i + 17 < len && hexd(s[i + 17])) || (i > 0 && hexd(s[i - 1]))) continue;
    for (size_t j = 0; j < 12; j++)
      if (s[i + j] != sep) s[i + j] = '*';
    i += 16;
  }
}

void redact_word(char *s, const char *word) {
  size_t wl = strlen(word);
  if (wl < 3) return;  // too short to replace without mangling unrelated text
  for (char *p = strstr(s, word); p; p = strstr(p + wl, word)) memset(p, '*', wl);
}
