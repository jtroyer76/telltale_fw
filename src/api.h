// api.h — HTTP API for config, status and widgets (docs/SPEC.md §6.4, §6.3).
//
//   GET    /api/config           running config (X-Config-Source: saved | last-known-good | built-in)
//   PUT    /api/config           validate, save, apply without reboot            [password]
//   POST   /api/config/validate  validate only
//   GET    /api/config/lkg       the last-known-good config, 404 if none
//   DELETE /api/config           forget saved configs, run the built-in one       [password]
//   GET    /api/widgets          widget descriptors (for an editor)
//   GET    /api/status           connection, screen, config source, subscribed values
//   POST   /api/screen?index=N   show screen N
//   POST   /api/restart                                                           [password]
//   POST   /api/wifi/setup       turn the setup hotspot on (setup.h)                [password]
//   GET    /api/screenshot       what the screen shows, as a BMP
//
// [password]: header X-OTA-Password (auth.h). Replies are JSON: {"ok":true,...} or {"ok":false,"error":"..."}.
#pragma once

#include "esp_http_server.h"

void api_register(httpd_handle_t server);
