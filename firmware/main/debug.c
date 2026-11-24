// Hardware test endpoints for the Debug page: camera, TF card, memory,
// temperature, LED and BOOT button. Modem audio, calls and GNSS are driven from
// the page through the AT console API.
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "battery.h"
#include "board.h"
#include "cJSON.h"
#include "devlog.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/temperature_sensor.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_rom_sys.h"
#include "img_converters.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "sdmmc_cmd.h"

static esp_err_t (*gate)(httpd_req_t *);

static esp_err_t send_json(httpd_req_t *req, cJSON *o) {
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
  free(s);
  return e;
}

static cJSON *result(bool ok, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", ok);
  cJSON_AddStringToObject(o, "message", msg);
  devlog("debug: %s %s", ok ? "PASS" : "FAIL", msg);
  return o;
}

static void query(httpd_req_t *req, const char *key, char *out, size_t cap) {
  char q[128];
  out[0] = 0;
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, key, out, cap);
}

// ---------- camera ----------

// Waveshare pin map for this board (CAMERA_MODEL_WAVESHARE_7670_BOARD in
// their ESP32-S3-A-SIM7670X-4G-V2 demo).
#define CAM_XCLK 39
#define CAM_SDA 15
#define CAM_SCL 16
#define CAM_VSYNC 42
#define CAM_HREF 41
#define CAM_PCLK 46

static void cam_config(camera_config_t *c, framesize_t fs, int xclk_hz, pixformat_t pf) {
  *c = (camera_config_t){
      .pin_pwdn = -1,
      .pin_reset = -1,
      .pin_xclk = CAM_XCLK,
      .pin_sccb_sda = CAM_SDA,
      .pin_sccb_scl = CAM_SCL,
      .pin_d7 = 14,
      .pin_d6 = 13,
      .pin_d5 = 12,
      .pin_d4 = 11,
      .pin_d3 = 10,
      .pin_d2 = 9,
      .pin_d1 = 8,
      .pin_d0 = 7,
      .pin_vsync = CAM_VSYNC,
      .pin_href = CAM_HREF,
      .pin_pclk = CAM_PCLK,
      .xclk_freq_hz = xclk_hz,
      .ledc_timer = LEDC_TIMER_0,
      .ledc_channel = LEDC_CHANNEL_0,
      .pixel_format = pf,
      .frame_size = pf == PIXFORMAT_JPEG ? fs : FRAMESIZE_VGA,
      .jpeg_quality = 12,
      .fb_count = 1,
      .fb_location = CAMERA_FB_IN_PSRAM,
      .grab_mode = CAMERA_GRAB_LATEST,
  };
}

// The camera driver's own error and warning lines logged after `marker`
// (ESP-IDF log lines are captured by the device log), joined with " | ".
static void driver_errors(const char *marker, char *out, size_t cap) {
  out[0] = 0;
  size_t n = 0;
  char *log = devlog_dump(&n);
  if (!log) return;
  char *p = NULL;
  for (char *q = strstr(log, marker); q; q = strstr(q + 1, marker)) p = q;
  size_t o = 0;
  for (char *line = p ? strchr(p, '\n') : NULL; line && *line && o + 8 < cap;) {
    line++;
    char *end = strchr(line, '\n');
    size_t len = end ? (size_t)(end - line) : strlen(line);
    char buf[200];
    size_t k = 0;
    for (size_t i = 0; i < len && k < sizeof(buf) - 1; i++) {
      if (line[i] == 0x1b) {  // skip colour codes
        while (i < len && line[i] != 'm') i++;
        continue;
      }
      buf[k++] = line[i];
    }
    buf[k] = 0;
    if (strstr(buf, "E (") || strstr(buf, "W (")) {
      const char *msg = strstr(buf, "E (") ? strstr(buf, "E (") : strstr(buf, "W (");
      o += (size_t)snprintf(out + o, cap - o, "%s%.110s", o ? " | " : "", msg);
    }
    line = end;
  }
  free(log);
}

