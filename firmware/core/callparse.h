// Incoming-call helpers (pure C, host-tested in tests/test_call.c): reading
// the modem's call and key-press (DTMF) notifications, and the keypad logic
// for PIN entry and mute/unmute sequences.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  CU_NONE = 0,  // not a call notification
  CU_RING,
  CU_CLIP,      // caller id; arg = number ("" if hidden)
  CU_BEGIN,     // call connected
  CU_END,       // call ended
  CU_MISSED,
  CU_DTMF,      // key pressed by the caller; arg[0] = key
  CU_REC_FULL,  // call recording stopped by the modem (file full)
} call_urc_t;

// Classifies one line from the modem. A76XX firmware is not documented for
// key reports, so every known spelling is accepted: "+RXDTMF: 5", "+DTMF: 5",
// "+DTMF:5", "+QTONEDET: 53" (ASCII code) and similar.
call_urc_t call_urc_parse(const char *line, char *arg, size_t cap);

// True for a line that may be a key report but did not parse, so it can be
// logged (with the value hidden) to learn the format.
bool call_urc_maybe_dtmf(const char *line);

typedef enum { KP_NONE = 0, KP_PIN_OK, KP_PIN_BAD, KP_UNMUTE, KP_MUTE } kp_event_t;

typedef struct {
  char pin[8];        // expected PIN
  char unmute[8];     // e.g. "*1"
  char mute[8];       // e.g. "*0"
  bool unlocked;
  int bad_tries;
  char buf[16];       // keys typed since the last decision
  size_t n;
} keypad_t;

void keypad_init(keypad_t *k, const char *pin, const char *unmute, const char *mute);
// Feeds one key. Before unlock, digits build the PIN ('*' or '#' clears it);
// a full-length entry gives KP_PIN_OK or KP_PIN_BAD. After unlock, the mute
// and unmute sequences are matched on the most recent keys.
kp_event_t keypad_feed(keypad_t *k, char key);
