#include "meter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "devlog.h"
#include "esp_netif_net_stack.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "nvs.h"
#include "settings.h"

// ---------- counting (lwIP thread) ----------

static struct netif *volatile m_wifi, *volatile m_lte;
static netif_output_fn orig_wifi, orig_lte;
// Raw byte counters since boot; 32-bit so reads are atomic. The meter task
// takes deltas every 2 s, so wrap-around is harmless.
static volatile uint32_t rx_w, tx_w, rx_l, tx_l;

int pb_input_hook(struct pbuf *p, struct netif *inp) {
  if (inp && inp == m_lte) rx_l += p->tot_len;
  else if (inp && inp == m_wifi) rx_w += p->tot_len;
  return 0;  // never consume the packet
}

static err_t out_wifi(struct netif *n, struct pbuf *p, const ip4_addr_t *a) {
  tx_w += p->tot_len;
  return orig_wifi ? orig_wifi(n, p, a) : ERR_IF;
}

static err_t out_lte(struct netif *n, struct pbuf *p, const ip4_addr_t *a) {
  tx_l += p->tot_len;
  return orig_lte ? orig_lte(n, p, a) : ERR_IF;
}

// Runs in the lwIP thread: (re)installs the output wrappers. PPP may replace
// its output function when it reconnects, so this is repeated.
static void wrap_outputs(void *arg) {
  struct netif *w = m_wifi, *l = m_lte;
  if (w && w->output && w->output != out_wifi) {
    orig_wifi = w->output;
    w->output = out_wifi;
  }
  if (l && l->output && l->output != out_lte) {
    orig_lte = l->output;
    l->output = out_lte;
  }
}

void meter_set_uplinks(esp_netif_t *wifi, esp_netif_t *lte) {
  m_wifi = wifi ? (struct netif *)esp_netif_get_netif_impl(wifi) : NULL;
  m_lte = lte ? (struct netif *)esp_netif_get_netif_impl(lte) : NULL;
}

// ---------- totals (meter task, persisted) ----------

#define HIST_DAYS 31
#define HIST_MONTHS 6
#define MAGIC 0x4D455432u  // "MET2"

typedef struct {
  uint32_t magic;
  int32_t day_key;    // YYYYMMDD of the current day, 0 before the clock is set
  int32_t month_key;  // YYYYMM of the billing period start
  uint64_t day_l, day_w, month_l, month_w, total_l, total_w;
  int32_t hist_day[HIST_DAYS];  // newest first
  uint64_t hist_l[HIST_DAYS], hist_w[HIST_DAYS];
  int32_t hist_month[HIST_MONTHS];
  uint64_t hist_month_l[HIST_MONTHS];
  int64_t allow_until;  // unix time; 4G override for the hotspot until then
  int32_t warned_month, capped_month, warned_day, capped_day;
} state_t;

static state_t s;
static SemaphoreHandle_t mtx;
static volatile bool blocked;
static bool dirty;
static int64_t last_save;

