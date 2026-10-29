# telltale_fw — Spec (draft)

Status: draft, 2026-10-27. Sections marked **(verify)** rest on facts not yet checked against hardware or the Signal K server.

**Status (2026-10-28):** Phases 2 and 3 are built and tested on the desk (§7): config saved on the device and changed over HTTP without a rebuild or reboot, last-known-good fallback, widget registry and descriptors, per-screen subscribe, Signal K metadata (units, decimals, zones, displayName), staleness from the server's timestamps, and `auto_screen` (switch screens from `navigation.state` or any text value, §6.6). Host tests: `test/host/run.sh`. Phase 4 widgets built: `ring`, `bar`, `state`, `alert` (§6.3). Derived values are computed in Signal K, not on the display (§6.5). An example boat's screens: `examples/sailboat.json` (§7 Phase 4). Not yet: the Phase 1 24 h soak, mDNS discovery of Signal K on the boat.

## 1. Purpose

A wall- or bulkhead-mounted touch display that shows Signal K data on screens defined entirely by config. First use is on a cruising sailboat; the firmware has nothing boat-specific in it.

### Goals

- **Config-driven.** Screens, widgets, paths, units, thresholds, labels and layout come from a JSON config. Adding a screen or changing a path never needs a rebuild.
- **Editor-ready.** The config format and the widget descriptors (§6.3) are what a future `telltale_editor` uses. Going from hand-written JSON to an editor needs no firmware rewrite.
- **Board-independent screens.** Layout is in grid units, not pixels, so one config can run on different boards.
- **Honest data.** Missing or stale values are shown as missing or stale, never as zero or the last value.
- **Signal K is the source of truth** for data, metadata (units, zones) and alarms (notifications).

### Non-goals (for now)

- NMEA 2000 or any data source other than Signal K.
- Writing to Signal K (no controls, no PUT requests) — except acknowledging and silencing notifications (Phase 5).
- Long-term history (InfluxDB). Graphs only keep what the device saw since boot.
- The editor itself (separate repo, later).

## 2. Key decisions

| Decision | Choice | Why |
|---|---|---|
| Data source | Signal K WebSocket stream only | Victron, RuuviTag and N2K data already reach Signal K on the first boat |
| Screens | JSON config, no screens in code | Main requirement; avoids the hardcoded layouts in Boatingwiththebaileys/Marine-Displays |
| Board support | Compiled-in board profile per board (one build env each) | Pins and panel timing are hardware facts, not user config |
| Layout units | Grid cells per screen | Same config scales to other resolutions |
| Subscriptions | Only the paths the visible screen (and any background widgets) need | Keeps WiFi and server load low; same idea as Marine-Displays |
| Units and thresholds | From Signal K metadata by default, overridable in config | One place to set alarm limits (the server) |
| Config schema | [`docs/config.schema.json`](config.schema.json) in this repo; the firmware's checks (`src/config.c`) are the reference | Moves to its own repo only if the editor needs it |
| Framework | ESP-IDF 5.5.0 via PlatformIO (`espressif32@6.12.0`), LVGL 8.4.0 | Waveshare's LVGL demo for this board is ESP-IDF (Espressif RGB panel example, bounce buffer + anti-tear tuned); IDF has WebSocket client, HTTP server, cJSON, OTA built in. Platform 7.x/8.x moves to IDF 6.1 — not tried; the board code uses the legacy I2C driver |
| License | MIT | |

## 3. Hardware

### 3.1 First board profile: Waveshare ESP32-S3-Touch-LCD-7

From the Waveshare wiki (`waveshare.com/wiki/ESP32-S3-Touch-LCD-7`), read 2026-10-27:

| Item | Value |
|---|---|
| Module | Wiki says ESP32-S3-WROOM-1-N16R8 / 16 MB flash in one place and 8 MB in another. **The board on hand reads 8 MB flash** (esptool `flash_id`, 2026-10-27); 8 MB octal PSRAM per Waveshare's demo config, which runs |
| Display | 7" IPS, 800×480, RGB parallel interface. Driver IC not named on the wiki |
| Touch | Capacitive, GT911, I2C on GPIO8/9, INT GPIO4, reset via CH422G EXIO1 |
| IO expander | CH422G — backlight enable, touch reset, TF card CS, CAN/USB select |
| SD card | TF slot on SPI (GPIO11/12/13), CS on CH422G EXIO4 |
| CAN | TJA1051 on GPIO20 (TX) / GPIO19 (RX), shared with USB via the CH422G select. Not used (§1 non-goals) |
| RS485 | GPIO15 (TX) / GPIO16 (RX). Not used |
| Power | 5 V USB-C; wiki gives 5 V / 450 mA typical. Optional 3.7 V Li cell on PH2.0 |

Wiki notes that affect the design: LVGL benchmark about 26 fps single-core at PCLK 21 MHz; flash and PSRAM clock settings must match (120 MHz); possible screen drift (links an Espressif FAQ); keep the PCB antenna clear of metal.

Board on hand is the touch variant (GT911 initialises). Revision not recorded.

Bring-up notes: USB serial is a CH343 (COM6 on the dev PC). The current GT911 component sets `scl_speed_hz`, which the legacy I2C path rejects — cleared in `board_lcd7.c`. The panel goes blank while flashing over USB (chip held in the bootloader) — expected. Flash writes while running (OTA) made the whole panel flash until `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` (code and rodata run from PSRAM, so flash writes don't stall the RGB panel's PSRAM framebuffer); costs about 1.4 MB of PSRAM, 6.8 MB left for the heap.