// Starts the camera, trying the settings that most often matter: a slower
// clock, and raw RGB565 for sensors without a JPEG encoder. Each attempt is
// described in `log` (may be NULL).
static esp_err_t cam_start(framesize_t fs, char *how, size_t hcap, cJSON *log) {
  static const struct {
    int hz;
    pixformat_t pf;
    const char *name;
  } T[] = {{20000000, PIXFORMAT_JPEG, "JPEG, 20 MHz clock"},
           {10000000, PIXFORMAT_JPEG, "JPEG, 10 MHz clock"},
           {20000000, PIXFORMAT_RGB565, "RGB565, 20 MHz clock"},
           {10000000, PIXFORMAT_RGB565, "RGB565, 10 MHz clock"}};
  esp_err_t e = ESP_FAIL;
  // Frames go straight to PSRAM by DMA, so the driver needs no 16-30 KB
  // internal buffer (internal RAM is what the Wi-Fi radio needs).
  esp_camera_set_psram_mode(true);
  for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
    camera_config_t c;
    cam_config(&c, fs, T[i].hz, T[i].pf);
    char marker[64];
    snprintf(marker, sizeof(marker), "camera: trying %s", T[i].name);
    devlog("%s", marker);
    e = esp_camera_init(&c);
    if (log) {
      char why[400] = "", m[480];
      if (e != ESP_OK) driver_errors(marker, why, sizeof(why));
      snprintf(m, sizeof(m), "%s: %s%s%s", T[i].name, e == ESP_OK ? "started" : esp_err_to_name(e), why[0] ? " - " : "", why);
      cJSON_AddItemToArray(log, cJSON_CreateString(m));
    }
    devlog("camera: init %s -> %s", T[i].name, esp_err_to_name(e));
    if (e == ESP_OK) {
      snprintf(how, hcap, "%s", T[i].name);
      return ESP_OK;
    }
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return e;
}

// Takes one picture as JPEG (converting raw frames). Caller frees *jpg.
static bool cam_grab(uint8_t **jpg, size_t *len, int *w, int *h) {
  camera_fb_t *fb = NULL;
  for (int i = 0; i < 4; i++) {  // let exposure settle
    if (fb) esp_camera_fb_return(fb);
    fb = esp_camera_fb_get();
  }
  if (!fb) return false;
  bool ok;
  *w = fb->width, *h = fb->height;
  if (fb->format == PIXFORMAT_JPEG) {
    *jpg = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ok = *jpg != NULL;
    if (ok) memcpy(*jpg, fb->buf, fb->len), *len = fb->len;
  } else {
    ok = frame2jpg(fb, 80, jpg, len);
  }
  esp_camera_fb_return(fb);
  return ok;
}

static esp_err_t h_camera(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char sz[8];
  query(req, "size", sz, sizeof(sz));
  framesize_t fs = !strcmp(sz, "uxga") ? FRAMESIZE_UXGA : !strcmp(sz, "vga") ? FRAMESIZE_VGA : FRAMESIZE_SVGA;
  battery_suspend();  // the camera's control bus shares GPIO15/16 with the battery gauge
  char how[48] = "";
  esp_err_t e = cam_start(fs, how, sizeof(how), NULL);
  if (e != ESP_OK) {
    battery_resume();
    char m[200];
    snprintf(m, sizeof(m), "camera: could not start (%s). Press \"Diagnose camera\" to find out whether it is the hardware.",
             esp_err_to_name(e));
    return send_json(req, result(false, m));
  }
  sensor_t *s = esp_camera_sensor_get();
  const camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
  char name[96];
  snprintf(name, sizeof(name), "%s (PID 0x%04x), %s", info ? info->name : "unknown", s ? s->id.PID : 0, how);
  uint8_t *jpg = NULL;
  size_t len = 0;
  int w = 0, hgt = 0;
  bool ok = cam_grab(&jpg, &len, &w, &hgt);
  esp_camera_deinit();
  battery_resume();
  if (!ok) {
    char m[160];
    snprintf(m, sizeof(m), "camera: sensor %s started but no picture arrived. Press \"Diagnose camera\".", name);
    return send_json(req, result(false, m));
  }
  devlog("debug: PASS camera %s, %dx%d JPEG %u bytes", name, w, hgt, (unsigned)len);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "X-Camera", name);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t out = httpd_resp_send(req, (const char *)jpg, (ssize_t)len);
  free(jpg);
  return out;
}

