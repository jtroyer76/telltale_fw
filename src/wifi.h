// wifi.h — station mode with automatic reconnect, plus mDNS (<hostname>.local); and the setup
// hotspot (docs/SPEC.md §5.6), where a phone joins the display to choose its WiFi network.
// The network comes from settings.h.
//
// The hotspot comes on by itself when no network is saved, or when the saved one hasn't been
// joined for SETUP_AFTER_S; or from the Settings screen. While it's on, the saved network isn't
// tried (trying it scans every channel, which drops the phone). With a network saved, it goes
// off again after SETUP_IDLE_S with no phone on it, and the saved network is tried again.
#pragma once

#include <stdbool.h>

#define SETUP_AFTER_S   180
#define SETUP_IDLE_S    300
#define SETUP_IP        "192.168.4.1"
#define SETUP_MAX_NETS  16

void wifi_start(const char *hostname);
bool wifi_connected(void);
// Current IP as text, or "" when not connected.
const char *wifi_ip(void);

// Signal strength (dBm) and channel of the access point; false when not associated.
bool wifi_link(int *rssi, int *channel);
// Times the link dropped since boot, and the last wifi_err_reason_t (0 = none yet).
int wifi_disconnects(int *last_reason);

// Setup hotspot on/off (any task; takes effect within a second). Off does nothing without a
// saved network.
void wifi_setup_start(void);
void wifi_setup_stop(void);

typedef struct {
    bool on;
    char ssid[33];       // the hotspot's name and password, for the screen
    char pass[16];
    int phones;          // joined now
    bool have_saved;     // a saved network to go back to
    char saved[33];
} wifi_setup_t;
void wifi_setup_info(wifi_setup_t *out);

// Networks found as the hotspot came on, strongest first, no repeats. Returns how many.
int wifi_setup_networks(char names[][33], int max);
