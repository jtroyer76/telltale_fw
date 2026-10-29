// sk_client.h — Signal K WebSocket stream client (docs/SPEC.md §5.2).
// Subscribes to the paths the store says are needed now (the visible screen plus background
// paths), with metadata, and writes values and metadata into the store.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Starts the client. An empty host means: find the server by mDNS (_signalk-ws._tcp).
void sk_client_start(const char *host, int port);

// Switches server (config change). Same rules as sk_client_start.
void sk_client_set_server(const char *host, int port);

// Re-sends the subscription (screen change, config change). Safe from any task; doesn't block.
void sk_client_resubscribe(void);

bool sk_client_connected(void);

// "host:port" in use (or being looked for), for status displays.
void sk_client_server(char *buf, int len);

// esp_timer time of the last message from the server, 0 = none yet.
int64_t sk_client_last_message_us(void);
