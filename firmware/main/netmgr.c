#include "netmgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "devlog.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "html_form.h"
#include "httpc.h"
#include "lb.h"
#include "meter.h"
#include "lte.h"
#include "settings.h"
#include "urlutil.h"

static esp_netif_t *ap_if, *sta_if;
static volatile bool sta_connected, sta_has_ip;
static char sta_ssid[33], sta_ip[16];
static int sta_slot = -1;  // which of w1..w3 is in use
static volatile net_state_t wifi_state, lte_state;
static char portal_url[1024];
static volatile bool recheck_now, reconnect_now;
static const char *uplink = "none";
static char ap_ip[16];
static bool sntp_started;
static int64_t last_wifi_probe_us, last_lte_probe_us;
static unsigned sta_fail_streak;
static int napt_on = 1;  // hotspot clients may reach the internet

// ---------- hotspot ----------

static void apply_ap_ip(const char *ip) {
  esp_netif_ip_info_t info = {0};
  esp_netif_str_to_ip4(ip, &info.ip);
  info.gw = info.ip;
  esp_netif_str_to_ip4("255.255.255.0", &info.netmask);
  esp_netif_napt_disable(ap_if);
  esp_netif_dhcps_stop(ap_if);
  esp_netif_set_ip_info(ap_if, &info);
  esp_netif_dhcps_start(ap_if);
  esp_netif_napt_enable(ap_if);
  snprintf(ap_ip, sizeof(ap_ip), "%s", ip);
  napt_on = 1;
}

// Hands the uplink's DNS server to hotspot clients via DHCP.
static void offer_dns_from(esp_netif_t *nif) {
  esp_netif_dns_info_t dns;
  if (!nif || esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK || dns.ip.u_addr.ip4.addr == 0)
    return;
  esp_netif_set_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns);  // also makes it the board's resolver
  uint8_t offer = 1;
  esp_netif_dhcps_stop(ap_if);
  esp_netif_dhcps_option(ap_if, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
  esp_netif_set_dns_info(ap_if, ESP_NETIF_DNS_MAIN, &dns);
  lb_set_dns(dns.ip.u_addr.ip4.addr);
  esp_netif_dhcps_start(ap_if);
}

// ---------- upstream Wi-Fi ----------

static bool slot_ssid(int slot, char *ssid, size_t cap, char *pass, size_t pcap) {
  char k1[12], k2[12];
  snprintf(k1, sizeof(k1), "w%d_ssid", slot + 1);
  snprintf(k2, sizeof(k2), "w%d_pass", slot + 1);
  cfg_get(k1, ssid, cap);
  cfg_get(k2, pass, pcap);
  return ssid[0] != 0;
}

static void connect_next(void) {
  char ssid[33], pass[64];
  for (int i = 1; i <= 3; i++) {
    int slot = (sta_slot + i) % 3;
    if (!slot_ssid(slot, ssid, sizeof(ssid), pass, sizeof(pass))) continue;
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_slot = slot;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    devlog("wifi: connecting to upstream \"%s\" (slot %d)", ssid, slot + 1);
    esp_wifi_connect();
    return;
  }
  sta_slot = -1;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = data;
    if (sta_connected) event_post("Upstream Wi-Fi \"%s\" disconnected (reason %d)", sta_ssid, d->reason);
    else devlog("wifi: connect to slot %d failed (reason %d)", sta_slot + 1, d->reason);
    sta_connected = sta_has_ip = false;
    sta_ip[0] = 0;
    wifi_state = NET_UNKNOWN;
    sta_fail_streak++;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    wifi_event_sta_connected_t *c = data;
    snprintf(sta_ssid, sizeof(sta_ssid), "%.*s", c->ssid_len, (const char *)c->ssid);
    sta_connected = true;
    sta_fail_streak = 0;
    devlog("wifi: associated with \"%s\" on channel %d", sta_ssid, c->channel);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *e = data;
    snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
    sta_has_ip = true;
    event_post("Joined upstream Wi-Fi \"%s\" as %s", sta_ssid, sta_ip);
    // Move the hotspot if the upstream network uses the same /24.
    esp_ip4_addr_t a;
    esp_netif_str_to_ip4(ap_ip, &a);
    if ((a.addr & 0x00FFFFFF) == (e->ip_info.ip.addr & 0x00FFFFFF)) {
      const char *alt = strncmp(ap_ip, "10.42.", 6) ? "10.42.0.1" : "192.168.77.1";
      event_post("Upstream network uses %s too; admin address moved to %s", ap_ip, alt);
      apply_ap_ip(alt);
    }
    recheck_now = true;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *e = data;
    devlog("hotspot: device " MACSTR " joined", MAC2STR(e->mac));
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *e = data;
    devlog("hotspot: device " MACSTR " left", MAC2STR(e->mac));
  }
}

