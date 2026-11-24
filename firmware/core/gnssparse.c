#include "gnssparse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 24

// Splits the text after `tag` at commas (up to MAXF fields, each trimmed, max 31 chars).
static int split(const char *reply, const char *tag, char f[MAXF][32]) {
  const char *p = reply ? strstr(reply, tag) : NULL;
  if (!p) return 0;
  p += strlen(tag);
  int n = 0;
  while (n < MAXF) {
    while (*p == ' ') p++;
    size_t k = 0;
    while (*p && *p != ',' && *p != '\r' && *p != '\n') {
      if (k < 31) f[n][k++] = *p;
      p++;
    }
    while (k && f[n][k - 1] == ' ') k--;
    f[n][k] = 0;
    n++;
    if (*p != ',') break;
    p++;
  }
  return n;
}

// ddmm.mmmm (or dddmm.mmmm) to decimal degrees.
static double dm_to_deg(const char *s) {
  double v = atof(s);
  double deg = floor(v / 100);
  return deg + (v - deg * 100) / 60.0;
}

static bool num(const char *s) {
  if (!*s) return false;
  for (; *s; s++)
    if ((*s < '0' || *s > '9') && *s != '.' && *s != '-') return false;
  return true;
}

bool gnss_parse_info(const char *reply, gnss_fix_t *f) {
  memset(f, 0, sizeof(*f));
  char v[MAXF][32];
  int n = split(reply, "+CGNSSINFO:", v);
  // Find the latitude by its hemisphere letters: <lat>,<N|S>,<lon>,<E|W>.
  int i = -1;
  for (int k = 1; k + 3 < n; k++)
    if ((!strcmp(v[k + 1], "N") || !strcmp(v[k + 1], "S")) && (!strcmp(v[k + 3], "E") || !strcmp(v[k + 3], "W")) &&
        num(v[k]) && num(v[k + 2])) {
      i = k;
      break;
    }
  if (i < 0) return false;
  f->mode = atoi(v[0]);
  for (int k = 1; k < i; k++) f->sats += atoi(v[k]);
  f->lat = dm_to_deg(v[i]) * (v[i + 1][0] == 'S' ? -1 : 1);
  f->lon = dm_to_deg(v[i + 2]) * (v[i + 3][0] == 'W' ? -1 : 1);
  if (i + 4 < n) snprintf(f->utc_date, sizeof(f->utc_date), "%.6s", v[i + 4]);
  if (i + 5 < n) snprintf(f->utc_time, sizeof(f->utc_time), "%.10s", v[i + 5]);
  if (i + 6 < n) f->alt_m = atof(v[i + 6]);
  if (i + 7 < n) f->speed_kmh = atof(v[i + 7]) * 1.852;
  if (i + 8 < n) f->course = atof(v[i + 8]);
  if (i + 10 < n) f->hdop = atof(v[i + 10]);
  f->fix = !(f->lat == 0 && f->lon == 0);
  return f->fix;
}

// Index form (non-negative) or raw form (negative, possibly in tenths).
static double level(const char *s, double idx_offset, double idx_scale, double tenths_below) {
  double v = atof(s);
  if (v >= 0) return (v + idx_offset) * idx_scale;
  return v < tenths_below ? v / 10 : v;
}

bool cpsi_parse(const char *reply, cell_info_t *c) {
  memset(c, 0, sizeof(*c));
  c->cell = -1;
  c->pci = -1;
  char v[MAXF][32];
  int n = split(reply, "+CPSI:", v);
  if (n < 1) return false;
  snprintf(c->sys, sizeof(c->sys), "%.15s", v[0]);
  if (n > 2) snprintf(c->mccmnc, sizeof(c->mccmnc), "%.7s", v[2]);
  if (n > 3) snprintf(c->tac, sizeof(c->tac), "%.9s", v[3]);
  if (n > 4 && num(v[4])) c->cell = atol(v[4]);
  if (strcmp(c->sys, "LTE") || n < 14) return true;
  if (num(v[5])) c->pci = atoi(v[5]);
  const char *b = strstr(v[6], "BAND");
  if (b) c->band = atoi(b + 4);
  c->earfcn = atoi(v[7]);
  // A76XX manual: RSRQ = (value-40)/2 dB, RSRP = value-140 dBm, RSSI = value-110 dBm, 255 = unknown.
  if (num(v[10]) && atof(v[10]) != 255) c->has_rsrq = true, c->rsrq = level(v[10], -40, 0.5, -40);
  if (num(v[11]) && atof(v[11]) != 255) c->has_rsrp = true, c->rsrp = level(v[11], -140, 1, -200);
  if (num(v[12]) && atof(v[12]) != 255) c->has_rssi = true, c->rssi = level(v[12], -110, 1, -200);
  if (num(v[13]) && atof(v[13]) != 255) {
    c->has_sinr = true;
    c->sinr = atof(v[13]);
    if (c->sinr > 60 || c->sinr < -60) c->sinr /= 10;
  }
  return true;
}

double geo_distance_m(double lat1, double lon1, double lat2, double lon2) {
  const double r = 6371000, d = 3.14159265358979323846 / 180;
  double a = sin((lat2 - lat1) * d / 2), b = sin((lon2 - lon1) * d / 2);
  double h = a * a + cos(lat1 * d) * cos(lat2 * d) * b * b;
  return 2 * r * asin(sqrt(h));
}
