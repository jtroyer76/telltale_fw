// settings.h — per-device settings, kept in NVS rather than in the config: the WiFi network,
// the device password (auth.h), and an optional Signal K server that overrides the config's
// (docs/SPEC.md §5.6). Set from the setup page (setup.c). Nothing is compiled in: a build
// with src/secrets.h (optional, gitignored) uses it for any setting not yet saved.
#pragma once

#include <stdbool.h>

#define SETTINGS_SSID_LEN     33   // 32 + NUL (802.11 limit)
#define SETTINGS_WIFI_PW_LEN  65   // WPA2: 8..63, or "" for an open network
#define SETTINGS_PW_MIN       8    // device password
#define SETTINGS_PW_LEN       33
#define SETTINGS_HOST_LEN     64

typedef struct {
    char wifi_ssid[SETTINGS_SSID_LEN];      // "" = none saved: start the setup hotspot
    char wifi_pass[SETTINGS_WIFI_PW_LEN];
    char password[SETTINGS_PW_LEN];         // device password; made up on first boot if none
    char sk_host[SETTINGS_HOST_LEN];        // "" = the config's signalk.host (or mDNS)
    int sk_port;
} settings_t;

// Loads the settings. Call after nvs_flash_init.
void settings_init(void);

void settings_get(settings_t *out);

// Saves all fields for the next boot (they apply after a restart). False on an NVS error.
bool settings_save(const settings_t *s);

// Checks a setting before it's saved; NULL if fine, else what's wrong.
const char *settings_check(const settings_t *s);