bool netmgr_sta_ifname(char *out, size_t cap) {
  if (!sta_if || cap < 6) return false;
  return esp_netif_get_netif_impl_name(sta_if, out) == ESP_OK;
}

// ---------- internet checks ----------

static net_state_t probe(esp_netif_t *nif, bool remember_portal) {
  if (!nif) return NET_OFFLINE;
  httpc_session_t *s = calloc(1, sizeof(httpc_session_t));
  if (!s) return NET_UNKNOWN;
  httpc_session_init(s, false);
  s->quiet = true;
  char name[8];
  if (esp_netif_get_netif_impl_name(nif, name) == ESP_OK) {
    s->bind = true;
    snprintf(s->ifr.ifr_name, sizeof(s->ifr.ifr_name), "%s", name);
  }
  char url[220];
  cfg_get("probe_url", url, sizeof(url));
  httpc_resp_t r = {0};
  httpc_request(s, "GET", url, NULL, NULL, 0, NULL, 0, &r);
  net_state_t st;
  if (r.status == 204 || (r.status == 200 && r.body_len == 0)) {
    st = NET_ONLINE;
  } else if (r.status < 0) {
    st = NET_OFFLINE;
  } else {
    st = NET_CAPTIVE;
    if (remember_portal) {
      char found[1024] = "", abs[1024];
      if (r.location[0]) snprintf(found, sizeof(found), "%s", r.location);
      else if (r.body && hf_find_redirect(r.body, found, sizeof(found))) {
      } else snprintf(found, sizeof(found), "%s", url);
      if (url_resolve(url, found, abs, sizeof(abs)) == 0) snprintf(portal_url, sizeof(portal_url), "%s", abs);
    }
  }
  httpc_resp_free(&r);
  free(s);
  return st;
}

static const char *state_name(net_state_t s) {
  switch (s) {
    case NET_ONLINE: return "online";
    case NET_CAPTIVE: return "login page";
    case NET_OFFLINE: return "no internet";
    default: return "unknown";
  }
}

// ---------- uplink policy ----------

