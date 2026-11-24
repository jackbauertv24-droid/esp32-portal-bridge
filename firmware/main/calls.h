// Incoming calls: answered automatically with all audio muted, opened only
// after the caller keys in the PIN set on the admin page. The board's
// microphone then goes live; the board's speaker (the caller's voice) stays
// muted until the caller keys the unmute sequence.
#pragma once
#include <stdbool.h>

#include "cJSON.h"
#include "callparse.h"

void calls_start(void);
// Called by the modem module for each call notification (never blocks).
void calls_on_urc(call_urc_t t, const char *arg);
cJSON *calls_json(void);
// "hangup", "mic_on", "mic_off", "spk_on", "spk_off"
bool calls_action(const char *action, char *msg, size_t cap);
void calls_summary(char *out, size_t cap);