// --- camera diagnosis ---

static const char *sensor_at(int addr) {
  switch (addr) {
    case 0x21: return "OV7670 / OV7725 / GC0308 / GC032A";
    case 0x30: return "OV2640 / SC031GS";
    case 0x3C: return "OV3660 / OV5640 / GC2145";
    case 0x24: return "HM0360 / HM1055";
    case 0x68: return "SC030IOT / SC101IOT";
    case 0x6E: return "BF3005 / BF20A6";
    case 0x36: return "MAX17048 battery gauge (not the camera)";
    default: return "unknown device";
  }
}

// Lists the I2C/SCCB addresses that answer on GPIO15/16. Returns how many
// look like a camera sensor; the list goes into `out`.
static int sccb_scan(char *out, size_t cap) {
  out[0] = 0;
  i2c_master_bus_handle_t bus = NULL;
  i2c_master_bus_config_t bc = {.i2c_port = -1, .sda_io_num = CAM_SDA, .scl_io_num = CAM_SCL,
                                .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                .flags.enable_internal_pullup = true};
  if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
    snprintf(out, cap, "could not open the bus");
    return 0;
  }
  int cams = 0;
  size_t o = 0;
  for (int a = 0x08; a < 0x78; a++) {
    if (i2c_master_probe(bus, (uint16_t)a, 20) != ESP_OK) continue;
    o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "%s0x%02X %s", o ? "; " : "", a, sensor_at(a));
    if (a != 0x36) cams++;
  }
  i2c_del_master_bus(bus);
  if (!out[0]) snprintf(out, cap, "nothing answers");
  return cams;
}

// Reads the sensor's chip id directly (clock must be running): OV5640/OV3660
// use 16-bit registers 0x300A/0x300B, GC2145 8-bit 0xF0/0xF1, OV2640 bank 1
// registers 0x0A/0x0B.
static void sensor_id(int addr, char *out, size_t cap) {
  snprintf(out, cap, "no id read");
  i2c_master_bus_handle_t bus = NULL;
  i2c_master_dev_handle_t dev = NULL;
  i2c_master_bus_config_t bc = {.i2c_port = -1, .sda_io_num = CAM_SDA, .scl_io_num = CAM_SCL,
                                .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                .flags.enable_internal_pullup = true};
  if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return;
  i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = (uint16_t)addr, .scl_speed_hz = 100000};
  if (i2c_master_bus_add_device(bus, &dc, &dev) == ESP_OK) {
    uint8_t hi = 0, lo = 0;
    if (addr == 0x3C) {
      uint8_t r1[2] = {0x30, 0x0A}, r2[2] = {0x30, 0x0B};
      bool ok16 = i2c_master_transmit_receive(dev, r1, 2, &hi, 1, 50) == ESP_OK &&
                  i2c_master_transmit_receive(dev, r2, 2, &lo, 1, 50) == ESP_OK;
      uint8_t g1 = 0xF0, g2 = 0xF1, ghi = 0, glo = 0;
      bool ok8 = i2c_master_transmit_receive(dev, &g1, 1, &ghi, 1, 50) == ESP_OK &&
                 i2c_master_transmit_receive(dev, &g2, 1, &glo, 1, 50) == ESP_OK;
      unsigned id16 = (unsigned)(hi << 8 | lo), id8 = (unsigned)(ghi << 8 | glo);
      snprintf(out, cap, "16-bit id register: %s0x%04X%s; 8-bit id register: %s0x%04X%s", ok16 ? "" : "(read failed) ", id16,
               id16 == 0x5640 ? " = OV5640" : id16 == 0x3660 ? " = OV3660" : "", ok8 ? "" : "(read failed) ", id8,
               id8 == 0x2145 ? " = GC2145" : "");
    } else if (addr == 0x30) {
      uint8_t sel[2] = {0xFF, 0x01}, a = 0x0A, b = 0x0B;
      bool ok = i2c_master_transmit(dev, sel, 2, 50) == ESP_OK && i2c_master_transmit_receive(dev, &a, 1, &hi, 1, 50) == ESP_OK &&
                i2c_master_transmit_receive(dev, &b, 1, &lo, 1, 50) == ESP_OK;
      snprintf(out, cap, "id register: %s0x%02X%02X%s", ok ? "" : "(read failed) ", hi, lo, hi == 0x26 ? " = OV2640" : "");
    }
    i2c_master_bus_rm_device(dev);
  }
  i2c_del_master_bus(bus);
}

