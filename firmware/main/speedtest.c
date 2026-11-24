#include "speedtest.h"

#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "devlog.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lte.h"
#include "modem_at.h"
#include "netmgr.h"
#include "settings.h"

#define HIST_FILE "/data/speed.jsonl"
#define HIST_MAX 100
#define CHUNK 4096

static volatile bool running, stop_req;
static char phase[24] = "idle", cur_via[8] = "";
static volatile double live_mbps;
static volatile uint32_t live_bytes;
static char want[8];

static double now_s(void) { return esp_timer_get_time() / 1e6; }

static bool ifname_for(const char *via, struct ifreq *ifr) {
  memset(ifr, 0, sizeof(*ifr));
  if (!strcmp(via, "wifi")) return netmgr_sta_connected() && netmgr_sta_ifname(ifr->ifr_name, sizeof(ifr->ifr_name));
  esp_netif_t *l = lte_netif();
  return lte_up() && l && esp_netif_get_netif_impl_name(l, ifr->ifr_name) == ESP_OK;
}

// Replaces {bytes} in the configured URL.
static void url_with_bytes(const char *key, long bytes, char *out, size_t cap) {
  char tmpl[220], n[16];
  cfg_get(key, tmpl, sizeof(tmpl));
  snprintf(n, sizeof(n), "%ld", bytes);
  const char *p = strstr(tmpl, "{bytes}");
  if (p) snprintf(out, cap, "%.*s%s%s", (int)(p - tmpl), tmpl, n, p + 7);
  else snprintf(out, cap, "%s", tmpl);
}

static esp_http_client_handle_t client(const char *url, struct ifreq *ifr, esp_http_client_method_t m) {
  esp_http_client_config_t c = {
      .url = url,
      .method = m,
      .timeout_ms = 10000,
      .buffer_size = CHUNK,
      .buffer_size_tx = 2048,
      .keep_alive_enable = true,
      .if_name = ifr,
      .user_agent = "portal-bridge-speedtest",
  };
  if (!strncasecmp(url, "https:", 6)) c.crt_bundle_attach = esp_crt_bundle_attach;
  return esp_http_client_init(&c);
}

// Round-trip time of small requests on one kept-alive connection; the first
// (connection setup) is not counted. Returns median ms, jitter in *jit.
static double latency(struct ifreq *ifr, double *jit, char *err, size_t ecap) {
  char url[240];
  url_with_bytes("sp_down_url", 0, url, sizeof(url));
  esp_http_client_handle_t c = client(url, ifr, HTTP_METHOD_GET);
  if (!c) return -1;
  double t[6];
  int n = 0;
  // perform() keeps the connection open between requests, so after the first
  // one each round trip is one request and one answer.
  for (int i = 0; i < 6 && !stop_req; i++) {
    double s = now_s();
    if (esp_http_client_perform(c) != ESP_OK) {
      snprintf(err, ecap, "cannot reach the test server");
      break;
    }
    double ms = (now_s() - s) * 1000;
    if (i > 0) t[n++] = ms;
  }
  esp_http_client_cleanup(c);
  if (n == 0) return -1;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (t[j] < t[i]) {
        double x = t[i];
        t[i] = t[j], t[j] = x;
      }
  double j = 0;
  for (int i = 1; i < n; i++) j += t[i] - t[i - 1];
  *jit = n > 1 ? j / (n - 1) : 0;
  return t[n / 2];
}

static double download(struct ifreq *ifr, long bytes, int max_s, double *mb, char *err, size_t ecap) {
  char url[240];
  url_with_bytes("sp_down_url", bytes, url, sizeof(url));
  esp_http_client_handle_t c = client(url, ifr, HTTP_METHOD_GET);
  if (!c) return -1;
  char *buf = malloc(CHUNK);
  double res = -1;
  if (buf && esp_http_client_open(c, 0) == ESP_OK) {
    int64_t len = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
      snprintf(err, ecap, "download: server answered %d", status);
    } else {
      double s = now_s(), el = 0;
      uint64_t got = 0;
      int n;
      while (!stop_req && (n = esp_http_client_read(c, buf, CHUNK)) > 0) {
        got += (uint64_t)n;
        el = now_s() - s;
        live_bytes = (uint32_t)got;
        if (el > 0.3) live_mbps = got * 8 / el / 1e6;
        if (el > max_s) break;
      }
      el = now_s() - s;
      if (got > 0 && el > 0) res = got * 8 / el / 1e6;
      *mb = got / 1048576.0;
      if (got == 0) snprintf(err, ecap, "download: no data received");
      (void)len;
    }
  } else if (!err[0]) {
    snprintf(err, ecap, "download: cannot reach the test server");
  }
  free(buf);
  esp_http_client_cleanup(c);
  return res;
}

