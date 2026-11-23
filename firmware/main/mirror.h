// Assisted login: the board fetches the upstream login page itself and serves a
// rewritten copy at /p/<scheme>/<host>/<path> to a phone on the hotspot. Typing
// <phone> or <code> into the page makes the board substitute the SIM's number
// or the code from the next SMS. All traffic is recorded, and because the board
// makes every request, the login authorises the board.
#pragma once
#include "cJSON.h"
#include "esp_http_server.h"

void mirror_register(httpd_handle_t h, esp_err_t (*auth)(httpd_req_t *));
void mirror_status_json(cJSON *root);
