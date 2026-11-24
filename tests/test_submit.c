// Prints SMS-SUBMIT PDUs for fixed inputs as JSON; tests/verify_submit.mjs
// parses them with the independent node-pdu library and checks the round trip.
#include <stdio.h>
#include <string.h>

#include "sms_pdu.h"

int main(void) {
  static const struct {
    const char *to, *text;
  } cases[] = {
      {"+85291234567", "Hello from Portal Bridge"},
      {"91234567", "Braces {ok} [x] ~^ and euro \xE2\x82\xAC"},
      {"+85291234567", "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE9\xA9\x97\xE8\xAD\x89\xE7\xA2\xBC 123456 \xF0\x9F\x98\x80"},
      {"+85291234567",
       "Long message 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 "
       "0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789 end"},
      {"+85291234567",
       "\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF"
       "\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF"
       "\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF"
       "\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF"
       "\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF"
       "\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF"
       "\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF"
       "\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF\xE9\x95\xB7\xE8\xA8\x8A\xE6\x81\xAF"},
  };
  printf("[");
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    sms_submit_part_t parts[SMS_SUBMIT_MAX_PARTS];
    int n = sms_pdu_encode_submit(cases[c].to, cases[c].text, 0x42, parts, SMS_SUBMIT_MAX_PARTS);
    if (n <= 0) {
      fprintf(stderr, "case %zu: encode failed %d\n", c, n);
      return 1;
    }
    printf("%s{\"to\":\"%s\",\"text\":\"", c ? "," : "", cases[c].to);
    for (const char *p = cases[c].text; *p; p++) printf(*p == '"' ? "\\\"" : "%c", *p);
    printf("\",\"parts\":[");
    for (int i = 0; i < n; i++) {
      // AT+CMGS length must equal the octets after the SMSC byte.
      if ((int)strlen(parts[i].hex) != 2 + 2 * parts[i].tpdu_len) {
        fprintf(stderr, "case %zu part %d: length mismatch\n", c, i);
        return 1;
      }
      printf("%s\"%s\"", i ? "," : "", parts[i].hex);
    }
    printf("]}");
  }
  printf("]\n");
  if (sms_pdu_encode_submit("abc", "x", 1, NULL, 0) != -1) return 1;
  return 0;
}