static void xclk(bool on) {
  if (on) {
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_1_BIT,
                             .timer_num = LEDC_TIMER_1, .freq_hz = 20000000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_channel_config_t ch = {.gpio_num = CAM_XCLK, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
                                .timer_sel = LEDC_TIMER_1, .duty = 1, .hpoint = 0};
    ledc_timer_config(&t);
    ledc_channel_config(&ch);
  } else {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    gpio_reset_pin(CAM_XCLK);
  }
}

// Reads a line with the chip's own pull resistors off: high means something
// on the board pulls it up.
static int idle_level(int pin) {
  gpio_reset_pin(pin);
  gpio_set_direction(pin, GPIO_MODE_INPUT);
  gpio_set_pull_mode(pin, GPIO_FLOATING);
  esp_rom_delay_us(200);
  int hi = 0;
  for (int i = 0; i < 20; i++) hi += gpio_get_level(pin);
  gpio_reset_pin(pin);
  return hi >= 18 ? 1 : hi <= 2 ? 0 : -1;  // -1 = floating / noisy
}

// Counts level changes on a pin for `ms` (fast signals alias, but any change shows activity).
static int activity(int pin, int ms) {
  int changes = 0, last = gpio_get_level(pin);
  int64_t end = esp_timer_get_time() + ms * 1000;
  while (esp_timer_get_time() < end) {
    int v = gpio_get_level(pin);
    if (v != last) changes++, last = v;
  }
  return changes;
}

static void step(cJSON *arr, const char *name, int ok, const char *detail) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "step", name);
  cJSON_AddStringToObject(o, "result", ok > 0 ? "pass" : ok == 0 ? "fail" : "info");
  cJSON_AddStringToObject(o, "detail", detail);
  cJSON_AddItemToArray(arr, o);
  devlog("camera diag: %s: %s - %s", name, ok > 0 ? "pass" : ok == 0 ? "FAIL" : "info", detail);
}

