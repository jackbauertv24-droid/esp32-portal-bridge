#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "devlog.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "netmgr.h"
#include "netparse.h"
#include "nvs.h"
#include "settings.h"

#define MAX_DEV 32
#define TICK_MS 2000

// ---------- lwIP side (runs in the tcpip thread, no locks) ----------

static struct netif *volatile ap_nif;
static netif_output_fn orig_out;
static volatile uint32_t ap_net;  // network byte order, /24
#define AP_MASK 0x00FFFFFFu
// Bytes per last address octet: up = sent by the device, down = sent to it.
static volatile uint32_t oct_up[256], oct_down[256];
static volatile uint32_t blocked_bits[8];
static volatile uint32_t dropped;

typedef struct {
  uint8_t mac[6];
  char host[33], vendor[33];
} dhcp_seen_t;
static dhcp_seen_t pend[4];
static volatile uint8_t pend_n;
static portMUX_TYPE pend_mux = portMUX_INITIALIZER_UNLOCKED;

static inline bool is_blocked(uint8_t o) { return blocked_bits[o >> 5] & (1u << (o & 31)); }

int devices_input_hook(struct pbuf *p, struct netif *inp) {
  if (!inp || inp != ap_nif || p->len < 20) return 0;
  const uint8_t *h = p->payload;
  if ((h[0] >> 4) != 4) return 0;
  uint32_t src, dst;
  memcpy(&src, h + 12, 4);
  memcpy(&dst, h + 16, 4);
  if ((src & AP_MASK) == ap_net) {
    uint8_t o = (uint8_t)(src >> 24);
    oct_up[o] += p->tot_len;
    // Blocked: nothing leaves the hotspot network (the admin page still works).
    if (is_blocked(o) && (dst & AP_MASK) != ap_net && dst != 0xFFFFFFFFu) {
      dropped++;
      pbuf_free(p);
      return 1;
    }
  }
  if (h[9] == 17) {
    dhcp_seen_t d;
    if (dhcp_client_info(h, p->len, d.mac, d.host, sizeof(d.host), d.vendor, sizeof(d.vendor)) &&
        (d.host[0] || d.vendor[0])) {
      portENTER_CRITICAL(&pend_mux);
      if (pend_n < 4) pend[pend_n++] = d;
      portEXIT_CRITICAL(&pend_mux);
    }
  }
  return 0;
}

static err_t out_ap(struct netif *n, struct pbuf *p, const ip4_addr_t *a) {
  if (p->len >= 20) {
    uint32_t dst;
    memcpy(&dst, (const uint8_t *)p->payload + 16, 4);
    if ((dst & AP_MASK) == ap_net) oct_down[(uint8_t)(dst >> 24)] += p->tot_len;
  }
  return orig_out ? orig_out(n, p, a) : ERR_IF;
}

static void wrap_ap(void *arg) {
  struct netif *n = ap_nif;
  if (n && n->output && n->output != out_ap) {
    orig_out = n->output;
    n->output = out_ap;
  }
}

// ---------- device table (devices task) ----------

typedef struct {
  uint8_t mac[6];
  uint8_t used, blocked;
  char name[32];       // set on the admin page
  uint64_t up, down;   // all-time bytes
  int64_t first, last; // unix time, 0 = before the clock was set
} prec_t;              // persisted part

typedef struct {
  prec_t p;
  char host[33], vendor[33];
  uint32_t ip;  // network byte order, 0 = none
  int8_t rssi;
  bool on;
  int64_t joined_us;
  uint64_t s_up, s_down;  // this connection
  uint32_t r_up, r_down;  // bytes per second, last interval
} pbdev_t;

static pbdev_t devs[MAX_DEV];
static SemaphoreHandle_t mtx;
static bool dirty;
static int64_t last_save_us;
static uint32_t last_up[256], last_down[256];

