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
#include "esp_camera.h"
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

static esp_err_t h_camera(httpd_req_t *req) {
  if (gate(req) != ESP_OK) return ESP_OK;
  char sz[8];
  query(req, "size", sz, sizeof(sz));
  framesize_t fs = !strcmp(sz, "uxga") ? FRAMESIZE_UXGA : !strcmp(sz, "vga") ? FRAMESIZE_VGA : FRAMESIZE_SVGA;
  // Waveshare pin map for this board (CAMERA_MODEL_WAVESHARE_7670_BOARD in their demo).
  camera_config_t c = {
      .pin_pwdn = -1,
      .pin_reset = -1,
      .pin_xclk = 39,
      .pin_sccb_sda = 15,
      .pin_sccb_scl = 16,
      .pin_d7 = 14,
      .pin_d6 = 13,
      .pin_d5 = 12,
      .pin_d4 = 11,
      .pin_d3 = 10,
      .pin_d2 = 9,
      .pin_d1 = 8,
      .pin_d0 = 7,
      .pin_vsync = 42,
      .pin_href = 41,
      .pin_pclk = 46,
      .xclk_freq_hz = 20000000,
      .ledc_timer = LEDC_TIMER_0,
      .ledc_channel = LEDC_CHANNEL_0,
      .pixel_format = PIXFORMAT_JPEG,
      .frame_size = fs,
      .jpeg_quality = 12,
      .fb_count = 1,
      .fb_location = CAMERA_FB_IN_PSRAM,
      .grab_mode = CAMERA_GRAB_LATEST,
  };
  battery_suspend();  // the camera's control bus shares GPIO15/16 with the battery gauge
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) {
    battery_resume();
    char m[200];
    snprintf(m, sizeof(m), "camera: init failed (%s). Is the camera DIP switch ON and the ribbon cable seated?",
             esp_err_to_name(e));
    return send_json(req, result(false, m));
  }
  sensor_t *s = esp_camera_sensor_get();
  const camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
  char name[48];
  snprintf(name, sizeof(name), "%s (PID 0x%04x)", info ? info->name : "unknown", s ? s->id.PID : 0);
  camera_fb_t *fb = NULL;
  for (int i = 0; i < 4; i++) {  // let exposure settle
    if (fb) esp_camera_fb_return(fb);
    fb = esp_camera_fb_get();
  }
  esp_err_t out;
  if (!fb) {
    char m[120];
    snprintf(m, sizeof(m), "camera: sensor %s found but no frame captured", name);
    out = send_json(req, result(false, m));
  } else {
    devlog("debug: PASS camera %s, %ux%u JPEG %u bytes", name, fb->width, fb->height, (unsigned)fb->len);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "X-Camera", name);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    out = httpd_resp_send(req, (const char *)fb->buf, (ssize_t)fb->len);
    esp_camera_fb_return(fb);
  }
  esp_camera_deinit();
  battery_resume();
  return out;
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
           {"/api/debug/temp", h_temp},     {"/api/debug/led", h_led},   {"/api/debug/button", h_button}};
  for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
    httpd_uri_t u = {.uri = r[i].uri, .method = HTTP_GET, .handler = r[i].fn};
    httpd_register_uri_handler(h, &u);
  }
}
