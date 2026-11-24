// Host tests for core/redact.c: what may leave the device in a public report.
#include <stdio.h>
#include <string.h>

#include "redact.h"

static int failures = 0;
#define CHECK_STR(label, got, want)                                            \
  do {                                                                         \
    if (strcmp((got), (want))) {                                               \
      printf("FAIL %s\n  got:  [%s]\n  want: [%s]\n", label, (got), (want));   \
      failures++;                                                              \
    }                                                                          \
  } while (0)

int main(void) {
  char a[] = "station: e2:32:e8:9c:be:11 join, bssid = ac:76:4C:e9:e3:ec end";
  redact_macs(a);
  CHECK_STR("macs", a, "station: **:**:**:**:be:11 join, bssid = **:**:**:**:e3:ec end");
  char b[] = "mac 7c-0c-5f-48-76-a4 at end 7c:0c:5f:48:76:a5";
  redact_macs(b);
  CHECK_STR("dash and end of string", b, "mac **-**-**-**-76-a4 at end **:**:**:**:76:a5");
  char c[] = "time 12:30:45 and ipv6 fe80::1 and 1:2:3 and aa:bb:cc:dd:ee";
  redact_macs(c);
  CHECK_STR("not macs untouched", c, "time 12:30:45 and ipv6 fe80::1 and 1:2:3 and aa:bb:cc:dd:ee");
  char d[] = "x";
  redact_macs(d);
  CHECK_STR("short string", d, "x");

  char e[] = "Joined \"Tenet\" as 10.0.0.2; Upstream Wi-Fi \"Tenet\": online";
  redact_word(e, "Tenet");
  CHECK_STR("ssid", e, "Joined \"*****\" as 10.0.0.2; Upstream Wi-Fi \"*****\": online");
  char f[] = "ab abc";
  redact_word(f, "ab");
  CHECK_STR("too short word kept", f, "ab abc");
  redact_word(f, "");
  CHECK_STR("empty word", f, "ab abc");

  char g[] = "imei 861234567890774 csq 30";
  redact_long_numbers(g);
  CHECK_STR("long numbers", g, "imei ************774 csq 30");

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("redact: all passed\n");
  return 0;
}
