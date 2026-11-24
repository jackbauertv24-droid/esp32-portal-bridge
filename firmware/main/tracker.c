#include "tracker.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "devlog.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gnssparse.h"
#include "modem_at.h"
#include "netmgr.h"
#include "settings.h"
#include "textutil.h"

#define HEADER "t,lat,lon,alt_m,speed_kmh,course,sats,hdop,uplink,csq,sys,rsrp,rsrq,sinr,band,mccmnc,tac,cell\n"

static SemaphoreHandle_t mtx;
static volatile bool log_now;
static bool gnss_on;  // powered by us
static gnss_fix_t last_fix;
static int64_t last_fix_us;  // 0 = never
static time_t last_fix_t;
static cell_info_t last_cell;
static int last_csq = 99;
static int64_t last_cell_us;
static double logged_lat, logged_lon;
static int64_t logged_us;
static unsigned points_today, points_total;
static char last_day[9];

// Pending "where are you" request.
static char req_from[24];
static volatile bool req_pending;
static int64_t last_reply_us;

static bool clock_ok(void) { return time(NULL) > 1600000000; }

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
static long days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Sets the clock from GNSS time when there has been no internet to do it.
static void clock_from_gnss(const gnss_fix_t *f) {
  if (clock_ok() || strlen(f->utc_date) != 6 || strlen(f->utc_time) < 6) return;
  int dd = (f->utc_date[0] - '0') * 10 + f->utc_date[1] - '0', mo = (f->utc_date[2] - '0') * 10 + f->utc_date[3] - '0',
      yy = (f->utc_date[4] - '0') * 10 + f->utc_date[5] - '0';
  int hh = (f->utc_time[0] - '0') * 10 + f->utc_time[1] - '0', mi = (f->utc_time[2] - '0') * 10 + f->utc_time[3] - '0',
      ss = (f->utc_time[4] - '0') * 10 + f->utc_time[5] - '0';
  if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 60) return;
  struct timeval tv = {.tv_sec = (time_t)days_from_civil(2000 + yy, mo, dd) * 86400 + hh * 3600 + mi * 60 + ss};
  settimeofday(&tv, NULL);
  devlog("tracker: clock set from GNSS time");
}

static void day_key(char *out) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  if (clock_ok()) strftime(out, 9, "%Y%m%d", &tm);
  else snprintf(out, 9, "00000000");  // before the clock is set
}

bool tracker_day_path(const char *day, char *out, size_t cap) {
  if (!day || strlen(day) != 8) return false;
  for (int i = 0; i < 8; i++)
    if (!isdigit((unsigned char)day[i])) return false;
  snprintf(out, cap, "/data/trk_%s.csv", day);
  return true;
}

// Oldest track file name, for clean-up.
static bool oldest_day(char *out) {
  DIR *d = opendir("/data");
  if (!d) return false;
  struct dirent *e;
  out[0] = 0;
  while ((e = readdir(d))) {
    const char *n = e->d_name;
    if (strncmp(n, "trk_", 4) || strlen(n) != 16) continue;
    if (!out[0] || strncmp(n + 4, out, 8) < 0) snprintf(out, 9, "%.8s", n + 4);
  }
  closedir(d);
  return out[0] != 0;
}

static void cleanup(void) {
  // Age limit.
  int keep = cfg_int("tr_keep_d");
  if (clock_ok()) {
    time_t lim = time(NULL) - (time_t)keep * 86400;
    struct tm tm;
    localtime_r(&lim, &tm);
    char lk[9], od[9], path[40];
    strftime(lk, sizeof(lk), "%Y%m%d", &tm);
    while (oldest_day(od) && strcmp(od, lk) < 0 && strcmp(od, "00000000")) {
      tracker_day_path(od, path, sizeof(path));
      unlink(path);
      devlog("tracker: removed old day %s", od);
    }
  }
  // Space limit: keep a quarter of the storage free for SMS and recordings.
  size_t total = 0, used = 0;
  char od[9], path[40], today[9];
  day_key(today);
  for (int i = 0; i < 30 && esp_spiffs_info("storage", &total, &used) == ESP_OK && total && used > total * 3 / 4; i++) {
    if (!oldest_day(od) || !strcmp(od, today)) break;
    tracker_day_path(od, path, sizeof(path));
    unlink(path);
    devlog("tracker: storage low, removed day %s", od);
  }
}

