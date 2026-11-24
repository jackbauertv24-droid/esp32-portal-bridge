#include "netparse.h"

#include <string.h>

// Copies a DHCP option string, keeping printable ASCII only.
static void copy_opt(const uint8_t *v, size_t n, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap; i++)
    if (v[i] >= 0x20 && v[i] < 0x7f) out[o++] = (char)v[i];
  out[o] = 0;
}

bool dhcp_client_info(const uint8_t *ip, size_t len, uint8_t mac[6], char *host, size_t hcap, char *vendor,
                      size_t vcap) {
  if (hcap) host[0] = 0;
  if (vcap) vendor[0] = 0;
  if (len < 20 || (ip[0] >> 4) != 4 || ip[9] != 17) return false;
  size_t ihl = (size_t)(ip[0] & 15) * 4;
  if (ihl < 20 || len < ihl + 8) return false;
  const uint8_t *udp = ip + ihl;
  unsigned sport = (unsigned)(udp[0] << 8 | udp[1]), dport = (unsigned)(udp[2] << 8 | udp[3]);
  if (sport != 68 || dport != 67) return false;
  const uint8_t *b = udp + 8;
  size_t blen = len - ihl - 8;
  // BOOTP fixed part is 236 bytes, then the magic cookie 63 82 53 63.
  if (blen < 240 || b[0] != 1 || b[1] != 1 || b[2] != 6) return false;
  if (b[236] != 0x63 || b[237] != 0x82 || b[238] != 0x53 || b[239] != 0x63) return false;
  memcpy(mac, b + 28, 6);
  size_t i = 240;
  while (i < blen) {
    uint8_t code = b[i];
    if (code == 255) break;
    if (code == 0) {
      i++;
      continue;
    }
    if (i + 1 >= blen) break;
    size_t n = b[i + 1];
    if (i + 2 + n > blen) break;
    if (code == 12) copy_opt(b + i + 2, n, host, hcap);
    else if (code == 60) copy_opt(b + i + 2, n, vendor, vcap);
    i += 2 + n;
  }
  return true;
}
