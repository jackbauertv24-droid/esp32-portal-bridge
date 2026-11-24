// Host tests for core/netparse.c. Packets are built here, not captured.
#include <stdio.h>
#include <string.h>

#include "netparse.h"

static int failures = 0;
#define CHECK(label, cond)          \
  do {                              \
    if (!(cond)) {                  \
      printf("FAIL %s\n", label);   \
      failures++;                   \
    }                               \
  } while (0)

// Builds IPv4 + UDP + DHCP with the given options; returns total length.
static size_t build_dhcp(uint8_t *p, int sport, int dport, const uint8_t *opts, size_t olen) {
  memset(p, 0, 600);
  p[0] = 0x45;
  p[9] = 17;
  uint8_t *u = p + 20;
  u[0] = (uint8_t)(sport >> 8), u[1] = (uint8_t)sport, u[2] = (uint8_t)(dport >> 8), u[3] = (uint8_t)dport;
  uint8_t *b = u + 8;
  b[0] = 1, b[1] = 1, b[2] = 6;
  const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  memcpy(b + 28, mac, 6);
  b[236] = 0x63, b[237] = 0x82, b[238] = 0x53, b[239] = 0x63;
  memcpy(b + 240, opts, olen);
  return 20 + 8 + 240 + olen;
}

int main(void) {
  uint8_t pkt[600], mac[6];
  char host[33], vend[33];
  const uint8_t opts[] = {53, 1, 3, 0, 12, 7, 'p', 'h', 'o', 'n', 'e', '-', 'A', 60, 15, 'a', 'n', 'd', 'r', 'o',
                          'i', 'd', '-', 'd', 'h', 'c', 'p', '-', '1', '4', 255};
  size_t n = build_dhcp(pkt, 68, 67, opts, sizeof opts);
  CHECK("request parsed", dhcp_client_info(pkt, n, mac, host, sizeof host, vend, sizeof vend));
  CHECK("mac", mac[0] == 0x02 && mac[5] == 0x55);
  CHECK("host", !strcmp(host, "phone-A"));
  CHECK("vendor", !strcmp(vend, "android-dhcp-14"));

  CHECK("server reply ignored", !dhcp_client_info(pkt, build_dhcp(pkt, 67, 68, opts, sizeof opts), mac, host,
                                                  sizeof host, vend, sizeof vend));
  CHECK("truncated ignored", !dhcp_client_info(pkt, 200, mac, host, sizeof host, vend, sizeof vend));

  // An option that claims to run past the end must not be read.
  const uint8_t bad[] = {12, 200, 'x', 'y'};
  n = build_dhcp(pkt, 68, 67, bad, sizeof bad);
  CHECK("overlong option tolerated", dhcp_client_info(pkt, n, mac, host, sizeof host, vend, sizeof vend) && !host[0]);

  // Long and non-printable names are cut and cleaned.
  uint8_t lng[2 + 60 + 1];
  lng[0] = 12, lng[1] = 60;
  for (int i = 0; i < 60; i++) lng[2 + i] = (uint8_t)(i == 3 ? 0x07 : 'a' + i % 26);
  lng[62] = 255;
  n = build_dhcp(pkt, 68, 67, lng, sizeof lng);
  dhcp_client_info(pkt, n, mac, host, sizeof host, vend, sizeof vend);
  CHECK("long host cut", strlen(host) == 32 && !strchr(host, 0x07));

  CHECK("tcp ignored", (pkt[9] = 6, !dhcp_client_info(pkt, n, mac, host, sizeof host, vend, sizeof vend)));

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("netparse: all passed\n");
  return 0;
}
