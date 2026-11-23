#include "battery.h"

#include <Wire.h>

#include "board.h"
#include "devlog.h"

static BatteryInfo info;

static bool gauge_present() {
  Wire.beginTransmission(MAX17048_ADDR);
  return Wire.endTransmission() == 0;
}

static bool read_reg(uint8_t reg, uint16_t &v) {
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MAX17048_ADDR, 2) != 2) return false;
  v = ((uint16_t)Wire.read() << 8) | Wire.read();
  return true;
}

void battery_begin() {
  const int pairs[2][2] = {{I2C_SDA_A, I2C_SCL_A}, {I2C_SDA_B, I2C_SCL_B}};
  for (auto &p : pairs) {
    Wire.begin(p[0], p[1], 100000);
    if (gauge_present()) {
      info.found = true;
      info.sda = p[0];
      info.scl = p[1];
      devlog("battery: MAX17048 found on SDA %d / SCL %d", p[0], p[1]);
      return;
    }
    Wire.end();
  }
  devlog("battery: MAX17048 not found on SDA/SCL %d/%d or %d/%d", I2C_SDA_A, I2C_SCL_A, I2C_SDA_B, I2C_SCL_B);
}

BatteryInfo battery_read() {
  if (!info.found) return info;
  uint16_t v;
  if (read_reg(0x02, v)) info.volts = v * 78.125e-6f;  // VCELL: 78.125 uV per bit
  if (read_reg(0x04, v)) info.percent = v / 256.0f;     // SOC: 1/256 % per bit
  return info;
}
