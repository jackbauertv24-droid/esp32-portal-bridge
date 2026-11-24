#include "calls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "devlog.h"
#include "esp_timer.h"
#include "events.h"
#include "memo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "modem_at.h"
#include "psram_task.h"
#include "settings.h"
#include "textutil.h"

typedef enum { C_IDLE, C_RINGING, C_PIN, C_ACTIVE, C_MEMO } cstate_t;
static const char *const STATE_NAME[] = {"idle", "ringing", "waiting for PIN", "connected", "recording voice memo"};

typedef struct {
  call_urc_t t;
  char arg[28];
} cev_t;

#define LOG_N 20
typedef struct {
  time_t t;
  char number[24];
  char result[48];
  int secs;
} clog_t;

static QueueHandle_t q;
static SemaphoreHandle_t mtx;
static cstate_t state;
static char number[24];
static int rings;
static int64_t last_ring_us, answered_us, pin_until_us, unlocked_us;
static bool decided, mic_muted, spk_muted;
static keypad_t kp;
static unsigned keys_seen, keys_total;
static clog_t logv[LOG_N];
static int log_next;
// Brute-force guard: wrong-PIN calls in the last hour.
static int64_t bad_calls_us[5];
static int64_t locked_until_us;
// Voice memo being recorded on the modem.
static char memo_path[24];
static int memo_seq;
static int64_t memo_start_us, memo_until_us;

static int64_t now_us(void) { return esp_timer_get_time(); }

void calls_on_urc(call_urc_t t, const char *arg) {
  if (!q) return;
  cev_t e = {.t = t};
  snprintf(e.arg, sizeof(e.arg), "%s", arg ? arg : "");
  xQueueSend(q, &e, 0);
}

static bool at(const char *cmd) {
  bool ok = false;
  free(modem_cmd(cmd, 5000, &ok));
  return ok;
}

static void set_mic(bool muted) {
  at(muted ? "AT+CMUT=1" : "AT+CMUT=0");
  mic_muted = muted;
}

static void set_spk(bool muted) {
  at(muted ? "AT+VMUTE=1" : "AT+VMUTE=0");
  spk_muted = muted;
}

static void tones(const char *keys) {
  if (!cfg_bool("ca_tones")) return;
  for (; *keys; keys++) {
    char c[16];
    snprintf(c, sizeof(c), "AT+VTS=%c", *keys);
    at(c);
  }
}

static void add_log(const char *result) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  clog_t *l = &logv[log_next++ % LOG_N];
  l->t = time(NULL);
  snprintf(l->number, sizeof(l->number), "%s", number[0] ? number : "hidden");
  snprintf(l->result, sizeof(l->result), "%s", result);
  l->secs = answered_us ? (int)((now_us() - answered_us) / 1000000) : 0;
  xSemaphoreGive(mtx);
}

// Ends our handling of a call: hangs up if asked, restores normal audio so
// calls from the Debug page work as usual, logs and reports.
static void finish(const char *result, bool hang_up) {
  if (hang_up) at("AT+CHUP");
  // Alerts only for calls auto-answer dealt with, not every missed call.
  bool report = state == C_PIN || state == C_ACTIVE || state == C_MEMO || (state == C_RINGING && cfg_bool("ca_en"));
  if (state == C_PIN || state == C_ACTIVE || state == C_MEMO) {
    set_mic(false);
    set_spk(false);
  }
  add_log(result);
  if (report) {
    char detail[40];
    snprintf(detail, sizeof(detail), " (%s)", number[0] ? number : "hidden number");
    event_post_detail(detail, "Incoming call: %s", result);
  }
  state = C_IDLE;
  number[0] = 0;
  rings = 0;
  decided = false;
  answered_us = unlocked_us = 0;
  keys_seen = 0;
  memset(&kp, 0, sizeof(kp));
}

static void note_bad_call(void) {
  int64_t t = now_us(), hour = 3600LL * 1000000;
  int recent = 0, oldest = 0;
  for (int i = 0; i < 5; i++) {
    if (bad_calls_us[i] && t - bad_calls_us[i] < hour) recent++;
    if (bad_calls_us[i] < bad_calls_us[oldest]) oldest = i;
  }
  bad_calls_us[oldest] = t;
  if (recent + 1 >= 5) {
    locked_until_us = t + hour;
    memset(bad_calls_us, 0, sizeof(bad_calls_us));
    event_post("Calls: 5 calls with a wrong PIN within an hour; not answering calls for 1 hour");
  }
}