static void save(void) {
  nvs_handle_t h;
  if (nvs_open("meter", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_blob(h, "st", &s, sizeof(s));
    nvs_commit(h);
    nvs_close(h);
  }
  dirty = false;
  last_save = time(NULL);
}

static void load(void) {
  nvs_handle_t h;
  size_t len = sizeof(s);
  bool ok = false;
  if (nvs_open("meter", NVS_READONLY, &h) == ESP_OK) {
    ok = nvs_get_blob(h, "st", &s, &len) == ESP_OK && len == sizeof(s) && s.magic == MAGIC;
    nvs_close(h);
  }
  if (!ok) {
    memset(&s, 0, sizeof(s));
    s.magic = MAGIC;
  }
}

static double cfg_double(const char *key) {
  char b[24];
  cfg_get(key, b, sizeof(b));
  return atof(b);
}

static bool clock_ok(void) { return time(NULL) > 1600000000; }

static int32_t day_key_of(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

static int32_t month_key_of(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  int y = tm.tm_year + 1900, m = tm.tm_mon + 1;
  if (tm.tm_mday < cfg_int("dm_bill_day")) {
    if (--m == 0) {
      m = 12;
      y--;
    }
  }
  return y * 100 + m;
}

// Bytes adjusted for carrier overhead (headers the carrier also bills).
static uint64_t billed(uint64_t b) { return b + b * (uint64_t)cfg_int("dm_overhead") / 100; }

// Effective monthly cap in bytes (0 = none): the smaller of the MB limit and
// what the budget buys at the configured price.
static uint64_t month_cap(void) {
  uint64_t cap = (uint64_t)cfg_int("dm_cap_mb") * 1024 * 1024;
  double price = cfg_double("dm_price"), budget = cfg_double("dm_budget");
  if (price > 0 && budget > 0) {
    uint64_t by_budget = (uint64_t)(budget / price * 1024.0 * 1024 * 1024);
    if (!cap || by_budget < cap) cap = by_budget;
  }
  return cap;
}

static uint64_t day_cap(void) { return (uint64_t)cfg_int("dm_day_mb") * 1024 * 1024; }

static double cost_of(uint64_t bytes) { return billed(bytes) / (1024.0 * 1024 * 1024) * cfg_double("dm_price"); }

static void roll_periods(void) {
  if (!clock_ok()) return;
  time_t now = time(NULL);
  int32_t dk = day_key_of(now), mk = month_key_of(now);
  if (s.day_key != dk) {
    if (s.day_key) {
      memmove(&s.hist_day[1], &s.hist_day[0], sizeof(s.hist_day[0]) * (HIST_DAYS - 1));
      memmove(&s.hist_l[1], &s.hist_l[0], sizeof(s.hist_l[0]) * (HIST_DAYS - 1));
      memmove(&s.hist_w[1], &s.hist_w[0], sizeof(s.hist_w[0]) * (HIST_DAYS - 1));
      s.hist_day[0] = s.day_key;
      s.hist_l[0] = s.day_l;
      s.hist_w[0] = s.day_w;
      devlog("meter: day %ld closed, 4G %llu bytes, Wi-Fi %llu bytes", (long)s.day_key,
             (unsigned long long)s.day_l, (unsigned long long)s.day_w);
    }
    s.day_key = dk;
    s.day_l = s.day_w = 0;
    dirty = true;
  }
  if (s.month_key != mk) {
    if (s.month_key) {
      memmove(&s.hist_month[1], &s.hist_month[0], sizeof(s.hist_month[0]) * (HIST_MONTHS - 1));
      memmove(&s.hist_month_l[1], &s.hist_month_l[0], sizeof(s.hist_month_l[0]) * (HIST_MONTHS - 1));
      s.hist_month[0] = s.month_key;
      s.hist_month_l[0] = s.month_l;
      event_post("New 4G billing period. Last period used %.1f MB", billed(s.month_l) / 1048576.0);
    }
    s.month_key = mk;
    s.month_l = s.month_w = 0;
    dirty = true;
  }
}

static void check_limits(void) {
  uint64_t mc = month_cap(), dc = day_cap();
  uint64_t mu = billed(s.month_l), du = billed(s.day_l);
  int warn = cfg_int("dm_warn_pct");
  char cur[8];
  cfg_get("dm_curr", cur, sizeof(cur));
  bool month_over = mc && mu >= mc, day_over = dc && du >= dc;
  int32_t mk = s.month_key ? s.month_key : 1, dk = s.day_key ? s.day_key : 1;
  if (mc && mu * 100 >= mc * (uint64_t)warn && s.warned_month != mk) {
    s.warned_month = mk;
    dirty = true;
    event_post("4G data: %d%% of the monthly limit used (%.0f of %.0f MB, about %s %.2f)", (int)(mu * 100 / mc),
               mu / 1048576.0, mc / 1048576.0, cur, cost_of(s.month_l));
  }
  if (month_over && s.capped_month != mk) {
    s.capped_month = mk;
    dirty = true;
    event_post("4G monthly limit reached (%.0f MB).%s", mc / 1048576.0,
               cfg_is("dm_action", "stop") ? " The hotspot no longer uses 4G; allow more on the Data page." : "");
  }
  if (dc && du * 100 >= dc * (uint64_t)warn && s.warned_day != dk) {
    s.warned_day = dk;
    dirty = true;
    event_post("4G data: %d%% of today's limit used (%.0f of %.0f MB)", (int)(du * 100 / dc), du / 1048576.0,
               dc / 1048576.0);
  }
  if (day_over && s.capped_day != dk) {
    s.capped_day = dk;
    dirty = true;
    event_post("4G daily limit reached (%.0f MB).%s", dc / 1048576.0,
               cfg_is("dm_action", "stop") ? " The hotspot no longer uses 4G until tomorrow." : "");
  }
  bool override = s.allow_until && time(NULL) < s.allow_until;
  bool b = cfg_is("dm_action", "stop") && (month_over || day_over) && !override;
  if (b != blocked) {
    blocked = b;
    devlog("meter: 4G for hotspot clients %s", b ? "BLOCKED (limit reached)" : "allowed");
  }
}

static void meter_task(void *arg) {
  uint32_t last_rx_w = rx_w, last_tx_w = tx_w, last_rx_l = rx_l, last_tx_l = tx_l;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    tcpip_callback(wrap_outputs, NULL);
    uint32_t a = rx_w, b = tx_w, c = rx_l, d = tx_l;
    uint64_t dw = (uint32_t)(a - last_rx_w) + (uint64_t)(uint32_t)(b - last_tx_w);
    uint64_t dl = (uint32_t)(c - last_rx_l) + (uint64_t)(uint32_t)(d - last_tx_l);
    last_rx_w = a, last_tx_w = b, last_rx_l = c, last_tx_l = d;
    xSemaphoreTake(mtx, portMAX_DELAY);
    roll_periods();
    if (dw || dl) {
      s.day_w += dw, s.month_w += dw, s.total_w += dw;
      s.day_l += dl, s.month_l += dl, s.total_l += dl;
      dirty = true;
    }
    check_limits();
    // Persist every 5 minutes while counting, so a power cut loses little.
    if (dirty && (time(NULL) - last_save > 300 || time(NULL) < last_save)) save();
    xSemaphoreGive(mtx);
  }
}

void meter_start(void) {
  mtx = xSemaphoreCreateMutex();
  load();
  last_save = time(NULL);
  devlog("meter: loaded, 4G this period %llu bytes, total %llu bytes", (unsigned long long)s.month_l,
         (unsigned long long)s.total_l);
  xTaskCreatePinnedToCore(meter_task, "meter", 4096, NULL, 3, NULL, 1);
}

bool meter_lte_blocked(void) { return blocked; }

void meter_allow_until_midnight(void) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  time_t now = time(NULL);
  if (clock_ok()) {
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = 23, tm.tm_min = 59, tm.tm_sec = 59;
    s.allow_until = mktime(&tm);
  } else {
    s.allow_until = now + 6 * 3600;
  }
  check_limits();
  save();
  xSemaphoreGive(mtx);
  event_post("4G limit overridden: the hotspot may use 4G until midnight");
}

