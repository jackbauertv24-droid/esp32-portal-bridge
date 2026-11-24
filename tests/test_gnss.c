// Host tests for core/gnssparse.c, using the reply examples from SIMCom's
// A76XX GNSS application note and AT command manual.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gnssparse.h"

static int failures = 0;
#define CHECK(label, cond)        \
  do {                            \
    if (!(cond)) {                \
      printf("FAIL %s\n", label); \
      failures++;                 \
    }                             \
  } while (0)
#define NEAR(a, b, eps) (fabs((a) - (b)) < (eps))

int main(void) {
  gnss_fix_t f;
  CHECK("manual example has a fix",
        gnss_parse_info("AT+CGNSSINFO\r\n+CGNSSINFO: 2,09,05,00,3113.330650,N,12121.262554,E,131117,091918.0,32.9,0.0,255.0,1.1,0.8,0.7\r\n\r\nOK", &f));
  CHECK("lat", NEAR(f.lat, 31 + 13.330650 / 60, 1e-7));
  CHECK("lon", NEAR(f.lon, 121 + 21.262554 / 60, 1e-7));
  CHECK("sats summed", f.sats == 14);
  CHECK("mode", f.mode == 2);
  CHECK("alt", NEAR(f.alt_m, 32.9, 1e-9));
  CHECK("hdop", NEAR(f.hdop, 0.8, 1e-9));
  CHECK("date/time", !strcmp(f.utc_date, "131117") && !strcmp(f.utc_time, "091918.0"));

  CHECK("extra satellite field tolerated",
        gnss_parse_info("+CGNSSINFO: 3,12,04,02,06,2217.123400,N,11410.567800,E,241126,031500.0,55.0,10.0,90.0,1.0,0.7,0.6", &f) &&
            f.sats == 24 && NEAR(f.lat, 22 + 17.1234 / 60, 1e-7) && NEAR(f.speed_kmh, 18.52, 1e-6) &&
            NEAR(f.hdop, 0.7, 1e-9));
  CHECK("south/west negative",
        gnss_parse_info("+CGNSSINFO: 3,08,00,00,3352.000000,S,15112.000000,W,010126,000000.0,5,0,0,1,1,1", &f) &&
            f.lat < -33.8 && f.lon < -151.1);
  CHECK("no fix (empty fields)", !gnss_parse_info("+CGNSSINFO: ,,,,,,,,,,,,,,,\r\nOK", &f));
  CHECK("no fix (missing line)", !gnss_parse_info("ERROR", &f));

  cell_info_t c;
  CHECK("cpsi manual example",
        cpsi_parse("+CPSI: LTE,Online,460-01,0x230A,175499523,318,EUTRAN-BAND3,1650,5,0,21,67,255,19\r\nOK", &c));
  CHECK("cpsi fields", !strcmp(c.sys, "LTE") && !strcmp(c.mccmnc, "460-01") && !strcmp(c.tac, "0x230A") &&
                           c.cell == 175499523 && c.pci == 318 && c.band == 3 && c.earfcn == 1650);
  CHECK("rsrp index form", c.has_rsrp && NEAR(c.rsrp, -73, 1e-9));
  CHECK("rsrq index form", c.has_rsrq && NEAR(c.rsrq, -9.5, 1e-9));
  CHECK("rssi unknown", !c.has_rssi);
  CHECK("sinr", c.has_sinr && NEAR(c.sinr, 19, 1e-9));
  cpsi_parse("+CPSI: LTE,Online,454-00,0x1A2B,12345678,101,EUTRAN-BAND7,3100,5,5,-94,-850,-545,15", &c);
  CHECK("raw tenths form", NEAR(c.rsrp, -85, 1e-9) && NEAR(c.rsrq, -9.4, 1e-9) && NEAR(c.rssi, -54.5, 1e-9) && c.band == 7);
  cpsi_parse("+CPSI: LTE,Online,454-00,0x1A2B,12345678,101,EUTRAN-BAND7,3100,5,5,-10,-95,-65,8", &c);
  CHECK("raw dB form", NEAR(c.rsrp, -95, 1e-9) && NEAR(c.rsrq, -10, 1e-9) && NEAR(c.rssi, -65, 1e-9));
  CHECK("no service", cpsi_parse("+CPSI: NO SERVICE,Low Power Mode", &c) && !strcmp(c.sys, "NO SERVICE") && !c.has_rsrp);
  CHECK("gsm cell", cpsi_parse("+CPSI: GSM,Online,454-00,0x1234,5678,40,-80,0,0", &c) && c.cell == 5678 && !c.has_rsrp);

  CHECK("distance 1 degree lat", NEAR(geo_distance_m(22, 114, 23, 114), 111195, 50));
  CHECK("distance zero", geo_distance_m(22.3, 114.1, 22.3, 114.1) < 0.001);

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("gnss: all passed\n");
  return 0;
}
