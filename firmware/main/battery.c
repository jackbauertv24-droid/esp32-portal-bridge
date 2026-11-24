#include "battery.h"

#include "board.h"
#include "devlog.h"
#include "driver/i2c_master.h"

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;
static int sda = -1, scl = -1;

void battery_init(void) {
  const int pairs[2][2] = {{I2C_SDA_A, I2C_SCL_A}, {I2C_SDA_B, I2C_SCL_B}};
  for (int i = 0; i < 2; i++) {
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = pairs[i][0],
        .scl_io_num = pairs[i][1],
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) continue;
    if (i2c_master_probe(bus, MAX17048_ADDR, 100) == ESP_OK) {
      i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = MAX17048_ADDR,
                                .scl_speed_hz = 100000};
      i2c_master_bus_add_device(bus, &dc, &dev);
      sda = pairs[i][0];
      scl = pairs[i][1];
      devlog("battery: MAX17048 found on SDA %d / SCL %d", sda, scl);
      return;
    }
    i2c_del_master_bus(bus);
    bus = NULL;
  }
  devlog("battery: MAX17048 not found on SDA/SCL %d/%d or %d/%d", I2C_SDA_A, I2C_SCL_A, I2C_SDA_B, I2C_SCL_B);
}

static bool read_reg(uint8_t reg, uint16_t *v) {
  uint8_t b[2];
  if (!dev || i2c_master_transmit_receive(dev, &reg, 1, b, 2, 100) != ESP_OK) return false;
  *v = (uint16_t)(b[0] << 8 | b[1]);
  return true;
}

void battery_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "battery");
  uint16_t v;
  cJSON_AddBoolToObject(o, "found", dev != NULL);
  cJSON_AddNumberToObject(o, "sda", sda);
  cJSON_AddNumberToObject(o, "scl", scl);
  if (read_reg(0x02, &v)) cJSON_AddNumberToObject(o, "volts", v * 78.125e-6);
  if (read_reg(0x04, &v)) cJSON_AddNumberToObject(o, "percent", v / 256.0);
}

void battery_suspend(void) {
  if (dev) i2c_master_bus_rm_device(dev);
  if (bus) i2c_del_master_bus(bus);
  dev = NULL;
  bus = NULL;
}

void battery_resume(void) {
  if (!bus) battery_init();
}
