// ota.h — firmware update over WiFi and rollback protection.
//
//   POST /ota       body = firmware.bin, header X-OTA-Password: <the device password, settings.h>
//   GET  /api/info  version and running partition
//
// A new image must get a WiFi IP within OTA_HEALTHY_TIMEOUT_S of its first boot,
// or the device restarts into the previous image.
#pragma once

#include "esp_http_server.h"

#define OTA_HEALTHY_TIMEOUT_S  300

// Registers the OTA and info handlers on `server`.
void ota_register(httpd_handle_t server);

// Starts the task that confirms a freshly updated image once WiFi is up.
void ota_start_health_check(void);

// Upload progress 0-100 while an update is being received, -1 otherwise.
int ota_progress(void);