static void save(void) {
  prec_t *buf = calloc(MAX_DEV, sizeof(prec_t));
  if (!buf) return;
  for (int i = 0; i < MAX_DEV; i++) buf[i] = devs[i].p;
  nvs_handle_t h;
  if (nvs_open("devices", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_blob(h, "tbl", buf, MAX_DEV * sizeof(prec_t));
    nvs_commit(h);
    nvs_close(h);
  }
  free(buf);
  dirty = false;
  last_save_us = esp_timer_get_time();
}

static void load(void) {
  prec_t *buf = calloc(MAX_DEV, sizeof(prec_t));
  if (!buf) return;
  nvs_handle_t h;
  size_t len = MAX_DEV * sizeof(prec_t);
  if (nvs_open("devices", NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_blob(h, "tbl", buf, &len) == ESP_OK && len == MAX_DEV * sizeof(prec_t))
      for (int i = 0; i < MAX_DEV; i++) devs[i].p = buf[i];
    nvs_close(h);
  }
  free(buf);
}

static bool parse_mac(const char *s, uint8_t m[6]) {
  unsigned v[6];
  if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) {
    if (v[i] > 255) return false;
    m[i] = (uint8_t)v[i];
  }
  return true;
}

static pbdev_t *find(const uint8_t mac[6]) {
  for (int i = 0; i < MAX_DEV; i++)
    if (devs[i].p.used && !memcmp(devs[i].p.mac, mac, 6)) return &devs[i];
  return NULL;
}

// Finds or adds a device; a full table reuses the longest-unseen offline slot.
static pbdev_t *find_or_add(const uint8_t mac[6], bool *is_new) {
  pbdev_t *d = find(mac);
  *is_new = false;
  if (d) return d;
  pbdev_t *slot = NULL;
  for (int i = 0; i < MAX_DEV && !slot; i++)
    if (!devs[i].p.used) slot = &devs[i];
  // Table full: reuse an offline, unblocked entry; unnamed ones first, then the longest unseen.
  for (int i = 0; i < MAX_DEV && !slot; i++) {
    pbdev_t *x = &devs[i];
    if (x->on || x->p.blocked) continue;
    if (!slot || (slot->p.name[0] && !x->p.name[0]) || (!slot->p.name[0] == !x->p.name[0] && x->p.last < slot->p.last))
      slot = x;
  }
  if (!slot) return NULL;
  memset(slot, 0, sizeof(*slot));
  memcpy(slot->p.mac, mac, 6);
  slot->p.used = 1;
  slot->p.first = time(NULL) > 1600000000 ? time(NULL) : 0;
  *is_new = true;
  dirty = true;
  return slot;
}

static const char *label(const pbdev_t *d) {
  if (d->p.name[0]) return d->p.name;
  if (d->host[0]) return d->host;
  if (d->vendor[0]) return d->vendor;
  return "unnamed device";
}

static void rebuild_block_bits(void) {
  uint32_t bits[8] = {0};
  for (int i = 0; i < MAX_DEV; i++)
    if (devs[i].p.used && devs[i].p.blocked && devs[i].on && devs[i].ip) {
      uint8_t o = (uint8_t)(devs[i].ip >> 24);
      bits[o >> 5] |= 1u << (o & 31);
    }
  for (int i = 0; i < 8; i++) blocked_bits[i] = bits[i];
}