static void read_cell(void) {
  bool ok = false;
  char *r = modem_cmd("AT+CPSI?", 9000, &ok);
  cell_info_t c;
  if (r && ok && cpsi_parse(r, &c)) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    last_cell = c;
    last_cell_us = esp_timer_get_time();
    xSemaphoreGive(mtx);
  }
  free(r);
  int csq = 99;
  modem_radio(&csq, NULL, 0, NULL, 0);
  last_csq = csq;
}

// Returns true if a fix was read.
static bool read_fix(void) {
  bool ok = false;
  char *r = modem_cmd("AT+CGNSSINFO", 3000, &ok);
  gnss_fix_t f;
  bool fix = r && ok && gnss_parse_info(r, &f);
  free(r);
  if (fix) {
    clock_from_gnss(&f);
    xSemaphoreTake(mtx, portMAX_DELAY);
    last_fix = f;
    last_fix_us = esp_timer_get_time();
    last_fix_t = time(NULL);
    xSemaphoreGive(mtx);
  }
  return fix;
}

static void gnss_power(bool on) {
  if (on == gnss_on) return;
  bool ok = false;
  char *r = modem_cmd(on ? "AT+CGNSSPWR=1" : "AT+CGNSSPWR=0", 5000, &ok);
  free(r);
  gnss_on = on && ok;
  devlog("tracker: GNSS %s%s", on ? "on" : "off", on && !ok ? " FAILED" : "");
}

static void fmt_d(char *out, size_t cap, bool has, double v, int prec) {
  if (has) snprintf(out, cap, "%.*f", prec, v);
  else out[0] = 0;
}

static void log_point(bool fix) {
  int64_t now_us = esp_timer_get_time();
  int min_m = cfg_int("tr_min_m");
  if (fix && min_m > 0 && logged_us && now_us - logged_us < 600LL * 1000000 &&
      geo_distance_m(logged_lat, logged_lon, last_fix.lat, last_fix.lon) < min_m)
    return;  // has not moved enough; still record at least every 10 minutes
  char day[9], path[40];
  day_key(day);
  tracker_day_path(day, path, sizeof(path));
  if (strcmp(day, last_day)) {
    snprintf(last_day, sizeof(last_day), "%s", day);
    points_today = 0;
    cleanup();
  }
  struct stat sb;
  bool fresh = stat(path, &sb) != 0;
  FILE *f = fopen(path, "a");
  if (!f) return;
  if (fresh) fputs(HEADER, f);
  xSemaphoreTake(mtx, portMAX_DELAY);
  cell_info_t c = last_cell;
  bool cell_fresh = last_cell_us && now_us - last_cell_us < 120LL * 1000000;
  gnss_fix_t g = last_fix;
  xSemaphoreGive(mtx);
  char rsrp[12], rsrq[12], sinr[12];
  fmt_d(rsrp, sizeof(rsrp), cell_fresh && c.has_rsrp, c.rsrp, 0);
  fmt_d(rsrq, sizeof(rsrq), cell_fresh && c.has_rsrq, c.rsrq, 1);
  fmt_d(sinr, sizeof(sinr), cell_fresh && c.has_sinr, c.sinr, 1);
  char up[16];
  netmgr_uplink(up, sizeof(up));
  if (fix)
    fprintf(f, "%lld,%.6f,%.6f,%.1f,%.1f,%.0f,%d,%.1f,", (long long)time(NULL), g.lat, g.lon, g.alt_m, g.speed_kmh,
            g.course, g.sats, g.hdop);
  else
    fprintf(f, "%lld,,,,,,,,", (long long)time(NULL));
  fprintf(f, "%s,%d,%s,%s,%s,%s,", up, last_csq, cell_fresh ? c.sys : "", rsrp, rsrq, sinr);
  if (cell_fresh && c.band) fprintf(f, "%d,%s,%s,%ld\n", c.band, c.mccmnc, c.tac, c.cell);
  else fprintf(f, ",,,\n");
  fclose(f);
  points_today++;
  points_total++;
  if (fix) {
    logged_lat = g.lat, logged_lon = g.lon;
    logged_us = now_us;
  }
}

