// Optional, for development builds: copy to src/secrets.h (gitignored) and fill in.
// Each value is used only until that setting is saved on the device (setup page,
// docs/SPEC.md §5.6). Release builds have no secrets.h: the display starts its setup hotspot.
#pragma once
#define WIFI_SSID  "boat-network"
#define WIFI_PASS  "boat-network-password"
#define OTA_PASS   "change-me-ota"   // also export it as TELLTALE_OTA_PASS for `pio run -e lcd7-ota`

// Optional, for desk testing away from the boat: connect to a relay instead of the
// Signal K server named in the config (the setup page's Signal K server does the same).
// #define SK_HOST_OVERRIDE  "192.168.1.50"
// #define SK_PORT_OVERRIDE  3001
