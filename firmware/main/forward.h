// Forwards every stored SMS (and device events) to the enabled channels:
// Telegram bot, HTTPS webhook, MQTT over TLS. Each channel remembers the last
// message id it delivered, so nothing is skipped across restarts or outages.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

void forward_start(void);
void forward_kick(void);
void forward_status_json(cJSON *root);
void forward_summary(char *out, size_t cap);

// Sends a test message on one channel ("tg", "wh", "mq"); writes the outcome.
bool forward_test(const char *ch, char *result, size_t cap);
// Lists chats that recently messaged the bot, as [{id, name}], into result JSON.
bool telegram_find_chats(char *result, size_t cap);
// Uploads a text file to Telegram as a document.
bool telegram_send_document(const char *filename, const char *data, size_t len, char *result, size_t cap);
// Uploads any file as a Telegram document with an optional caption, to
// chat_override or (NULL/"") the forwarding chat.
bool telegram_send_file(const char *chat_override, const char *filename, const char *ctype, const char *caption,
                        const char *data, size_t len, char *result, size_t cap);
// POSTs a file as the raw body to url, with the forwarding webhook's auth
// header and an X-Caller header.
bool webhook_send_file(const char *url, const char *ctype, const char *caller, const char *data, size_t len,
                       char *result, size_t cap);
// Creates a GitHub issue with `body`, then adds `extra` (may be NULL) as
// comments in chunks. Writes the issue URL or an error into result.
bool github_create_issue(const char *title, const char *body, const char *extra, char *result, size_t cap);