static double upload(struct ifreq *ifr, long bytes, int max_s, double *mb, char *err, size_t ecap) {
  char url[240];
  url_with_bytes("sp_up_url", bytes, url, sizeof(url));
  esp_http_client_handle_t c = client(url, ifr, HTTP_METHOD_POST);
  if (!c) return -1;
  esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
  char *buf = calloc(1, CHUNK);
  double res = -1;
  if (buf && esp_http_client_open(c, (int)bytes) == ESP_OK) {
    double s = now_s(), el = 0;
    long sent = 0;
    bool full = true;
    while (sent < bytes && !stop_req) {
      int want_n = bytes - sent > CHUNK ? CHUNK : (int)(bytes - sent);
      int n = esp_http_client_write(c, buf, want_n);
      if (n <= 0) {
        full = false;
        break;
      }
      sent += n;
      el = now_s() - s;
      live_bytes = (uint32_t)sent;
      if (el > 0.3) live_mbps = sent * 8 / el / 1e6;
      if (el > max_s) {
        full = false;
        break;
      }
    }
    if (full) esp_http_client_fetch_headers(c);  // wait for the server to take it all
    el = now_s() - s;
    if (sent > 0 && el > 0) res = sent * 8 / el / 1e6;
    *mb = sent / 1048576.0;
    if (sent == 0) snprintf(err, ecap, "upload: nothing could be sent");
  } else {
    snprintf(err, ecap, "upload: cannot reach the test server");
  }
  free(buf);
  esp_http_client_cleanup(c);
  return res;
}

static void set_phase(const char *p) {
  snprintf(phase, sizeof(phase), "%s", p);
  live_mbps = 0;
  live_bytes = 0;
}

static void append_history(cJSON *r) {
  // Keep the newest HIST_MAX lines: read the tail of the old file, then rewrite.
  char *line = cJSON_PrintUnformatted(r);
  if (!line) return;
  FILE *f = fopen(HIST_FILE, "r");
  char **keep = calloc(HIST_MAX, sizeof(char *));
  int n = 0;
  if (f && keep) {
    char *b = malloc(600);
    while (b && fgets(b, 600, f)) {
      if (n == HIST_MAX - 1) {
        free(keep[0]);
        memmove(keep, keep + 1, sizeof(char *) * (HIST_MAX - 2));
        n--;
      }
      keep[n++] = strdup(b);
    }
    free(b);
  }
  if (f) fclose(f);
  f = fopen(HIST_FILE, "w");
  if (f) {
    for (int i = 0; i < n; i++) fputs(keep[i], f);
    fprintf(f, "%s\n", line);
    fclose(f);
  }
  for (int i = 0; i < n; i++) free(keep[i]);
  free(keep);
  free(line);
}