static void reply_location(const char *to) {
  // Without the tracker running, power GNSS up and give it a minute.
  bool fix = read_fix();
  bool powered_here = false;
  if (!fix && !gnss_on) {
    gnss_power(true);
    powered_here = true;
  }
  for (int i = 0; i < 12 && !fix && gnss_on; i++) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    fix = read_fix();
  }
  read_cell();
  char msg[320], when[24] = "";
  xSemaphoreTake(mtx, portMAX_DELAY);
  gnss_fix_t g = last_fix;
  int64_t age_s = last_fix_us ? (esp_timer_get_time() - last_fix_us) / 1000000 : -1;
  time_t ft = last_fix_t;
  cell_info_t c = last_cell;
  xSemaphoreGive(mtx);
  if (ft > 1600000000) {
    struct tm tm;
    localtime_r(&ft, &tm);
    strftime(when, sizeof(when), "%m-%d %H:%M", &tm);
  }
  int n;
  if (age_s >= 0) {
    n = snprintf(msg, sizeof(msg), "%s %s: https://maps.google.com/?q=%.6f,%.6f (%d sats, %.0f km/h)",
                 age_s < 120 ? "Location" : "Last known location", when, g.lat, g.lon, g.sats, g.speed_kmh);
  } else {
    n = snprintf(msg, sizeof(msg), "No GPS fix (antenna needs open sky).");
  }
  if (n > 0 && (size_t)n < sizeof(msg) && c.sys[0])
    snprintf(msg + n, sizeof(msg) - n, " Cell %s %s %s/%ld%s%.0f dBm.", c.sys, c.mccmnc, c.tac, c.cell,
             c.has_rsrp ? ", RSRP " : "", c.has_rsrp ? c.rsrp : 0.0);
  if (powered_here && !cfg_bool("tr_en")) gnss_power(false);
  char err[120];
  int parts = modem_send_sms(to, msg, err, sizeof(err));
  event_post("Location requested by SMS from an allowed number: %s", parts > 0 ? "replied" : err);
}

void tracker_on_sms(const char *from, const char *text) {
  char kw[24], allowed[128];
  cfg_get("tr_sms_kw", kw, sizeof(kw));
  cfg_get("tr_sms_from", allowed, sizeof(allowed));
  if (!kw[0] || !allowed[0] || !from) return;
  // The whole message must be the keyword (any case, surrounding spaces ignored).
  while (*text == ' ' || *text == '\n') text++;
  size_t n = strlen(text);
  while (n && (text[n - 1] == ' ' || text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == '?')) n--;
  if (n != strlen(kw) || strncasecmp(text, kw, n)) return;
  if (!phone_in_list(from, allowed)) {
    devlog("tracker: location keyword from a number not on the list; ignored");
    return;
  }
  snprintf(req_from, sizeof(req_from), "%s", from);
  req_pending = true;
}

static void task(void *arg) {
  int64_t next = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (req_pending) {
      req_pending = false;
      if (esp_timer_get_time() - last_reply_us > 30LL * 1000000 || !last_reply_us) {
        last_reply_us = esp_timer_get_time();
        char to[24];
        snprintf(to, sizeof(to), "%s", req_from);
        reply_location(to);
      }
    }
    bool en = cfg_bool("tr_en");
    if (!en) {
      if (gnss_on) gnss_power(false);
      if (!log_now) continue;
    }
    int64_t now = esp_timer_get_time();
    if (!log_now && now < next) continue;
    log_now = false;
    next = now + (int64_t)cfg_int("tr_int_s") * 1000000;
    if (en && !gnss_on) gnss_power(true);
    bool fix = gnss_on && read_fix();
    read_cell();
    log_point(fix);
  }
}

void tracker_start(void) {
  mtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(task, "tracker", 6144, NULL, 2, NULL, 1);
}

void tracker_log_now(void) { log_now = true; }

bool tracker_delete_day(const char *day) {
  char path[40];
  return tracker_day_path(day, path, sizeof(path)) && unlink(path) == 0;
}