// No PIN: the caller may leave a message, recorded by the modem from the
// caller's side only (the board's microphone stays muted).
static void start_memo(void) {
  snprintf(memo_path, sizeof(memo_path), "C:/pbmemo%d.wav", memo_seq++ % 3);
  modem_file_delete(memo_path);  // the modem may append to an old file
  tones("1");
  char c[48];
  snprintf(c, sizeof(c), "AT+CREC=2,\"%s\"", memo_path);
  if (!at(c)) {
    finish("could not start recording a voice memo", true);
    return;
  }
  state = C_MEMO;
  memo_start_us = now_us();
  memo_until_us = memo_start_us + (int64_t)cfg_int("cm_max_s") * 1000000;
  devlog("calls: recording a voice memo");
}

static void stop_memo(bool hang_up) {
  at("AT+CREC=0");
  int secs = (int)((now_us() - memo_start_us) / 1000000);
  char num[24], res[48];
  snprintf(num, sizeof(num), "%s", number);
  if (hang_up) tones("1");
  snprintf(res, sizeof(res), "voice memo recorded (%d s)", secs);
  finish(secs >= 2 ? res : "voice memo too short, discarded", hang_up);
  if (secs >= 2) memo_add(memo_path, num, secs);
  else modem_file_delete(memo_path);
}

// Decides whether to pick up once enough rings have passed.
static void maybe_answer(void) {
  decided = true;
  if (!cfg_bool("ca_en")) return;  // ring on; nothing to do
  char pin[12], allowed[128];
  cfg_get("ca_pin", pin, sizeof(pin));
  cfg_get("ca_from", allowed, sizeof(allowed));
  bool memo = cfg_bool("cm_en"), has_pin = strlen(pin) == 6;
  if (!has_pin && !memo) {
    devlog("calls: not answering, no 6-digit PIN set");
    return;
  }
  if (allowed[0] && !phone_in_list(number, allowed)) {
    devlog("calls: not answering, caller not on the allowed list");
    return;
  }
  if (locked_until_us && now_us() < locked_until_us) {
    devlog("calls: not answering, locked after wrong PINs");
    return;
  }
  // Mute first, so not a moment of room audio goes out before the PIN.
  set_mic(true);
  set_spk(true);
  if (!at("ATA")) {
    devlog("calls: answer failed");
    set_mic(false);
    set_spk(false);
    return;
  }
  set_mic(true);
  set_spk(true);
  char un[8], mu[8];
  cfg_get("ca_unmute", un, sizeof(un));
  cfg_get("ca_mute", mu, sizeof(mu));
  keypad_init(&kp, pin, un, mu);
  memset(pin, 0, sizeof(pin));
  state = C_PIN;
  answered_us = now_us();
  pin_until_us = answered_us + (int64_t)cfg_int("ca_pin_s") * 1000000;
  keys_seen = 0;
  if (!has_pin) {
    devlog("calls: answered for a voice memo (no PIN set)");
    start_memo();
    return;
  }
  devlog("calls: answered, waiting for the PIN (all audio muted)");
}

static void on_key(char key) {
  keys_seen++;
  keys_total++;
  devlog("calls: key pressed");  // never the key itself: it may be the PIN
  kp_event_t e = keypad_feed(&kp, key);
  if (e == KP_PIN_OK) {
    state = C_ACTIVE;
    unlocked_us = now_us();
    tones("1");
    set_mic(false);  // the caller now hears the room
    set_spk(true);   // the caller's voice stays off the speaker until unmuted
    char detail[40];
    snprintf(detail, sizeof(detail), " (%s)", number[0] ? number : "hidden number");
    event_post_detail(detail, "Incoming call: PIN accepted, board microphone on");
  } else if (e == KP_PIN_BAD) {
    tones("000");
    if (kp.bad_tries >= 3) {
      note_bad_call();
      finish("wrong PIN 3 times, hung up", true);
    }
  } else if (e == KP_UNMUTE) {
    set_spk(false);
    devlog("calls: caller unmuted the board speaker");
  } else if (e == KP_MUTE) {
    set_spk(true);
    devlog("calls: caller muted the board speaker");
  }
}

