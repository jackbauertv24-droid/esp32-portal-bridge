#include "redact.h"

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
