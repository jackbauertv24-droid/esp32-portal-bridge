#include "survey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devlog.h"
#include "esp_wifi.h"
#include "settings.h"

#define MAX_REC 64

static const char *auth_name(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_OWE: return "OWE (enhanced open)";
    default: return "other";
  }
}

static const char *cipher_name(wifi_cipher_type_t c) {
  switch (c) {
    case WIFI_CIPHER_TYPE_NONE: return "none";
    case WIFI_CIPHER_TYPE_WEP40:
    case WIFI_CIPHER_TYPE_WEP104: return "WEP";
    case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP: return "AES";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP+AES";
    default: return "other";
  }
}

cJSON *survey_json(void) {
  cJSON *o = cJSON_CreateObject();
  wifi_scan_config_t sc = {.show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
  sc.scan_time.active.min = 60;
  sc.scan_time.active.max = 140;
  esp_err_t e = esp_wifi_scan_start(&sc, true);
  if (e != ESP_OK) {
    char m[120];
    snprintf(m, sizeof(m), "Scan not possible right now (%s). The board may be joining a network; try again in a few seconds.",
             esp_err_to_name(e));
    cJSON_AddStringToObject(o, "error", m);
    return o;
  }
  uint16_t total = 0;
  esp_wifi_scan_get_ap_num(&total);
  uint16_t n = MAX_REC;
  wifi_ap_record_t *r = calloc(n, sizeof(wifi_ap_record_t));
  if (!r || esp_wifi_scan_get_ap_records(&n, r) != ESP_OK) n = 0;
  cJSON *arr = cJSON_AddArrayToObject(o, "networks");
  for (int i = 0; i < n; i++) {
    cJSON *j = cJSON_CreateObject();
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", r[i].bssid[0], r[i].bssid[1], r[i].bssid[2],
             r[i].bssid[3], r[i].bssid[4], r[i].bssid[5]);
    cJSON_AddStringToObject(j, "ssid", (const char *)r[i].ssid);
    cJSON_AddStringToObject(j, "bssid", b);
    cJSON_AddNumberToObject(j, "rssi", r[i].rssi);
    cJSON_AddNumberToObject(j, "channel", r[i].primary);
    // 40 MHz networks also occupy the channel 4 above or below.
    cJSON_AddNumberToObject(j, "second", r[i].second == WIFI_SECOND_CHAN_ABOVE ? 1 : r[i].second == WIFI_SECOND_CHAN_BELOW ? -1 : 0);
    cJSON_AddStringToObject(j, "auth", auth_name(r[i].authmode));
    cJSON_AddBoolToObject(j, "open", r[i].authmode == WIFI_AUTH_OPEN);
    cJSON_AddStringToObject(j, "cipher", cipher_name(r[i].pairwise_cipher));
    char phy[24] = "";
    if (r[i].phy_11b) strcat(phy, "b");
    if (r[i].phy_11g) strcat(phy, "g");
    if (r[i].phy_11n) strcat(phy, "n");
    if (r[i].phy_11ax) strcat(phy, phy[0] ? "/ax" : "ax");
    if (r[i].phy_lr) strcat(phy, " LR");
    cJSON_AddStringToObject(j, "phy", phy);
    cJSON_AddBoolToObject(j, "wps", r[i].wps);
    char cc[3] = {r[i].country.cc[0], r[i].country.cc[1], 0};
    cJSON_AddStringToObject(j, "country", (cc[0] >= 'A' && cc[0] <= 'Z') ? cc : "");
    cJSON_AddItemToArray(arr, j);
  }
  free(r);
  cJSON_AddNumberToObject(o, "seen", total);
  wifi_ap_record_t cur;
  if (esp_wifi_sta_get_ap_info(&cur) == ESP_OK) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", cur.bssid[0], cur.bssid[1], cur.bssid[2], cur.bssid[3],
             cur.bssid[4], cur.bssid[5]);
    cJSON_AddStringToObject(o, "upstream_bssid", b);
  }
  uint8_t ch = 0;
  wifi_second_chan_t sec;
  if (esp_wifi_get_channel(&ch, &sec) == ESP_OK) cJSON_AddNumberToObject(o, "radio_channel", ch);
  cJSON_AddNumberToObject(o, "ap_channel_setting", cfg_int("ap_chan"));
  cJSON *saved = cJSON_AddArrayToObject(o, "saved");
  for (int i = 1; i <= 3; i++) {
    char k[12], v[33];
    snprintf(k, sizeof(k), "w%d_ssid", i);
    cfg_get(k, v, sizeof(v));
    cJSON_AddItemToArray(saved, cJSON_CreateString(v));
  }
  devlog("survey: %u networks seen", (unsigned)total);
  return o;
}