static void task(void *arg) {
  for (;;) {
    cev_t e;
    if (xQueueReceive(q, &e, pdMS_TO_TICKS(200)) == pdTRUE) {
      switch (e.t) {
        case CU_RING:
          if (state == C_IDLE) {
            state = C_RINGING;
            rings = 0;
            decided = false;
          }
          if (state == C_RINGING) {
            rings++;
            last_ring_us = now_us();
          }
          break;
        case CU_CLIP:
          if (state == C_RINGING) snprintf(number, sizeof(number), "%s", e.arg);
          break;
        case CU_DTMF:
          if (state == C_PIN || state == C_ACTIVE) on_key(e.arg[0]);
          break;
        case CU_END:
        case CU_MISSED:
          if (state == C_MEMO) stop_memo(false);
          else if (state == C_ACTIVE) finish("call ended", false);
          else if (state == C_PIN) finish("caller hung up before the PIN", false);
          else if (state == C_RINGING) finish(cfg_bool("ca_en") ? "not answered" : "missed (auto-answer off)", false);
          break;
        case CU_REC_FULL:
          if (state == C_MEMO) stop_memo(true);
          break;
        default:
          break;
      }
    }
    int64_t t = now_us();
    if (state == C_RINGING) {
      if (!decided && rings >= cfg_int("ca_rings")) maybe_answer();
      // Rings come every few seconds; silence means the caller gave up.
      else if (state == C_RINGING && t - last_ring_us > 10LL * 1000000)
        finish(cfg_bool("ca_en") ? "not answered" : "missed (auto-answer off)", false);
    } else if (state == C_PIN && cfg_bool("cm_en") &&
               ((keys_seen == 0 && t - answered_us > (int64_t)cfg_int("cm_wait_s") * 1000000) || t > pin_until_us)) {
      start_memo();  // no PIN: let the caller leave a message
    } else if (state == C_PIN && t > pin_until_us) {
      tones("000");
      note_bad_call();
      finish(keys_seen ? "PIN not completed in time, hung up" : "no keys received, hung up", true);
    } else if (state == C_MEMO && t > memo_until_us) {
      stop_memo(true);
    } else if (state == C_ACTIVE) {
      int max_min = cfg_int("ca_max_min");
      if (max_min > 0 && t - unlocked_us > (int64_t)max_min * 60 * 1000000) finish("time limit reached, hung up", true);
    }
  }
}

void calls_start(void) {
  mtx = xSemaphoreCreateMutex();
  q = xQueueCreate(32, sizeof(cev_t));
  task_psram(task, "calls", 4096, NULL, 4, 1);  // AT commands and queues only, no flash access
}

bool calls_action(const char *a, char *msg, size_t cap) {
  if (state != C_PIN && state != C_ACTIVE && state != C_MEMO) {
    snprintf(msg, cap, "No call in progress.");
    return false;
  }
  if (!strcmp(a, "hangup")) {
    // The calls task will log the end when the modem reports it.
    at("AT+CHUP");
    snprintf(msg, cap, "Hung up.");
  } else if (!strcmp(a, "mic_on") || !strcmp(a, "mic_off")) {
    set_mic(!strcmp(a, "mic_off"));
    snprintf(msg, cap, "Board microphone %s.", mic_muted ? "muted" : "on");
  } else if (!strcmp(a, "spk_on") || !strcmp(a, "spk_off")) {
    set_spk(!strcmp(a, "spk_off"));
    snprintf(msg, cap, "Board speaker %s.", spk_muted ? "muted" : "on");
  } else {
    snprintf(msg, cap, "unknown action");
    return false;
  }
  return true;
}

cJSON *calls_json(void) {
  cJSON *o = cJSON_CreateObject();
  char pin[12];
  cfg_get("ca_pin", pin, sizeof(pin));
  cJSON_AddBoolToObject(o, "enabled", cfg_bool("ca_en"));
  cJSON_AddBoolToObject(o, "pin_set", strlen(pin) == 6);
  memset(pin, 0, sizeof(pin));
  cJSON_AddStringToObject(o, "state", STATE_NAME[state]);
  cJSON_AddStringToObject(o, "number", state == C_IDLE ? "" : number);
  cJSON_AddNumberToObject(o, "rings", rings);
  cJSON_AddNumberToObject(o, "seconds", answered_us ? (double)((now_us() - answered_us) / 1000000) : 0);
  cJSON_AddNumberToObject(o, "keys_seen", keys_seen);
  cJSON_AddNumberToObject(o, "keys_total", keys_total);
  cJSON_AddBoolToObject(o, "mic_muted", mic_muted);
  cJSON_AddBoolToObject(o, "spk_muted", spk_muted);
  int64_t lock = locked_until_us > now_us() ? (locked_until_us - now_us()) / 1000000 : 0;
  cJSON_AddNumberToObject(o, "locked_s", (double)lock);
  cJSON *arr = cJSON_AddArrayToObject(o, "log");
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 1; i <= LOG_N; i++) {
    clog_t *l = &logv[(log_next - i + LOG_N * 2) % LOG_N];
    if (!l->t) continue;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "t", (double)l->t);
    cJSON_AddStringToObject(j, "number", l->number);
    cJSON_AddStringToObject(j, "result", l->result);
    cJSON_AddNumberToObject(j, "seconds", l->secs);
    cJSON_AddItemToArray(arr, j);
  }
  xSemaphoreGive(mtx);
  return o;
}

// Counts only: caller numbers stay out of reports.
void calls_summary(char *out, size_t cap) {
  int n = 0;
  for (int i = 0; i < LOG_N; i++) n += logv[i].t != 0;
  snprintf(out, cap, "calls: auto-answer %d, state %s, logged %d, keys received since start %u, locked %d\n",
           cfg_bool("ca_en"), STATE_NAME[state], n, keys_total, locked_until_us > now_us());
}