static void select_uplink(void) {
  char mode[20];
  cfg_get("up_mode", mode, sizeof(mode));
  esp_netif_t *ppp = lte_netif();
  bool capped = meter_lte_blocked();  // data limit reached: 4G only for the board itself
  bool lte_ok = lte_up() && ppp && lte_state != NET_OFFLINE && !capped;
  bool lte_board = lte_up() && ppp && capped;
  bool wifi_ok = sta_has_ip && wifi_state == NET_ONLINE;
  esp_netif_t *sel = NULL;
  const char *name = "none";
  if (!strcmp(mode, "wifi")) {
    if (sta_has_ip) sel = sta_if, name = "wifi";
  } else if (!strcmp(mode, "4g")) {
    if (lte_up() && ppp && !capped) sel = ppp, name = "4g";
    else if (lte_board) sel = ppp, name = "4g-board";
  } else if (!strcmp(mode, "4g_then_wifi")) {
    if (lte_ok) sel = ppp, name = "4g";
    else if (sta_has_ip) sel = sta_if, name = "wifi";
    else if (lte_board) sel = ppp, name = "4g-board";
  } else {  // wifi_then_4g
    if (wifi_ok) sel = sta_if, name = "wifi";
    else if (lte_ok) sel = ppp, name = "4g";
    else if (sta_has_ip) sel = sta_if, name = "wifi";
    else if (lte_board) sel = ppp, name = "4g-board";
  }
  if (strcmp(name, uplink)) {
    event_post("Uplink: %s -> %s", uplink, name);
    uplink = name;
    if (sel) {
      esp_netif_set_default_netif(sel);
      offer_dns_from(sel);
    }
  }
  // At a 4G data limit with no Wi-Fi, hotspot devices are cut off (no NAT)
  // while the board keeps 4G for its own alerts and SMS forwarding.
  int want_napt = strcmp(uplink, "4g-board") != 0;
  if (want_napt != napt_on) {
    if (want_napt) esp_netif_napt_enable(ap_if);
    else esp_netif_napt_disable(ap_if);
    napt_on = want_napt;
    event_post(want_napt ? "Hotspot devices have internet again" : "4G limit reached: hotspot devices are offline until Wi-Fi is available or you allow more 4G");
  }
  if (!sntp_started && sel && netmgr_online()) {
    char ntp[64];
    cfg_get("ntp", ntp, sizeof(ntp));
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp);
    sc.start = true;
    esp_netif_sntp_init(&sc);
    sntp_started = true;
  }
}

static void net_task(void *arg) {
  int64_t next_connect = 0;
  for (;;) {
    int64_t now = esp_timer_get_time();
    int period = cfg_int("probe_s");
    if (reconnect_now) {
      reconnect_now = false;
      sta_slot = -1;
      esp_wifi_disconnect();
      next_connect = 0;
    }
    if (!sta_connected && now >= next_connect) {
      connect_next();
      // Back off after a full round of failures so scanning does not disturb hotspot clients.
      next_connect = now + (sta_fail_streak >= 3 ? 60LL : 15LL) * 1000000;
    }
    if (sta_has_ip && (recheck_now || now - last_wifi_probe_us > (int64_t)period * 1000000)) {
      net_state_t prev = wifi_state;
      wifi_state = probe(sta_if, true);
      last_wifi_probe_us = esp_timer_get_time();
      if (wifi_state != prev) {
        if (wifi_state == NET_CAPTIVE) event_post("Upstream Wi-Fi \"%s\" shows a login page", sta_ssid);
        else event_post("Upstream Wi-Fi \"%s\": %s", sta_ssid, state_name(wifi_state));
      }
    }
    if (lte_up() && (recheck_now || now - last_lte_probe_us > (int64_t)period * 2 * 1000000)) {
      lte_state = probe(lte_netif(), false);
      last_lte_probe_us = esp_timer_get_time();
    }
    if (!lte_up()) lte_state = NET_UNKNOWN;
    recheck_now = false;
    select_uplink();
    char apip[16];
    netmgr_ap_ip(apip, sizeof(apip));
    meter_set_uplinks(sta_if, lte_netif());
    lb_update(sta_if, sta_has_ip && wifi_state == NET_ONLINE, lte_netif(),
              lte_up() && lte_state != NET_OFFLINE && !meter_lte_blocked(), apip);
    for (int i = 0; i < 20 && !recheck_now && !reconnect_now; i++) vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void netmgr_start(void) {
  char tz[48];
  cfg_get("tz", tz, sizeof(tz));
  setenv("TZ", tz, 1);
  tzset();

  ap_if = esp_netif_create_default_wifi_ap();
  sta_if = esp_netif_create_default_wifi_sta();
  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&ic));
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_APSTA);

  char mac[20];
  cfg_get("sta_mac", mac, sizeof(mac));
  if (mac[0]) {
    unsigned m[6];
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
      uint8_t b[6];
      for (int i = 0; i < 6; i++) b[i] = (uint8_t)m[i];
      b[0] &= 0xFE;  // unicast
      esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, b);
      devlog("wifi: upstream MAC override %s: %s", mac, esp_err_to_name(e));
    }
  }

  wifi_config_t ac = {0};
  char ssid[33], pass[64];
  cfg_get("ap_ssid", ssid, sizeof(ssid));
  cfg_get("ap_pass", pass, sizeof(pass));
  snprintf((char *)ac.ap.ssid, sizeof(ac.ap.ssid), "%s", ssid);
  ac.ap.ssid_len = (uint8_t)strlen(ssid);
  snprintf((char *)ac.ap.password, sizeof(ac.ap.password), "%s", pass);
  ac.ap.channel = (uint8_t)cfg_int("ap_chan");
  ac.ap.max_connection = (uint8_t)cfg_int("ap_maxcli");
  ac.ap.ssid_hidden = cfg_bool("ap_hidden") ? 1 : 0;
  ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ac.ap.pmf_cfg.required = false;
  esp_wifi_set_config(WIFI_IF_AP, &ac);
  ESP_ERROR_CHECK(esp_wifi_start());

  char ip[16];
  cfg_get("ap_ip", ip, sizeof(ip));
  apply_ap_ip(ip);
  devlog("hotspot: \"%s\" on channel %d, admin at http://%s/", ssid, ac.ap.channel, ap_ip);
  xTaskCreatePinnedToCore(net_task, "netmgr", 16384, NULL, 4, NULL, 1);
}