cJSON *tracker_json(void) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "enabled", cfg_bool("tr_en"));
  cJSON_AddBoolToObject(o, "gnss_on", gnss_on);
  cJSON_AddNumberToObject(o, "interval_s", cfg_int("tr_int_s"));
  cJSON_AddNumberToObject(o, "points_today", points_today);
  char kw[24], allowed[128];
  cfg_get("tr_sms_kw", kw, sizeof(kw));
  cfg_get("tr_sms_from", allowed, sizeof(allowed));
  cJSON_AddBoolToObject(o, "sms_on", kw[0] && allowed[0]);
  cJSON_AddStringToObject(o, "sms_keyword", kw);
  int64_t now = esp_timer_get_time();
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (last_fix_us) {
    cJSON *f = cJSON_AddObjectToObject(o, "fix");
    cJSON_AddNumberToObject(f, "lat", last_fix.lat);
    cJSON_AddNumberToObject(f, "lon", last_fix.lon);
    cJSON_AddNumberToObject(f, "alt_m", last_fix.alt_m);
    cJSON_AddNumberToObject(f, "speed_kmh", last_fix.speed_kmh);
    cJSON_AddNumberToObject(f, "course", last_fix.course);
    cJSON_AddNumberToObject(f, "sats", last_fix.sats);
    cJSON_AddNumberToObject(f, "hdop", last_fix.hdop);
    cJSON_AddNumberToObject(f, "mode", last_fix.mode);
    cJSON_AddNumberToObject(f, "age_s", (double)((now - last_fix_us) / 1000000));
  }
  if (last_cell_us) {
    cJSON *c = cJSON_AddObjectToObject(o, "cell");
    cJSON_AddStringToObject(c, "sys", last_cell.sys);
    cJSON_AddStringToObject(c, "mccmnc", last_cell.mccmnc);
    cJSON_AddStringToObject(c, "tac", last_cell.tac);
    cJSON_AddNumberToObject(c, "cell", (double)last_cell.cell);
    cJSON_AddNumberToObject(c, "pci", last_cell.pci);
    cJSON_AddNumberToObject(c, "band", last_cell.band);
    cJSON_AddNumberToObject(c, "earfcn", last_cell.earfcn);
    if (last_cell.has_rsrp) cJSON_AddNumberToObject(c, "rsrp", last_cell.rsrp);
    if (last_cell.has_rsrq) cJSON_AddNumberToObject(c, "rsrq", last_cell.rsrq);
    if (last_cell.has_rssi) cJSON_AddNumberToObject(c, "rssi", last_cell.rssi);
    if (last_cell.has_sinr) cJSON_AddNumberToObject(c, "sinr", last_cell.sinr);
    cJSON_AddNumberToObject(c, "csq", last_csq);
    cJSON_AddNumberToObject(c, "age_s", (double)((now - last_cell_us) / 1000000));
  }
  xSemaphoreGive(mtx);
  cJSON *days = cJSON_AddArrayToObject(o, "days");
  DIR *d = opendir("/data");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (strncmp(e->d_name, "trk_", 4) || strlen(e->d_name) != 16) continue;
      char path[300];
      snprintf(path, sizeof(path), "/data/%s", e->d_name);
      struct stat sb;
      cJSON *j = cJSON_CreateObject();
      char day[9];
      snprintf(day, sizeof(day), "%.8s", e->d_name + 4);
      cJSON_AddStringToObject(j, "day", day);
      cJSON_AddNumberToObject(j, "bytes", stat(path, &sb) == 0 ? sb.st_size : 0);
      cJSON_AddItemToArray(days, j);
    }
    closedir(d);
  }
  return o;
}

// No positions or cell ids: reports may be public.
void tracker_summary(char *out, size_t cap) {
  char kw[24], allowed[128];
  cfg_get("tr_sms_kw", kw, sizeof(kw));
  cfg_get("tr_sms_from", allowed, sizeof(allowed));
  snprintf(out, cap, "tracker: enabled %d, gnss on %d, fix %s, points today %u, since start %u, sms location %s\n",
           cfg_bool("tr_en"), gnss_on, last_fix_us ? "yes" : "never", points_today, points_total,
           kw[0] && allowed[0] ? "on" : "off");
}
