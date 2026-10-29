// auth.h — the device password (settings.h) for HTTP requests that change things:
// firmware updates, config writes, restart, settings. Sent as the X-OTA-Password header.
#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

// True if the request carries the right password. Otherwise sends 401 and returns false.
bool auth_check(httpd_req_t *req);

// True if `given` is the device password (constant time).
bool auth_password_ok(const char *given);