static void refresh(esp_netif_t *ap) {
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) list.num = 0;
  esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM];
  int n = list.num > ESP_WIFI_MAX_CONN_NUM ? ESP_WIFI_MAX_CONN_NUM : list.num;
  for (int i = 0; i < n; i++) {
    memcpy(pairs[i].mac, list.sta[i].mac, 6);
    pairs[i].ip.addr = 0;
  }
  if (n) esp_netif_dhcps_get_clients_by_mac(ap, n, pairs);
  bool clock = time(NULL) > 1600000000;
  int64_t now_us = esp_timer_get_time();

  xSemaphoreTake(mtx, portMAX_DELAY);
  bool present[MAX_DEV] = {0};
  for (int i = 0; i < n; i++) {
    bool fresh;
    pbdev_t *d = find_or_add(list.sta[i].mac, &fresh);
    if (!d) continue;
    present[d - devs] = true;
    if (!d->on) {
      d->on = true;
      d->joined_us = now_us;
      d->s_up = d->s_down = 0;
      if (fresh && cfg_bool("dev_alert")) {
        char detail[80];
        snprintf(detail, sizeof(detail), " (%s)", label(d));
        event_post_detail(detail, "A new device joined the hotspot");
      }
    }
    d->rssi = list.sta[i].rssi;
    if (pairs[i].ip.addr) d->ip = pairs[i].ip.addr;
    if (clock) d->p.last = time(NULL);
  }
  for (int i = 0; i < MAX_DEV; i++)
    if (devs[i].on && !present[i]) {
      devs[i].on = false;
      devs[i].r_up = devs[i].r_down = 0;
      dirty = true;
    }

  // Names learned from DHCP requests.
  dhcp_seen_t got[4];
  int gn;
  portENTER_CRITICAL(&pend_mux);
  gn = pend_n;
  memcpy(got, pend, sizeof(got));
  pend_n = 0;
  portEXIT_CRITICAL(&pend_mux);
  for (int i = 0; i < gn; i++) {
    pbdev_t *d = find(got[i].mac);
    if (!d) continue;
    if (got[i].host[0]) snprintf(d->host, sizeof(d->host), "%s", got[i].host);
    if (got[i].vendor[0]) snprintf(d->vendor, sizeof(d->vendor), "%s", got[i].vendor);
  }

  // Traffic since the last tick, credited to whoever holds each address now.
  for (int o = 1; o < 255; o++) {
    uint32_t u = oct_up[o], dn = oct_down[o];
    uint32_t du = u - last_up[o], dd = dn - last_down[o];
    last_up[o] = u, last_down[o] = dn;
    if (!du && !dd) continue;
    for (int i = 0; i < MAX_DEV; i++)
      if (devs[i].on && devs[i].ip && (uint8_t)(devs[i].ip >> 24) == o) {
        devs[i].p.up += du, devs[i].p.down += dd;
        devs[i].s_up += du, devs[i].s_down += dd;
        devs[i].r_up = du * 1000 / TICK_MS, devs[i].r_down = dd * 1000 / TICK_MS;
        dirty = true;
        break;
      }
  }
  for (int i = 0; i < MAX_DEV; i++) {
    // Zero the rate of devices that sent nothing this tick.
    static uint64_t prev_tot[MAX_DEV];
    uint64_t t = devs[i].s_up + devs[i].s_down;
    if (t == prev_tot[i]) devs[i].r_up = devs[i].r_down = 0;
    prev_tot[i] = t;
  }
  rebuild_block_bits();
  if (dirty && esp_timer_get_time() - last_save_us > 600LL * 1000000) save();
  xSemaphoreGive(mtx);
}

static void dev_task(void *arg) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    esp_netif_t *ap = netmgr_ap_netif();
    if (!ap) continue;
    ap_nif = esp_netif_get_netif_impl(ap);
    char ip[16];
    netmgr_ap_ip(ip, sizeof(ip));
    esp_ip4_addr_t a;
    if (esp_netif_str_to_ip4(ip, &a) == ESP_OK) ap_net = a.addr & AP_MASK;
    tcpip_callback(wrap_ap, NULL);
    refresh(ap);
  }
}

void devices_start(void) {
  mtx = xSemaphoreCreateMutex();
  load();
  last_save_us = esp_timer_get_time();
  xTaskCreatePinnedToCore(dev_task, "devices", 4096, NULL, 3, NULL, 1);
}

