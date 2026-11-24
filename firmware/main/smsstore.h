// Durable SMS store on the board's flash (/data/sms.jsonl). A message is only
// removed from the SIM after smsstore_add() has written and synced it here.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct {
  uint32_t id;
  int64_t rx;         // unix time received by the board (0 if clock not set yet)
  char from[48];
  char sent[48];      // service-centre timestamp from the PDU
  char *text;         // malloc'd
  int parts;
  bool otp;           // matched the login-code rule when it arrived
} sms_rec_t;

void smsstore_init(void);
// Returns the new id, or 0 if it could not be written.
uint32_t smsstore_add(const char *from, const char *sent, const char *text, int parts);
uint32_t smsstore_last_id(void);
// Reads message `id`; returns true if found. Free rec->text afterwards.
bool smsstore_get(uint32_t id, sms_rec_t *rec);
// Newest first, at most `limit`.
cJSON *smsstore_list_json(int limit);
int smsstore_count(void);

// Waits for a login code in an SMS that arrived after `since_us`
// (esp_timer time). Applies the otp_* settings. Returns true with the code.
bool sms_wait_code(int64_t since_us, int timeout_s, char *code, size_t cap);
// Checks whether `from`/`text` match the login-code rule; writes the code.
bool sms_match_code(const char *from, const char *text, char *code, size_t cap);

// ---------- SMS manager ----------

// Records an outgoing message; returns its id (0 on failure).
uint32_t smsstore_add_sent(const char *to, const char *text, int parts, bool ok, const char *error);
cJSON *smsstore_list_sent_json(int limit);
// Removes messages by id from the inbox ("in") or sent ("out") store.
// Returns how many were removed.
int smsstore_delete(const char *box, const uint32_t *ids, int n);
// Unread tracking: everything with id > seen is unread.
uint32_t smsstore_seen(void);
void smsstore_set_seen(uint32_t id);