### 3.2 Power on the boat

The board takes 5 V only. Needs a fused 12 V → 5 V supply sized for the board's draw. Supply model and fuse size not chosen.

### 3.3 Mounting

A bulkhead visible from where people come and go. Near a companionway the enclosure must shed drips; keep metal away from the PCB antenna end. A central mount also makes it a candidate BLE gateway (Backlog).

### 3.4 Other boards

Each new board is a new profile (§5.1). No screen config changes.

## 4. Design rules

- No Signal K path, label, colour, threshold or position in code. If code needs one, it comes from config or Signal K metadata.
- Widgets only talk to the data store (§5.3), never to the WebSocket.
- Nothing blocks the UI task: network, config I/O and parsing run elsewhere.
- Every value has a state: `ok`, `stale`, `missing`, or a zone state from metadata. Widgets render each one visibly.
- A bad config never bricks the display: fall back to last-known-good, then to a built-in setup screen (§6.4).
- Config files carry `schema_version`; the firmware migrates older versions forward or refuses with a clear message.

## 5. Architecture

```
 board profile ──► display / touch / backlight / LVGL
                                │
 config.json ──► config loader ─┼─► screen builder ──► widgets (from registry)
                                │                         ▲
 Signal K ──WebSocket──► SK client ──► data store ────────┘
                         (subscribes to what the screens need)
```

### 5.1 Board profile (compiled in)

One header plus one PlatformIO env per board: panel timing, pins, touch, expander, backlight, resolution. Exposes resolution and capabilities (touch yes/no, SD yes/no) to the rest of the firmware.

### 5.2 Signal K client

- Server from config (`host`, `port`); an empty `host` means find it by mDNS (`_signalk-ws._tcp`, first IPv4 answer, retried every 10 s). A `.local` host name also resolves (lwIP mDNS queries are on). Built, not tested: there's no Signal K advertising on the home LAN. Planning assumption (owner, 2026-10-28): mDNS will work on the boat once Signal K runs outside Docker.
- Connect with `subscribe=none&sendMeta=all`, then subscribe to exactly the paths the visible screen and background uses (`auto_screen`) need. On a screen change: `{"context":"*","unsubscribe":[{"path":"*"}]}`, then the new subscribe. Checked on Signal K 2.26.0 (2026-10-28): unsubscribe-all works, and a new subscription replays each path's last value with its original timestamp.
- Metadata: `sendMeta=all` sends a `meta` delta for each newly subscribed path (checked on 2.26.0). The test server's metadata has `units` (SI), `displayUnits` (`targetUnit`, `symbol`, `displayFormat`, e.g. `F`, `foot`, `kn`, `percent`, `hour`, `degree`, `V`) and no zones on any path checked. Telltale uses `targetUnit` when it maps to its unit table and converts from the stated SI unit, and `displayFormat` for decimals; config overrides both. Units the table lacks (e.g. `btu`, which the test server picks for `yieldToday`) fall back to SI.
- Staleness by server time: the hello message's `timestamp` gives the server clock; each update's `timestamp` then says how old the value is, so a replayed value that is minutes old shows as stale at once. Timestamps ahead of the device clock by more than 2 s re-base it (the Pi has no RTC and may get NTP time late). No hello timestamp: ages count from arrival.
- Dead links: WebSocket ping every 10 s; no pong for 30 s drops and reconnects. A stream that is connected but silent for 30 s shows "Signal K: no data" on the status line.
- Silence watchdog: a fresh connection always gets the hello and the subscribed values' last state, so a "connected" stream silent for 60 s is dead and is reconnected; still silent at 180 s (client stuck), the display restarts. Added 2026-10-28 after the desk display sat "connected" for 9.7 h with nothing arriving: the relay had dropped the TCP connection, the ping/pong check never fired, and swiping (resubscribing) didn't help. Root cause in the client not found (no serial log at the time). Verified the same morning: the stream dropped about 4 min after boot and the watchdog reconnected 60 s later without a restart. The restart step is untested.
- Subscribe to `notifications.*` used by alert widgets.
- Reconnect every 5 s; on reconnect, re-subscribe and mark all values stale until fresh data arrives.
- Auth: the test server (Signal K 2.26.0) has security on with `readOnlyAccess: true` and device access requests allowed (`/skServer/loginStatus`, 2026-10-27). Reading needs no token. A token (device access request flow) is only needed if read-only access is turned off.
- Desk testing when the server can't be reached directly (e.g. only over a VPN the ESP32 can't join): `tools/sk_relay.py` on a PC forwards to it, with the display's Signal K server setting (§5.6) pointed at the PC. On a boat, if the server isn't found by mDNS, set its name or IP on the setup page.
- Subscriptions use `period: 1000` with the default policy. Values that stop updating go stale on the display as intended.
- Write token (Phase 5): owner's planning assumption (2026-10-28) is that Telltale will get a token with write access through the device access request flow.

### 5.3 Data store

Keyed by path (`$source` not yet). Each entry: value (number or text), when the server got it, metadata (SI unit, display unit and decimals, displayName, zones), which screens use it, and whether it is a background path. Staleness is checked against the binding's `stale_after_s` (§6.2). Derived values come from the Signal K server like any other path (§6.5); the store holds no computed entries.

### 5.4 Widget registry

Each widget type registers a type name, a descriptor (its props and bindings, with types and defaults — §6.3), and create/update functions. The screen builder only knows the registry. Built as two tables so the config side has no LVGL in it (and runs in the host tests): `src/widget_types.c` (names, bindings, descriptors) and `src/widgets.c` (drawing). A new type is an entry in each; boot logs an error if a type has no drawing code.