static esp_err_t h_camdiag(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  cJSON *o = cJSON_CreateObject();
  cJSON *st = cJSON_AddArrayToObject(o, "steps");
  char d[240];
  const char *verdict = "", *kind = "";

  size_t ps = esp_psram_get_size();
  snprintf(d, sizeof(d), "%u MB (picture buffers live here); internal RAM free %u KB, largest block %u KB", (unsigned)(ps >> 20),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
  step(st, "Memory", ps > 0, d);

  battery_suspend();
  int sda = idle_level(CAM_SDA), scl = idle_level(CAM_SCL);
  bool pulled = sda == 1 && scl == 1;
  snprintf(d, sizeof(d), "SDA %s, SCL %s. The camera bus pull-ups are fed by the camera power rail, so both high usually means the camera is powered.",
           sda == 1 ? "high" : sda == 0 ? "low" : "floating", scl == 1 ? "high" : scl == 0 ? "low" : "floating");
  step(st, "Camera bus idle levels (GPIO15/16)", pulled ? 1 : 0, d);

  char found[200];
  int cams = sccb_scan(found, sizeof(found));
  step(st, "Who answers on the camera bus, clock off", -1, found);
  xclk(true);
  vTaskDelay(pdMS_TO_TICKS(20));
  int cams_clk = sccb_scan(found, sizeof(found));
  char idtxt[160] = "";
  if (strstr(found, "0x3C")) sensor_id(0x3C, idtxt, sizeof(idtxt));
  else if (strstr(found, "0x30")) sensor_id(0x30, idtxt, sizeof(idtxt));
  xclk(false);
  step(st, "Who answers with the 20 MHz camera clock on (GPIO39)", cams_clk > 0 ? 1 : 0, found);
  if (idtxt[0]) step(st, "Sensor chip id (read directly)", strstr(idtxt, " = ") ? 1 : 0, idtxt);
  cams = cams > cams_clk ? cams : cams_clk;

  cJSON *tries = cJSON_CreateArray();
  char how[48] = "";
  esp_err_t e = cam_start(FRAMESIZE_VGA, how, sizeof(how), tries);
  char *tj = cJSON_PrintUnformatted(tries);
  cJSON_Delete(tries);
  step(st, "Camera driver start", e == ESP_OK, tj ? tj : "");
  free(tj);
  bool frame = false;
  if (e == ESP_OK) {
    sensor_t *s = esp_camera_sensor_get();
    const camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
    snprintf(d, sizeof(d), "%s, PID 0x%04x, %s", info ? info->name : "unknown", s ? s->id.PID : 0, how);
    step(st, "Sensor", 1, d);
    int v = activity(CAM_VSYNC, 300), hr = activity(CAM_HREF, 100), pc = activity(CAM_PCLK, 20);
    snprintf(d, sizeof(d), "changes seen: VSYNC %d, HREF %d, PCLK %d", v, hr, pc);
    step(st, "Picture signals from the sensor", v > 0 && pc > 0, d);
    uint8_t *jpg = NULL;
    size_t len = 0;
    int w = 0, hgt = 0;
    frame = cam_grab(&jpg, &len, &w, &hgt);
    free(jpg);
    snprintf(d, sizeof(d), frame ? "%dx%d, %u bytes as JPEG" : "no frame within the timeout", w, hgt, (unsigned)len);
    step(st, "Picture", frame, d);
    if (!frame)
      verdict = v == 0 ? "The sensor answers and accepts its settings but sends no picture signals. Most likely the ribbon cable is not fully seated (control lines work, data lines do not) or the module is faulty."
                       : "Picture signals arrive but no frame is assembled. This points to firmware timing; send a report.",
      kind = v == 0 ? "hardware (likely)" : "firmware";
    esp_camera_deinit();
  }
  battery_resume();

  if (frame) verdict = "The camera works.", kind = "none";
  else if (e != ESP_OK && cams > 0)
    verdict = "A sensor answers on the bus but the driver could not start it. This is a firmware or compatibility problem, not wiring; send a report so the sensor type can be supported.",
    kind = "firmware";
  else if (e != ESP_OK && !pulled)
    verdict = "The camera bus has no pull-up voltage and nothing answers: the camera connector is not powered. Check the Camera DIP switch is ON (switches off then on again), and that the camera module is plugged in.",
    kind = "hardware";
  else if (e != ESP_OK)
    verdict = "The camera connector looks powered but no sensor answers. Most likely the ribbon cable is not fully inserted and locked, is inserted upside down (contacts must face the connector's contacts), or the camera module is faulty.",
    kind = "hardware";
  cJSON_AddStringToObject(o, "verdict", verdict);
  cJSON_AddStringToObject(o, "kind", kind);
  devlog("camera diag: verdict (%s) %s", kind, verdict);
  return send_json(req, o);
}

// ---------- TF card ----------

static esp_err_t h_sd(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = 5;
  slot.cmd = 4;
  slot.d0 = 6;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 2,
                                         .allocation_unit_size = 16 * 1024};
  sdmmc_card_t *card = NULL;
  esp_err_t e = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card);
  char m[220];
  if (e != ESP_OK) {
    snprintf(m, sizeof(m), "TF card: not mounted (%s). Insert a FAT32-formatted card.", esp_err_to_name(e));
    return send_json(req, result(false, m));
  }
  uint64_t bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
  const size_t N = 256 * 1024;
  uint8_t *buf = heap_caps_malloc(N, MALLOC_CAP_SPIRAM);
  bool ok = buf != NULL;
  double wr = 0, rd = 0;
  if (ok) {
    for (size_t i = 0; i < N; i++) buf[i] = (uint8_t)(i * 31 + 7);
    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen("/sdcard/pbtest.bin", "wb");
    ok = f && fwrite(buf, 1, N, f) == N;
    if (f) fclose(f);
    wr = N / ((esp_timer_get_time() - t0) / 1e6) / 1024;
    memset(buf, 0, N);
    t0 = esp_timer_get_time();
    f = fopen("/sdcard/pbtest.bin", "rb");
    ok = ok && f && fread(buf, 1, N, f) == N;
    if (f) fclose(f);
    rd = N / ((esp_timer_get_time() - t0) / 1e6) / 1024;
    for (size_t i = 0; ok && i < N; i++) ok = buf[i] == (uint8_t)(i * 31 + 7);
    unlink("/sdcard/pbtest.bin");
  }
  free(buf);
  snprintf(m, sizeof(m), "TF card: %s, %.1f GB; 256 KB write %.0f KB/s, read %.0f KB/s, data %s", card->cid.name,
           bytes / 1e9, wr, rd, ok ? "verified" : "MISMATCH");
  esp_vfs_fat_sdcard_unmount("/sdcard", card);
  return send_json(req, result(ok, m));
}

