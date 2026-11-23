#pragma once
#include <Arduino.h>

struct BatteryInfo {
  bool found = false;
  int sda = -1, scl = -1;  // pin pair the gauge answered on
  float volts = 0;
  float percent = 0;
};

// Probes both candidate I2C pin pairs once; later calls re-read the gauge.
void battery_begin();
BatteryInfo battery_read();