Readings (`value`, `state`, and the number inside `ring`) follow KIP's layout (ideas only, no code): caption at the top left, the number big and bold in the middle, the unit small in the bottom-right corner (under the number in a ring). Degrees stay on the number. The number takes the largest font that fits the space between caption and unit: Montserrat SemiBold at 96, 64, 48 and 36 px (`src/fonts/`, OFL-1.1, made with `lv_font_conv`; about 108 KB of flash), then LVGL's 24 and 16. So a long reading ("Not charging", a position on two lines) gets smaller instead of running off the card. Changed 2026-10-28 after the first screenshots showed text overflowing its card.

### 5.5 OTA and rollback (built)

- `POST /ota` on port 80, body = `firmware.bin`, header `X-OTA-Password` = the device password (§5.6, compared in constant time). Plain HTTP: keep the device on the boat LAN only.
- `GET /api/info` — version (git describe), running partition, IP.
- `pio run -e lcd7-ota -t upload` runs `tools/ota_upload.py` (password from `TELLTALE_OTA_PASS`, so it's not on the command line). Device found as `<hostname>.local` via mDNS; hostname from config `device.hostname`.
- Status line shows upload progress.
- Rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): a new image must get a WiFi IP within 5 min of first boot, or it restarts into the previous one; a crash before that also rolls back. WiFi is the test because it's what the next update needs.
- WiFi, HTTP and OTA start even when the config fails to parse.
- An upload that goes silent for 30 s is abandoned. Before this fix (2026-10-28) a stalled upload held the only HTTP task forever, and only a USB flash recovered it.
- Tested 2026-10-27: three updates over WiFi (`ota_0` ↔ `ota_1`), each confirmed itself; wrong password → 401. An actual rollback has not been tested.
- **WiFi interference from the panel's pixel clock — fixed 2026-10-28.** At the default GPIO drive, PCLK (GPIO7) radiated into the 2.4 GHz antenna: 10–65 % ping loss, pings up to 1.5 s, uploads 5–40 KB/s, stalled OTAs, with a −31 to −46 dBm signal 2 m from the AP while other WiFi devices on the same AP were clean. `board_lcd7.c` now sets PCLK to `GPIO_DRIVE_CAP_0` (weakest): 4 boots at 16 MHz, 0 % loss, 2–3 ms, uploads 270–300 KB/s, a 1.5 MB OTA in 7 s. How it was found (one variable at a time, a control device pinged alongside):
  - Panel output stopped: WiFi perfect. Panel stopped with core 1 reading PSRAM flat out: still perfect, so not PSRAM contention. CPU was >90 % idle on both cores throughout.
  - All-black image (same PSRAM traffic, quiet data lines): no better. PCLK pin alone disconnected (peripheral, DMA and bounce ISR still running): perfect. PCLK at drive 0: perfect.
  - Lower pixel clocks helped partly (12 MHz) but 8 MHz is below what the panel will display (it shows its colour-cycle test pattern). Not helpful or not needed: no bounce buffer (worse), panel ISR on core 1, LWIP/WiFi IRAM options, PSRAM 120 MHz (needs 120 MHz flash with HPM, not tried), 32 KB I-cache (RTC watchdog reset at boot).
  - A clock change over OTA (warm restart) can leave the panel blank until a power cycle; the same is reported for this board in the Home Assistant community thread.
  - Not done: the 24 h soak, and checking the panel at drive 0 across temperature.

### 5.6 Device settings and setup (built)

Settings that belong to one display rather than to its screens live in NVS, not in the config, and nothing is compiled in, so one release binary fits every boat (`src/settings.c`):

- WiFi network and password.
- Device password (§6.4 auth). Made up on first boot: 12 characters from `a-z 2-9`, without the look-alikes `0 O 1 l i`.
- Optional Signal K server (`host[:port]`), which wins over the config's `signalk` block. Blank uses the config's server, and an empty config host means mDNS.
- A development build with `src/secrets.h` (gitignored, optional) uses its values for any setting not yet saved. Once a setting is saved, the saved value wins, even when it is blank.

**Setup hotspot** (`src/wifi.c`):
- **When it comes on:** with no network saved, when the saved network hasn't been joined for 3 minutes, from Change WiFi in Settings, or from `POST /api/wifi/setup` (password).
- **Network:** `Telltale-XXXX` (from the MAC), WPA2, with a password made up at each boot. The name and password show on the screen with two QR codes, one to join and one for `http://192.168.4.1/`. Serial also logs the password, since reading serial needs the device in hand, as reading the screen does.
- **What goes on:** while it's on, the saved network isn't tried, because a connect attempt scans every channel and the phone drops. The networks in range are scanned just before it comes on, for the setup page.
- **When it goes off:** with a network saved, after 5 minutes with no phone joined, or on Cancel. Then the saved network is tried again, and after 3 more minutes without it the hotspot comes back.
- **Captive portal:** every DNS name answers `192.168.4.1` (`src/captive_dns.c`), and any unknown URL redirects to the page, so phones open it by themselves.

**Setup page** (`src/setup.c`):
- **Where it's served:** at `/` on the hotspot, and at `http://<hostname>.local/` on the boat network.
- **Fields:** WiFi network (with the scanned names to tap), WiFi password (blank keeps the saved one for the same network), Signal K server, and a new device password (blank keeps it).
- **Saving:** from the boat network, saving needs the current device password. From the hotspot it doesn't, because joining needs the password on the screen. Saving restarts the display.

