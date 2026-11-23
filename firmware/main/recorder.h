// Recordings of login-page traffic, one JSON object per request/response, in
// /data/rec-<n>-<via>.jsonl. Used to diagnose logins offline and to build
// recipes. The newest 12 are kept.
#pragma once
#include <stdio.h>

#include "cJSON.h"

// Opens a new recording; `via` is "auto", "mirror" or "test". Returns NULL on error.
FILE *rec_open(const char *via, char *name_out, size_t cap);
void rec_close(FILE *f);
cJSON *rec_list_json(void);
// Full path for a recording name, or NULL if the name is not a recording.
const char *rec_path(const char *name, char *buf, size_t cap);
int rec_delete(const char *name);
// Name of the newest recording, "" if none.
void rec_latest(char *out, size_t cap);
