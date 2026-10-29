// setup.h — the setup page (docs/SPEC.md §5.6): WiFi network, Signal K server, device password.
//
//   GET  /        the page: from the setup hotspot (http://192.168.4.1/, phones open it by
//                 themselves), or from the boat network (http://<hostname>.local/)
//   POST /setup   save and restart. From the boat network it needs the current device password
//                 (a form field); from the hotspot it doesn't: its password is on the screen.
//
// While the hotspot is on, any other address redirects to the page (the phone's captive check).
#pragma once

#include "esp_http_server.h"

void setup_register(httpd_handle_t server, const char *hostname);