void meter_reset(const char *scope) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (!strcmp(scope, "all")) {
    memset(&s, 0, sizeof(s));
    s.magic = MAGIC;
    roll_periods();
  } else {
    s.day_l = s.day_w = s.month_l = s.month_w = 0;
    s.warned_month = s.capped_month = s.warned_day = s.capped_day = 0;
  }
  check_limits();
  save();
  xSemaphoreGive(mtx);
  event_post("Data counters reset (%s)", scope);
}

static void add_mb(cJSON *o, const char *k, uint64_t bytes) { cJSON_AddNumberToObject(o, k, billed(bytes) / 1048576.0); }
// Wi-Fi is not billed by the carrier, so no overhead is added.
static void add_raw(cJSON *o, const char *k, uint64_t bytes) { cJSON_AddNumberToObject(o, k, bytes / 1048576.0); }

void meter_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "data");
  xSemaphoreTake(mtx, portMAX_DELAY);
  add_mb(o, "lte_today_mb", s.day_l);
  add_mb(o, "lte_period_mb", s.month_l);
  add_mb(o, "lte_total_mb", s.total_l);
  add_raw(o, "wifi_today_mb", s.day_w);
  add_raw(o, "wifi_period_mb", s.month_w);
  add_raw(o, "wifi_total_mb", s.total_w);
  cJSON_AddNumberToObject(o, "period_cap_mb", month_cap() / 1048576.0);
  cJSON_AddNumberToObject(o, "day_cap_mb", day_cap() / 1048576.0);
  cJSON_AddNumberToObject(o, "warn_pct", cfg_int("dm_warn_pct"));
  cJSON_AddNumberToObject(o, "period_cost", cost_of(s.month_l));
  cJSON_AddNumberToObject(o, "today_cost", cost_of(s.day_l));
  cJSON_AddNumberToObject(o, "price_per_gb", cfg_double("dm_price"));
  cJSON_AddNumberToObject(o, "budget", cfg_double("dm_budget"));
  char cur[8], act[8];
  cfg_get("dm_curr", cur, sizeof(cur));
  cfg_get("dm_action", act, sizeof(act));
  cJSON_AddStringToObject(o, "currency", cur);
  cJSON_AddStringToObject(o, "action", act);
  cJSON_AddBoolToObject(o, "clock_ok", clock_ok());
  cJSON_AddNumberToObject(o, "period_key", s.month_key);
  cJSON_AddNumberToObject(o, "bill_day", cfg_int("dm_bill_day"));
  cJSON_AddBoolToObject(o, "blocked", blocked);
  cJSON_AddNumberToObject(o, "allow_until", (double)s.allow_until);
  cJSON *days = cJSON_AddArrayToObject(o, "days");
  cJSON *today = cJSON_CreateObject();
  cJSON_AddNumberToObject(today, "day", s.day_key);
  add_mb(today, "lte_mb", s.day_l);
  add_raw(today, "wifi_mb", s.day_w);
  cJSON_AddItemToArray(days, today);
  for (int i = 0; i < HIST_DAYS && s.hist_day[i]; i++) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "day", s.hist_day[i]);
    add_mb(d, "lte_mb", s.hist_l[i]);
    add_raw(d, "wifi_mb", s.hist_w[i]);
    cJSON_AddItemToArray(days, d);
  }
  cJSON *months = cJSON_AddArrayToObject(o, "periods");
  for (int i = 0; i < HIST_MONTHS && s.hist_month[i]; i++) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "period", s.hist_month[i]);
    add_mb(m, "lte_mb", s.hist_month_l[i]);
    cJSON_AddNumberToObject(m, "cost", cost_of(s.hist_month_l[i]));
    cJSON_AddItemToArray(months, m);
  }
  xSemaphoreGive(mtx);
}

void meter_summary(char *out, size_t cap) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(out, cap,
           "data: 4G today %.1f MB, period %.1f MB (cap %.0f MB, day cap %.0f MB), total %.1f MB; Wi-Fi period %.1f MB; "
           "blocked %d; period %ld\n",
           billed(s.day_l) / 1048576.0, billed(s.month_l) / 1048576.0, month_cap() / 1048576.0,
           day_cap() / 1048576.0, billed(s.total_l) / 1048576.0, s.month_w / 1048576.0, blocked,
           (long)s.month_key);
  xSemaphoreGive(mtx);
}
