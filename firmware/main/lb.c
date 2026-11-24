#include "lb.h"

#include <string.h>

#include "esp_netif_net_stack.h"
#include "events.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "settings.h"

// Read by the hook in the lwIP thread: plain volatile words, no locks.
static struct netif *volatile w_if, *volatile l_if;
static volatile bool on;
static volatile uint32_t ap_net, ap_mask = 0x00FFFFFF, dns_ip;
static volatile uint32_t pct = 50;
static volatile uint32_t hits_w, hits_l, pinned;

static inline bool usable(struct netif *n) {
  return n && netif_is_up(n) && netif_is_link_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n));
}

static inline bool same_subnet(struct netif *n, uint32_t a) {
  return n && ((a ^ ip4_addr_get_u32(netif_ip4_addr(n))) & ip4_addr_get_u32(netif_ip4_netmask(n))) == 0;
}

struct netif *pb_route_hook(const ip4_addr_t *src, const ip4_addr_t *dest) {
  if (on && src && dest) {
    uint32_t s = ip4_addr_get_u32(src), d = ip4_addr_get_u32(dest);
    struct netif *w = w_if, *l = l_if;
    if ((s & ap_mask) == ap_net && (d & ap_mask) != ap_net && d != IPADDR_BROADCAST && !ip4_addr_ismulticast(dest) &&
        usable(w) && usable(l)) {
      if (d == dns_ip) {
        pinned++;
        return NULL;  // default route: the uplink the DNS server came from
      }
      if (same_subnet(w, d)) return w;  // the venue network's own hosts
      uint32_t h = s * 2654435761u ^ d * 2246822519u;
      h ^= h >> 15;
      h *= 2654435761u;
      h ^= h >> 13;
      if (h % 100 < pct) {
        hits_w++;
        return w;
      }
      hits_l++;
      return l;
    }
  }
  // ESP-IDF's default behaviour: route by source address.
  if (src && !ip4_addr_isany(src)) {
    struct netif *n;
    NETIF_FOREACH(n) {
      if (usable(n) && ip4_addr_cmp(src, netif_ip4_addr(n))) return n;
    }
  }
  return NULL;
}

void lb_set_dns(uint32_t dns_addr) { dns_ip = dns_addr; }

void lb_update(esp_netif_t *wifi, bool wifi_online, esp_netif_t *lte, bool lte_online, const char *ap_ip) {
  esp_ip4_addr_t a;
  if (esp_netif_str_to_ip4(ap_ip, &a) == ESP_OK) ap_net = a.addr & ap_mask;
  pct = (uint32_t)cfg_int("lb_wifi_pct");
  w_if = wifi ? (struct netif *)esp_netif_get_netif_impl(wifi) : NULL;
  l_if = lte ? (struct netif *)esp_netif_get_netif_impl(lte) : NULL;
  bool want = cfg_is("lb_mode", "balance") && wifi_online && lte_online && w_if && l_if;
  if (want != on) {
    on = want;
    if (want) event_post("Load balancing on: Wi-Fi %lu%% / 4G %lu%%", (unsigned long)pct, (unsigned long)(100 - pct));
    else event_post("Load balancing off");
  }
}

void lb_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "lb");
  char mode[12];
  cfg_get("lb_mode", mode, sizeof(mode));
  cJSON_AddStringToObject(o, "mode", mode);
  cJSON_AddBoolToObject(o, "active", on);
  cJSON_AddNumberToObject(o, "wifi_pct", pct);
  cJSON_AddNumberToObject(o, "packets_wifi", hits_w);
  cJSON_AddNumberToObject(o, "packets_4g", hits_l);
  cJSON_AddNumberToObject(o, "dns_pinned", pinned);
}