static void run_one(const char *via) {
  snprintf(cur_via, sizeof(cur_via), "%s", via);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "t", (double)time(NULL));
  cJSON_AddStringToObject(r, "via", via);
  char err[120] = "";
  struct ifreq ifr;
  if (!ifname_for(via, &ifr)) {
    snprintf(err, sizeof(err), "%s is not connected", strcmp(via, "wifi") ? "4G" : "upstream Wi-Fi");
  } else {
    // Where the test ran: signal and network, for comparing results later.
    if (!strcmp(via, "wifi")) {
      wifi_ap_record_t ai;
      if (esp_wifi_sta_get_ap_info(&ai) == ESP_OK) {
        cJSON_AddStringToObject(r, "network", (const char *)ai.ssid);
        cJSON_AddNumberToObject(r, "signal_dbm", ai.rssi);
      }
    } else {
      int csq = 99;
      char oper[40] = "";
      modem_radio(&csq, oper, sizeof(oper), NULL, 0);
      cJSON_AddStringToObject(r, "network", oper);
      if (csq >= 0 && csq <= 31) cJSON_AddNumberToObject(r, "signal_dbm", -113 + 2 * csq);
    }
    bool lte = strcmp(via, "wifi") != 0;
    int max_s = cfg_int("sp_max_s");
    long down_b = (long)cfg_int(lte ? "sp_lte_down" : "sp_down_mb") * 1048576L;
    long up_b = (long)cfg_int(lte ? "sp_lte_up" : "sp_up_mb") * 1048576L;
    double jit = 0, mb = 0;
    set_phase("latency");
    double ms = latency(&ifr, &jit, err, sizeof(err));
    if (ms >= 0) {
      cJSON_AddNumberToObject(r, "ping_ms", ms);
      cJSON_AddNumberToObject(r, "jitter_ms", jit);
    }
    if (!stop_req && !err[0] && down_b > 0) {
      set_phase("download");
      double d = download(&ifr, down_b, max_s, &mb, err, sizeof(err));
      if (d >= 0) cJSON_AddNumberToObject(r, "down_mbps", d);
      cJSON_AddNumberToObject(r, "down_mb", mb);
    }
    if (!stop_req && !err[0] && up_b > 0) {
      set_phase("upload");
      mb = 0;
      double u = upload(&ifr, up_b, max_s, &mb, err, sizeof(err));
      if (u >= 0) cJSON_AddNumberToObject(r, "up_mbps", u);
      cJSON_AddNumberToObject(r, "up_mb", mb);
    }
  }
  if (stop_req && !err[0]) snprintf(err, sizeof(err), "stopped");
  if (err[0]) cJSON_AddStringToObject(r, "error", err);
  double dm = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "down_mbps")), um = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "up_mbps"));
  devlog("speedtest %s: down %.1f up %.1f Mbit/s%s%s", via, dm == dm ? dm : 0, um == um ? um : 0, err[0] ? " - " : "", err);
  append_history(r);
  cJSON_Delete(r);
}

static void task(void *arg) {
  if (!strcmp(want, "both")) {
    run_one("wifi");
    if (!stop_req) run_one("4g");
  } else {
    run_one(want);
  }
  set_phase("idle");
  cur_via[0] = 0;
  running = false;
  vTaskDelete(NULL);
}

bool speedtest_start(const char *via, char *err, size_t errcap) {
  if (running) {
    snprintf(err, errcap, "A test is already running.");
    return false;
  }
  if (strcmp(via, "wifi") && strcmp(via, "4g") && strcmp(via, "both")) {
    snprintf(err, errcap, "choose wifi, 4g or both");
    return false;
  }
  snprintf(want, sizeof(want), "%s", via);
  stop_req = false;
  running = true;
  set_phase("starting");
  if (xTaskCreatePinnedToCore(task, "speedtest", 10240, NULL, 2, NULL, 1) != pdPASS) {
    running = false;
    snprintf(err, errcap, "out of memory");
    return false;
  }
  return true;
}

void speedtest_stop(void) { stop_req = true; }

void speedtest_clear(void) { remove(HIST_FILE); }

cJSON *speedtest_json(void) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "running", running);
  cJSON_AddStringToObject(o, "phase", phase);
  cJSON_AddStringToObject(o, "via", cur_via);
  cJSON_AddNumberToObject(o, "live_mbps", live_mbps);
  cJSON_AddNumberToObject(o, "bytes", live_bytes);
  cJSON_AddBoolToObject(o, "wifi_ready", netmgr_sta_connected());
  cJSON_AddBoolToObject(o, "lte_ready", lte_up());
  cJSON *h = cJSON_AddArrayToObject(o, "history");
  FILE *f = fopen(HIST_FILE, "r");
  if (f) {
    char *b = malloc(600);
    while (b && fgets(b, 600, f)) {
      cJSON *j = cJSON_Parse(b);
      if (j) cJSON_AddItemToArray(h, j);
    }
    free(b);
    fclose(f);
  }
  return o;
}