// ---------- memory, temperature ----------

static esp_err_t h_mem(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  const size_t N = 2 * 1024 * 1024;
  uint32_t *p = heap_caps_malloc(N, MALLOC_CAP_SPIRAM);
  bool ok = p != NULL;
  double mbps = 0;
  if (ok) {
    int64_t t0 = esp_timer_get_time();
    for (size_t i = 0; i < N / 4; i++) p[i] = (uint32_t)i * 2654435761u;
    for (size_t i = 0; ok && i < N / 4; i++) ok = p[i] == (uint32_t)i * 2654435761u;
    mbps = 2.0 * N / ((esp_timer_get_time() - t0) / 1e6) / 1e6;
    free(p);
  }
  uint32_t flash = 0;
  esp_flash_get_size(NULL, &flash);
  char m[220];
  snprintf(m, sizeof(m), "PSRAM %u KB, 2 MB pattern %s (%.0f MB/s); free internal %u KB, free PSRAM %u KB; flash %lu MB",
           (unsigned)(esp_psram_get_size() / 1024), ok ? "verified" : "FAILED", mbps,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned long)(flash / (1024 * 1024)));
  return send_json(req, result(ok, m));
}

static esp_err_t h_temp(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  temperature_sensor_handle_t t = NULL;
  temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  float c = 0;
  bool ok = temperature_sensor_install(&tc, &t) == ESP_OK && temperature_sensor_enable(t) == ESP_OK &&
            temperature_sensor_get_celsius(t, &c) == ESP_OK;
  if (t) {
    temperature_sensor_disable(t);
    temperature_sensor_uninstall(t);
  }
  char m[80];
  snprintf(m, sizeof(m), ok ? "ESP32-S3 chip temperature %.1f °C" : "temperature sensor not available", c);
  return send_json(req, result(ok, m));
}

// ---------- LED, button ----------

static esp_err_t h_led(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char c[8];
  query(req, "c", c, sizeof(c));
  int r = !strcmp(c, "r") || !strcmp(c, "w") ? 80 : 0, g = !strcmp(c, "g") || !strcmp(c, "w") ? 80 : 0,
      b = !strcmp(c, "b") || !strcmp(c, "w") ? 80 : 0;
  led_test(r, g, b, 2500);
  return send_json(req, result(true, "LED colour set for 2.5 s"));
}

static esp_err_t h_button(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
  gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "pressed", gpio_get_level(GPIO_NUM_0) == 0);
  return send_json(req, o);
}

void debug_register(httpd_handle_t h, esp_err_t (*auth_gate)(httpd_req_t *)) {
  gate = auth_gate;
  struct {
    const char *uri;
    esp_err_t (*fn)(httpd_req_t *);
  } r[] = {{"/api/debug/camera", h_camera}, {"/api/debug/sd", h_sd},     {"/api/debug/mem", h_mem},
           {"/api/debug/temp", h_temp},     {"/api/debug/led", h_led},   {"/api/debug/button", h_button}, {"/api/debug/camdiag", h_camdiag}};
  for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
    httpd_uri_t u = {.uri = r[i].uri, .method = HTTP_GET, .handler = r[i].fn};
    httpd_register_uri_handler(h, &u);
  }
}
