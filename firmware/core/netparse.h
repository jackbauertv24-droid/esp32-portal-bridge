// Small packet and modem-reply parsers, kept free of ESP-IDF so they can be
// tested on a PC (tests/test_netparse.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Reads a DHCP request sent by a hotspot device. `ip` points at the IPv4
// header and `len` bytes are readable. On success fills the client MAC, the
// host name (option 12) and the vendor class (option 60, e.g. "android-dhcp-14");
// either string may be empty. Returns false if this is not a DHCP client packet.
bool dhcp_client_info(const uint8_t *ip, size_t len, uint8_t mac[6], char *host, size_t hcap, char *vendor,
                      size_t vcap);
