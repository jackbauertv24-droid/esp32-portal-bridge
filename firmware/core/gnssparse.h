// Parsers for the A76XX modem's GNSS and cell replies (pure C, host-tested
// in tests/test_gnss.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  bool fix;
  int mode;           // 2 = 2D, 3 = 3D
  int sats;           // satellites used, all systems
  double lat, lon;    // decimal degrees, south/west negative
  double alt_m, speed_kmh, course;
  double hdop;
  char utc_date[7];   // ddmmyy
  char utc_time[11];  // hhmmss.s
} gnss_fix_t;

// Parses a +CGNSSINFO reply (the whole reply or just its line). Tolerates
// firmware that reports a different number of satellite-count fields.
// Returns true only when the reply carries a position.
bool gnss_parse_info(const char *reply, gnss_fix_t *f);

typedef struct {
  char sys[16];       // "LTE", "GSM", "WCDMA", "NO SERVICE"
  char mccmnc[8];
  char tac[10];
  long cell;          // serving cell id, -1 unknown
  int pci;            // physical cell id, -1 unknown
  int band;           // e.g. 3 for EUTRAN-BAND3, 0 unknown
  int earfcn;
  bool has_rsrp, has_rsrq, has_rssi, has_sinr;
  double rsrp, rsrq, rssi, sinr;  // dBm, dB, dBm, dB
} cell_info_t;

// Parses a +CPSI reply. Handles both the A76XX index form (RSRP 67 = -73 dBm)
// and the raw form some firmware prints (-73, or tenths like -730).
bool cpsi_parse(const char *reply, cell_info_t *c);

// Great-circle distance in metres.
double geo_distance_m(double lat1, double lon1, double lat2, double lon2);