static void fmt_mac(const uint8_t *m, char *out) {
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

cJSON *devices_json(void) {
  cJSON *o = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(o, "devices");
  int64_t now_us = esp_timer_get_time();
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < MAX_DEV; i++) {
    pbdev_t *d = &devs[i];
    if (!d->p.used) continue;
    cJSON *j = cJSON_CreateObject();
    char mac[18], ip[16] = "";
    fmt_mac(d->p.mac, mac);
    if (d->ip) {
      esp_ip4_addr_t a = {.addr = d->ip};
      snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
    }
    cJSON_AddStringToObject(j, "mac", mac);
    cJSON_AddStringToObject(j, "name", d->p.name);
    cJSON_AddStringToObject(j, "host", d->host);
    cJSON_AddStringToObject(j, "vendor", d->vendor);
    cJSON_AddStringToObject(j, "ip", d->on ? ip : "");
    cJSON_AddBoolToObject(j, "connected", d->on);
    cJSON_AddBoolToObject(j, "blocked", d->p.blocked);
    // Locally administered MACs are the random "private addresses" phones use.
    cJSON_AddBoolToObject(j, "random_mac", d->p.mac[0] & 0x02);
    cJSON_AddNumberToObject(j, "rssi", d->on ? d->rssi : 0);
    cJSON_AddNumberToObject(j, "up", (double)d->p.up);
    cJSON_AddNumberToObject(j, "down", (double)d->p.down);
    cJSON_AddNumberToObject(j, "session_up", (double)d->s_up);
    cJSON_AddNumberToObject(j, "session_down", (double)d->s_down);
    cJSON_AddNumberToObject(j, "rate_up", d->r_up);
    cJSON_AddNumberToObject(j, "rate_down", d->r_down);
    cJSON_AddNumberToObject(j, "connected_s", d->on ? (double)((now_us - d->joined_us) / 1000000) : 0);
    cJSON_AddNumberToObject(j, "first_seen", (double)d->p.first);
    cJSON_AddNumberToObject(j, "last_seen", (double)d->p.last);
    cJSON_AddItemToArray(arr, j);
  }
  cJSON_AddNumberToObject(o, "dropped_packets", dropped);
  xSemaphoreGive(mtx);
  cJSON_AddNumberToObject(o, "max_devices", cfg_int("ap_maxcli"));
  return o;
}

bool devices_set(const char *mac_s, const char *name, int blocked, char *err, size_t errcap) {
  uint8_t mac[6];
  if (!parse_mac(mac_s, mac)) {
    snprintf(err, errcap, "bad device address");
    return false;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  pbdev_t *d = find(mac);
  if (d) {
    if (name) snprintf(d->p.name, sizeof(d->p.name), "%s", name);
    if (blocked >= 0) d->p.blocked = blocked ? 1 : 0;
    rebuild_block_bits();
    save();
  }
  xSemaphoreGive(mtx);
  if (!d) snprintf(err, errcap, "unknown device");
  else if (blocked >= 0) devlog("devices: a device was %s", blocked ? "blocked" : "unblocked");
  return d != NULL;
}

bool devices_kick(const char *mac_s, char *err, size_t errcap) {
  uint8_t mac[6];
  uint16_t aid = 0;
  if (!parse_mac(mac_s, mac) || esp_wifi_ap_get_sta_aid(mac, &aid) != ESP_OK || !aid) {
    snprintf(err, errcap, "device is not connected");
    return false;
  }
  esp_err_t e = esp_wifi_deauth_sta(aid);
  snprintf(err, errcap, "%s", e == ESP_OK ? "Disconnected. It may join again by itself; use Block to keep it off the internet." : esp_err_to_name(e));
  return e == ESP_OK;
}

bool devices_forget(const char *mac_s, char *err, size_t errcap) {
  uint8_t mac[6];
  if (!parse_mac(mac_s, mac)) {
    snprintf(err, errcap, "bad device address");
    return false;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  pbdev_t *d = find(mac);
  bool ok = d && !d->on;
  if (ok) {
    memset(d, 0, sizeof(*d));
    rebuild_block_bits();
    save();
  }
  xSemaphoreGive(mtx);
  if (!ok) snprintf(err, errcap, d ? "disconnect it first" : "unknown device");
  return ok;
}

// Counts only: names and addresses of people's devices stay out of reports.
void devices_summary(char *out, size_t cap) {
  int known = 0, on = 0, blk = 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < MAX_DEV; i++)
    if (devs[i].p.used) {
      known++;
      on += devs[i].on;
      blk += devs[i].p.blocked;
    }
  xSemaphoreGive(mtx);
  snprintf(out, cap, "devices: %d known, %d connected, %d blocked, %lu packets dropped\n", known, on, blk,
           (unsigned long)dropped);
}