bool netmgr_sta_connected(void) { return sta_has_ip; }
net_state_t netmgr_wifi_state(void) { return wifi_state; }
void netmgr_portal_url(char *out, size_t cap) { snprintf(out, cap, "%s", portal_url); }
void netmgr_recheck(void) { recheck_now = true; }
void netmgr_reconnect(void) { reconnect_now = true; }
void netmgr_ap_ip(char *out, size_t cap) { snprintf(out, cap, "%s", ap_ip); }

bool netmgr_online(void) {
  if (!strcmp(uplink, "wifi")) return wifi_state == NET_ONLINE;
  if (!strcmp(uplink, "4g") || !strcmp(uplink, "4g-board")) return lte_up() && lte_state != NET_OFFLINE;
  return false;
}

void netmgr_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "net");
  cJSON_AddStringToObject(o, "uplink", uplink);
  cJSON_AddBoolToObject(o, "online", netmgr_online());
  cJSON_AddBoolToObject(o, "wifi_connected", sta_has_ip);
  cJSON_AddStringToObject(o, "wifi_ssid", sta_connected ? sta_ssid : "");
  cJSON_AddStringToObject(o, "wifi_ip", sta_ip);
  cJSON_AddStringToObject(o, "wifi_state", state_name(wifi_state));
  cJSON_AddStringToObject(o, "lte_state", state_name(lte_state));
  wifi_ap_record_t ai;
  cJSON_AddNumberToObject(o, "wifi_rssi", sta_connected && esp_wifi_sta_get_ap_info(&ai) == ESP_OK ? ai.rssi : 0);
  cJSON_AddStringToObject(o, "portal_url", portal_url);
  wifi_sta_list_t list;
  cJSON_AddNumberToObject(o, "clients", esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0);
  cJSON_AddStringToObject(o, "ap_ip", ap_ip);
  char ssid[33];
  cfg_get("ap_ssid", ssid, sizeof(ssid));
  cJSON_AddStringToObject(o, "ap_ssid", ssid);
  time_t t = time(NULL);
  cJSON_AddNumberToObject(o, "time", (double)t);
}

void netmgr_summary(char *out, size_t cap) {
  snprintf(out, cap, "uplink: %s, online: %d\nwifi: connected %d ssid \"%s\" ip %s state %s\nlte state: %s\n"
           "portal url: %s\nap ip: %s\n",
           uplink, netmgr_online(), sta_has_ip, sta_ssid, sta_ip, state_name(wifi_state), state_name(lte_state),
           portal_url, ap_ip);
}