**Settings on the screen** (`src/settings_ui.c`): the gear at the top right of every screen opens it.
- **It shows:** WiFi (name, signal, channel), the display's address, the Signal K server and whether it's connected, firmware version, config source, free memory, and the device password (hidden until Show).
- **Buttons:** Show (the password), Change WiFi, Restart, Close.
- **No typing on the display:** it has no keyboard. Anything typed is done on a phone.

**Release** (`.github/workflows/release.yml`, on a `v*` tag): builds without `secrets.h`, runs the host tests, and publishes a GitHub release with two files:
- `telltale-<ver>-full.bin`: everything, written at `0x0`.
- `telltale-<ver>-update.bin`: the app alone, for updates over WiFi.

The same run deploys the web flasher (`site/`, ESP Web Tools 10.4.0) to GitHub Pages, with the firmware copied into the site because release downloads don't allow cross-origin reads. Installing without erasing keeps a display's settings and screens.

Tested on the desk (2026-10-28):
- **Phone flow (owner's phone):** joined the hotspot, the setup page opened, Save and restart worked, and the display rejoined WiFi and Signal K with the saved settings.
- **API and timeout:** `POST /api/wifi/setup` brings the hotspot up within 4 s, with 6 networks scanned. With no phone joined, it went off after about 5 minutes, and WiFi and Signal K came back without a restart.
- **Plain HTTP:** a missing page gives 404 on the boat network.
- **Memory:** free internal RAM is about 31–36 KB with this build, down from about 45–50 KB.
- **Not tested:** a build without `secrets.h` (the release build will be the first), and the captive DNS from a PC.


## 6. Config

### 6.1 Files and delivery

- `config.json` on the device filesystem (LittleFS on the `storage` partition, mounted at `/data`; LittleFS survives power loss mid-write). `config.lkg.json` is the config that was running before the last change.
- The built-in config (`data/default_config.json`) runs when nothing is saved. It's generic for any boat: standard Signal K paths only (navigation, wind and water, and all notifications), no units given so the server's display units apply, and the server found by mDNS. Battery, solar and similar names differ from boat to boat, so they belong in a boat's own config (e.g. `examples/sailboat.json`).
- Upload raw JSON over HTTP (§6.4); `tools/telltale.py` wraps the API.
- Later: optionally pull config from the Signal K server so one editor can manage several displays. **(verify)** whether Signal K's application data storage fits this.

### 6.2 Shape (example, not final)

Paths below are illustrations. Real paths from the first boat are in §8 Resolved 3 and `examples/sailboat.json`.

```json
{
  "schema_version": 1,
  "device": { "name": "Nav station", "hostname": "telltale-nav" },
  "signalk": { "host": "<server host>", "port": <port> },
  "display": { "brightness": 80, "screen_timeout_s": 0 },
  "theme": "day",
  "screens": [
    {
      "id": "health",
      "title": "Boat health",
      "grid": { "cols": 6, "rows": 4 },
      "widgets": [
        { "type": "ring", "at": { "col": 0, "row": 0, "w": 2, "h": 2 },
          "label": "Battery", "min": 0, "max": 1,
          "bind": { "value": { "path": "electrical.batteries.<id>.capacity.stateOfCharge",
                               "unit": "%", "stale_after_s": 30 } } },
        { "type": "value", "at": { "col": 2, "row": 0, "w": 2, "h": 1 },
          "label": "Battery power",
          "bind": { "value": { "path": "electrical.batteries.<id>.power", "unit": "W" } } },
        { "type": "value", "at": { "col": 2, "row": 1, "w": 2, "h": 1 },
          "label": "Fridge",
          "bind": { "value": { "path": "<fridge temperature path>", "unit": "degC",
                               "zones": [ { "upper": 280.15, "state": "normal" },
                                          { "lower": 280.15, "state": "alarm" } ] } } }
      ]
    }
  ]
}
```

Rules (built and checked by `config_parse`, mirrored in `docs/config.schema.json`):

- `at` is in grid cells; the board profile maps cells to pixels. Widgets may not overlap or leave the grid.
- Strings too long for the device are rejected, never cut short (a cut path would bind the wrong value).
- A binding is a Signal K `path` (optional `source`, not built). Computed values are paths too, made on the server (§6.5).
- Signal K values are SI; `unit` is the display unit and the firmware converts. The unit table is part of the schema.
- `unit` and `decimals` default to Signal K's `displayUnits` / `displayFormat` (§5.2), then SI and 1 decimal.
- `zones` in config override zones from Signal K metadata; same shape as Signal K zones (`lower`, `upper`, `state`: `nominal`, `normal`, `alert`, `warn`, `alarm`, `emergency`). Zone limits are in SI, like the data. A value inside several zones takes the most severe. Colours: `alert`/`warn` amber, `alarm`/`emergency` red.
- `label` defaults to the metadata `displayName`, then to the path's last element.
- `signalk.host` empty → mDNS (§5.2). `device.hostname` changes apply after a restart (the PUT reply says `restart_required`).

### 6.3 Widget descriptors

`GET /api/widgets` returns every registered type with its props, bindings, types, defaults and allowed values. The editor builds its forms from this; the firmware validates config against the same rules.

First set:

| Type | Shows | Props | Bindings | Built |
|---|---|---|---|---|
| `value` | Number (with unit) or text, with label; colour from zones | `label` | `value` | yes |
| `ring` | 270° arc from `min` to `max`, value in the middle; arc in the zone colour (accent blue when in no warning zone) | `label`, `min` (default 0), `max` (required) — SI like the data | `value` | 2026-10-28 |
| `bar` | Bar from `min` to `max` with the value; horizontal (value above) or vertical (bar on the left) | `label`, `min`, `max`, `orientation` (default from the cell shape) | `value` | 2026-10-28 |
| `state` | An enum as text and colour, e.g. charger mode. Values not in the map show as received | `label`, `states`: up to 8 × `{value, text, state}`; `value` as received (numbers as written, booleans `true`/`false`), `state` a zone state for the colour | `value` | 2026-10-28 |
| `alert` | The most severe active notification (`alert` or worse) under a path prefix: its message on an amber (`alert`/`warn`) or red (`alarm`/`emergency`) card, "+N more" for the others; "No alerts" when none; "Alerts unknown: no Signal K" when not connected (a dead link must not look like a quiet boat) | `label` (default "Alerts") | `notifications`: `{prefix}` under `notifications.`, e.g. `electrical.batteries`; absent = all | 2026-10-28 |
| `tide` | Predicted tide curve from an hour back to `hours` ahead, every high and low labelled with height and local time, a dot at now, the lowest low ahead in amber with a footer "Lowest next 18 h: −2.3 ft at 1:47 AM"; the height now and ↑/↓ from the curve (so they agree with the dot; signalk-tides' `heightNow` was seen republishing an unchanged value for minutes, 2026-10-28), the binding's value until predictions arrive; the station name above the footer | `label` (default "Tide"), `api` (required: the tides API base on the Signal K server), `hours` (6–48, default 18) | `value`: the height now, e.g. `environment.tide.heightNow` | 2026-10-28 |
| `anchor` | North-up plot centred on the anchor: alarm circle, anchor, the boat as a triangle along its heading, and its swing trail; "distance / radius" on top; circle, boat and footer message red while a notification under the prefix is active or the boat is outside the circle. With no anchor set, centred on the boat with its recent track ("Anchor not set") | `label` (default "Anchor"), `trail_minutes` (10–720, default 240; the example uses 720, a night at anchor) | `value`: distance to the anchor (e.g. `navigation.anchor.currentRadius`); `position`, `anchor`, `radius` (required) and `heading` paths; `notifications.prefix` (optional) | 2026-10-28 |
| `label` | Static text | `label` (required) | — | yes |

`max` is required for gauges so a scale can't silently peg on a default that doesn't suit the value. Out-of-range values clamp to the ends; the number shown is still the real value. Signal K booleans are stored as the text `true`/`false` (for `state` and `value`).

`tide` details: predictions come from a Signal K tides API over HTTP, not the stream: the stream has only the *next* high and *next* low (`environment.tide.*`), which can't answer "how low tonight" when the next low is the small afternoon one (as on 2026-10-28: next low 6.8 ft at 1:43 PM, overnight low −2.3 ft at 1:47 AM). `src/tides.c` fetches `<api>/timeline` (level every 10 min) and `<api>/extremes` for the window every 30 min, retrying every 5 s until Signal K is connected and its clock known, then every 60 s on failure; the `vessel/default` URL redirects to the station's own, which the HTTP client follows. Predictions are signalk-tides' (offline, from NOAA harmonics; levels in metres above the station's datum, MLLW for NOAA). Curve and labels use the binding's unit rules. No computed depth at the low (owner: shows the transducer depth beside it and adjusts mentally). Parsing, interpolation and "lowest in a window" are host-tested against saved real responses (`test/host/fixtures/`). `GET /api/status` has a `tides` summary.

Memory note (2026-10-28): with the tide and history tasks and the HTTP client, free internal RAM at runtime is about 45–50 KB (it was about 100 KB before them). Watch it as features are added. Where it goes, besides WiFi and task stacks: our fixed tables are static arrays in internal RAM — `s_entries` (store, 96 paths) 43,008 bytes, `s_alerts` 5,888, `s_views` 4,608, `s_watches` 1,600 (`xtensa-esp32s3-elf-nm` on the build, 2026-10-28). Option when needed (not done): mark the store and alerts tables `EXT_RAM_BSS_ATTR` with `CONFIG_SPIRAM_ALLOW_BSS_EXT_MEM=y` to put them in PSRAM, freeing about 49 KB of internal RAM; check first that nothing touches them while flash is written. Task stacks can also move to PSRAM.

Clock: Signal K has no local time, only UTC (`navigation.datetime`). Telltale takes the server's clock from its hello and shows local time from a (-)hhmm offset path given in config, `"clock": { "offset_path": "environment.time.timezoneOffset", "hour12": true }`, published by [signalk-ships-time](https://github.com/meri-imperiumi/signalk-ships-time) in auto mode (offset from position, daylight saving included; installed 2026-10-28, publishes −700 for PDT). Without an offset, times show as UTC and say so.

`anchor` details: paths as hoekens-anchor-alarm publishes them (its `docs/API.md`, read 2026-10-28): `navigation.anchor.position` (`{latitude, longitude}`, null when raised), `currentRadius` and `maxRadius` (m; `maxRadius` null for non-circle watch zones, then no circle is drawn). The store now keeps positions (`{latitude, longitude}` objects) and nulls (value gone); a `value` widget bound to a position shows degrees and decimal minutes. The trail fades by age — six equal stretches of the trail time, oldest faint, newest full-strength blue and thicker — so how she's been lying reads at a glance; red stays reserved for the alarm (hoekens-anchor-alarm colours its track red → yellow → green, oldest to newest, but here red means alarm; owner agreed 2026-10-28). The trail is kept on the display (`src/track.c`, host-tested): a fix at most every 5 s and only after 0.5 m of movement, at least one a minute, 12000 points (16 h at the fastest rate, ~190 KB of PSRAM); with `history` set (e.g. `/signalk/v2/api/history/values`), the trail is first filled from the Signal K History API — the last `trail_minutes` of the position path at 30 s resolution (12 h ≈ 1440 rows, ~80 KB), fetched once in the background when the widget is built (`src/history.c`; a small scanner, not cJSON, which would need a few hundred KB of internal RAM for that many rows) — and put in front of the live 5 s points, so a restart or update doesn't lose the night. On the test server a history provider answers (which plugin isn't recorded; it returned 1441 positions for 12 h on 2026-10-28). Not the tracks plugin hoekens uses: its v1 route returns coordinates without times (85,327 points at once on the test server), and the fading bands need times. Without `history` the trail starts at boot. Bands are equal slices of `trail_minutes` (2 h each at 720). The widget's paths are subscribed whatever screen is on show so the trail has no gaps. View radius: the larger of the circle and the boat's distance, at least 15 m, plus 20 %. Redrawn at most every 2 s. Tested on the desk against a scripted fake server (anchor set, boat swinging ±60° on a 25 m arc bow-to-anchor, then dragging past a 40 m circle with an `emergency` notification) and on a boat's server with no anchor set (trail around the boat at the dock). Not yet seen with a real anchor set.

`alert` details: notifications live in their own table (`src/alerts.c`), not the value store, written by the SK client. Each alert widget subscribes `notifications.<prefix>` and `notifications.<prefix>.*` (or `notifications.*`) while its screen is on show — both, because a wildcard doesn't match the path itself and hoekens-anchor-alarm raises its alarm at exactly `notifications.navigation.anchor` (found 2026-10-28; with only the wildcard the anchor alarm would never have shown); Signal K replays the active notifications on subscribe (checked on the test server 2026-10-28, and that prefix patterns filter correctly). `normal`/`nominal` and null values count as cleared; an unknown state shows as `alert`. On reconnect the table is emptied and refilled by the replay. Tested on the desk against a scripted fake server (warn → alarm with "+1 more" → cleared → none → connection lost = unknown) and against a boat's server (two `normal` notifications → "No alerts"). `GET /api/status` has an `alerts` summary (live, active count, worst). This is a per-screen widget; the always-on alarm banner, acknowledge and announcing are Phase 5.

Later: `sparkline`/`graph` (since-boot history), `compass`, `wind`.

### 6.4 Config API and safety

Built 2026-10-28 (`src/api.h`):

- `GET /api/config` — running config; header `X-Config-Source`: `saved`, `last-known-good` or `built-in`.
- `PUT /api/config` — validate; on success save, keep the previous one as last-known-good, rebuild screens and re-subscribe without a reboot; on failure return the error and change nothing. Reply `{"ok":true,"restart_required":false}` or `{"ok":false,"error":"..."}`.
- `POST /api/config/validate` — validate only. `GET /api/config/lkg` — the last-known-good config.
- `DELETE /api/config` — forget saved configs and run the built-in one.
- `GET /api/status` — IP, uptime, heap, config source, Signal K server and last-message age, visible screen, and each subscribed value with its age and metadata. `POST /api/screen?index=N` — show a screen (counts as a swipe for `auto_screen`). `POST /api/restart`. `POST /api/wifi/setup` turns the setup hotspot on (§5.6). `GET /api/screenshot` returns what the screen shows (or the Settings panel or setup screen, if one is open) as a 16-bit BMP, which `tools/telltale.py screenshot` saves as PNG. It's for docs and bug reports, and the README's screenshots come from it.
- Auth: changes (`PUT`, `DELETE`, `/ota`, `/api/restart`, `/api/wifi/setup`, the setup page from the boat network) need the device password (§5.6, header `X-OTA-Password`, checked in constant time). Reads don't. Plain HTTP: boat LAN only.
- Boot: `config.json`; if invalid, `config.lkg.json`; then the built-in config; if all fail, an error screen with the device IP. A fallback shows as an amber line at the bottom of the screen.
- Crash-loop guard: 2 crashes in a row (panic or watchdog, counted in RTC memory, cleared after 60 s up) skip `config.json`; 4 skip `config.lkg.json` too; 6 leave the screens off so WiFi and OTA keep working.
- Tested on the desk (2026-10-28): eight kinds of bad config rejected with a clear message; good config applied live; wrong password → 401; saved config survives restart and USB flashing; 24 PUTs in a row with no heap loss.

### 6.5 Derived values: in Signal K, not here

Decided 2026-10-28 (owner): values computed from other values (net power, totals across charge controllers, rates) are made on the Signal K server and published as ordinary paths. Telltale binds to them like anything else; it has no `derived` config.

Why: one number for every consumer (Telltale, Grafana/InfluxDB history, phone apps, a second display) instead of a formula copied into each display's config; a server path can carry zones and raise notifications, which Phase 5 alarms need; it gets logged; and it keeps Telltale a display, the same split as BLE decoding staying in server plugins (Backlog). Local derived values were planned here (`sum`, `diff`, `min`, `max`, `scale`) and dropped before being built.

How, on the server (owner's step, when a screen needs one): a server plugin. Candidates, **not checked** for what they compute or whether they run on Signal K 2.26.0: signalk-derived-data (standard calculations) and Node-RED (custom formulas). Check before relying on either. Often no derived value is needed: the SmartShunt already reports `electrical.batteries.shunt.power`, the net flow into or out of the battery from all sources.

### 6.6 Auto screen (built)

```json
"auto_screen": {
  "path": "navigation.state",
  "screens": { "anchored": "anchor", "moored": "health", "sailing": "sail", "motoring": "engine" },
  "manual_hold_s": 300
}
```

- Shows the mapped screen when the value changes (and on the first value after boot or reconnect). Values not in the map leave the screen alone.
- A swipe (or `POST /api/screen`) wins: no automatic switch for `manual_hold_s`, and after that only when the value changes again.
- The path is subscribed in the background, whatever screen is showing.
- Any text path works. Tested on the desk with `electrical.solar.dodger_solar.chargingMode` standing in for `navigation.state`.
- `navigation.state` is a Signal K path; the [signalk-autostate](https://github.com/meri-imperiumi/signalk-autostate) plugin sets it (README read 2026-10-28): `anchored` when `navigation.anchor.position` is set; `moored` from little movement or zero speed with the engine stopped; `sailing` / `motoring` when moving, from `propulsion.*.state` / `propulsion.*.revolutions`, with a configured default. Not installed on the test server (the path doesn't exist there yet). Without engine data (e.g. HALMET) it can't tell sailing from motoring.
- Phase 5's context-aware announcing (night / underway / aboard) should read the same state rather than guess its own.

## 7. Phases

### Phase 1 — Board bring-up

Display, touch, backlight, LVGL on the LCD-7 profile; WiFi; OTA; logging. Start from Waveshare's LCD-7 demo for panel timing. Acceptance: stable UI with WiFi active for 24 h, no drift or flicker (wiki §3.1 notes). **Status:** built; WiFi interference fixed (§5.5); 24 h soak not run.

### Phase 2 — Config and screens

Config loader, schema validation, last-known-good, screen builder, widget registry with `value` and `label`; config API. Acceptance: change a screen by PUTting JSON, no rebuild, no reboot; bad JSON is rejected and the display keeps running. **Status:** built and accepted on the desk 2026-10-28 (§6.4).

### Phase 3 — Signal K client

Connect, per-screen subscribe, metadata, staleness, reconnect. Acceptance: values update; pulling the server shows stale state within `stale_after_s`; reconnect restores values without user action. **Status:** built 2026-10-28. Seen working: per-screen subscribe (12 paths on screen 1, 6 on screen 2), metadata units/decimals, server-timestamp ages, reconnect after the device restarts. The "pull the server" test isn't done yet: it needs the relay stopped, which is the owner's window.

### Phase 4 — First widget set and the first boat's screens

`ring`, `bar`, `state`, `alert` with notifications, `tide`, `anchor` (built 2026-10-28; zones were done before, §6.2). Derived values moved to the Signal K server (§6.5). The first boat's config: [`examples/sailboat.json`](../examples/sailboat.json) (2026-10-28) — five screens: **health** (battery ring, battery power and voltage, time to go, fridge, cabin, outside, cabin humidity, sea temp, all alerts), **power** (battery ring, dodger solar bar 0–400 W, dodger charger `state` mapped from the plugin's `chargingMode` names, battery current and power, solar today, electrical alerts), **sail** (apparent wind and angle, true wind over ground and direction, heading, SOG, COG, STW, depth), **anchor** (anchor view with circle, swing trail and alarm; tide curve for the next 18 h; transducer depth; wind) and **weather** (cabin barometer, outside temperature and humidity, true wind, sea temp); `auto_screen` on `navigation.state` (moored → health, anchored → anchor, sailing and motoring → sail until the engine screen exists). Ranges only where the boat's documents give them (dodger panels ~400 W); no zones or other thresholds yet — the owner's to choose. `chargingMode` values are the victron_ble `OperationMode` names lower-cased (signalk-victron-ble `plugin.py`, main branch, read 2026-10-28). Units given explicitly: Signal K sends some paths (battery power, yieldToday, pressure) without display units. Not yet: engine screen (HALMET), anchor view and AIS (Backlog), forecast (a weather provider plugin on the server), morning summary (history).

The first boat's screens, by boat state (`auto_screen` on `navigation.state`, §6.6). These are from the use-case list (owner liked them, 2026-10-28) plus the owner's ask for a display that knows whether the boat is motoring, sailing, anchored or at the dock:

| State | Screen | Shows | Needs |
|---|---|---|---|
| `moored` (dock) | Boat health | SoC + net power, shore power / charger on, fridge, bilge cycles 24 h, cabin and outside temp; each colours on its own zone | Bilge monitor for bilge cycles |
| `anchored` | Anchor / night | Dimmed red: wind, depth, drift from the anchor, battery; dark until something changes, then the alert full-screen | Night mode (Backlog); anchor position/drift — hoekens-anchor-alarm is on the test server, but which paths it writes isn't checked (none under `navigation.anchor` while not anchored, 2026-10-28) |
| `anchored`, also | Power at anchor | Solar per MPPT, battery current, SoC, time to go, today's harvest vs the energy budget — "can we run the heater/watermaker tonight?" | Missing MPPT/MultiPlus data (§8 Q9) |
| `sailing` | Sail | Wind (apparent/true), SOG, COG, heading, depth | Compass/wind widgets (Backlog) for the nice version |
| `motoring` | Engine | RPM, coolant, oil pressure, alternator output via B2B, fuel burn | HALMET engine data |
| any | Morning summary | Overnight lows inside/out, Ah used overnight, wind trend, bilge cycles | History since boot (sparkline) |
| — | Returning to the boat | "Since you left": lowest SoC, highest fridge temp, bilge runs, shore power drops | History beyond the device's boot: InfluxDB, outside "Signal K only" — later |

For it to work on the first boat: install signalk-autostate on its server (owner's step), engine data from HALMET for `motoring`, and Phase 4's widgets.

### Phase 5 — Alarm center

Telltale shows Signal K alarms, lets you acknowledge them, and decides how loudly to announce them. It is an annunciator, never the only alarm for anything life-safety: it depends on the Pi, WiFi and Signal K. A high-water alarm, for one, should stay standalone.

- **Alarm list and banner** from `notifications.*`: message, severity colour, source path, age. Tap to acknowledge or silence.
- **Write access:** Signal K ≥ 2.21 has `POST …/notifications/:id/acknowledge`, `…/:id/silence`, `acknowledgeAll`, `silenceAll` (checked in signalk-server source); the test server's notifications already carry `status.{silenced, acknowledged, canSilence, canAcknowledge}`. Its anonymous access is read-only, so this needs a device token with write access (device access request flow). **(verify)** the exact URL prefix and the permission those endpoints require.
- **Signal K lost is an alarm:** if the stream drops (or every subscribed value goes stale) for longer than a config time, raise a local alarm. A dead chain must not look like a quiet boat.
- **Context-aware announcing** (all in config): per severity × context → outputs.

  | Context | Detected from | Example |
  |---|---|---|
  | Night | Sunrise/sunset from `navigation.position` + `navigation.datetime` | Screen wakes, flashes red; quiet chirp; escalate if not acknowledged |
  | Underway | `navigation.speedOverGround` above a threshold; engine RPM once HALMET is in | Cockpit output; cabin sound is useless over the engine |
  | Day, aboard | Neither | Banner + normal sound |
  | Manual override | Tap: Sleeping / Underway / Auto | When detection guesses wrong |

- **Escalation ladder:** start gentle; no acknowledgement in N minutes → louder / more outputs; top severities go straight to everything.
- **Outputs:** the board has no buzzer or audio amp and almost no free GPIO. Use an **RS485 relay module** on the RS485 terminal (GPIO15/16): long cable runs, relays wherever the sounders are. Planned channels on the first boat: (1) quiet piezo near the display for night; (2) dry contact onto the high-water alarm's line (a horn/strobe box with fail-safe timed mute) — wired **in parallel** with the high-water float, closed only for `emergency` severity and held closed. Which alarm it is shows on Telltale's screen. **(verify)** the RS485 relay module's protocol (likely Modbus RTU) once one is chosen.
- **Pi speaker:** the test server has `signalk-notification-player` (`digital.notificationPlayer.{silence,resolve,disable}`); Telltale could mute it at night. **(verify)** that those paths accept writes and what they do.
- **First worked example — bilge** (N2k.BilgeMonitor, not installed): normal pump runs → screen only, no sound; `notifications.environment.bilge.main.currentRunDuration` (long run) → alarm; runs-per-hour → alarm only if it is raised as a Signal K notification (a Grafana-only alert won't reach Telltale); bilge data stale → "bilge monitor offline" alarm.

### Backlog

Graph/sparkline, night mode (red, dimmed), compass/wind, config pull from Signal K, more board profiles, `telltale_editor`. A visual design pass (layout, type sizes, colours, value placement) once the first boat's screens exist — owner suggests a session with a design agent (2026-10-28). Current layout: small caption top left, reading centred below it; ring and bar tracks a lighter blue than the card. (Screen switching by boat state is built: §6.6.)

**BLE gateway mode** (optional, switched on in config): forward raw BLE advertisements to Signal K's BLE API, decoding stays in the server plugins — the role a separate BLE gateway (e.g. an AtomS3) plays. Needs Signal K 2.31+ (the test server is 2.26.0). WiFi and BLE share one radio; check the WiFi impact when built.

## 8. Open questions

6. Grid size default and minimum widget size for readability at 800×480.
7. 12 V → 5 V supply and mounting location.
9. Missing from Signal K on 2026-10-27: outside RuuviTag, MultiPlus, B2B, arch solar MPPT. SmartShunt and fridge tag updates gap for 7–16 min at a time (BLE reception).
10. Page schema: consider adopting Navura's (`plotter_app`) page/widget JSON (`PageModel`, `WidgetPlacement`, `WidgetConfig`) so Navura's page editor can design Telltale screens. Schema only — Navura is proprietary, no code shared.

### Resolved

5. Where the first boat's screen config lives → `examples/sailboat.json` here, so the host tests check every example against the firmware's rules on each run (2026-10-28). The boat's own docs link to it. The built-in `data/default_config.json` is a generic fallback (2026-10-28: standard paths only, for any boat).
1. Framework → ESP-IDF 5.5.0 via PlatformIO (§2).
11. Signal K stream behaviour on 2.26.0 → `sendMeta=all`, unsubscribe-all and replay-on-subscribe all work (§5.2, checked 2026-10-28).
2. LVGL → 8.4.0 (Waveshare demo requires >8.3.9, <9).
3. The first boat's paths (2026-10-27): `electrical.batteries.shunt.{voltage,current,power}`, `electrical.batteries.shunt.capacity.{stateOfCharge,timeRemaining,dischargeSinceFull}`, `electrical.solar.dodger_solar.{panelPower,current,voltage,chargingMode,yieldToday}`, `environment.{inside,fridge}.{temperature,relativeHumidity,pressure}`, `environment.water.temperature`, `environment.depth.belowKeel`, `environment.wind.{speedApparent,angleApparent,directionTrue}`, `navigation.{headingTrue,speedOverGround,courseOverGroundTrue}`.
4. Signal K security → on, read-only access allowed (§5.2).
8. Board variant → touch (§3.1).
