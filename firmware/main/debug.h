// Hardware test API for the admin app's Debug page.
#pragma once
#include "esp_http_server.h"

void debug_register(httpd_handle_t h, esp_err_t (*gate)(httpd_req_t *));
