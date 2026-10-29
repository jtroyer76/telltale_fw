# Adding a board

Telltale runs on one board today: the Waveshare ESP32-S3-Touch-LCD-7 (800×480, SPEC §3.1). If yours isn't supported, this is how to add it. It's written for people and for AI coding agents; agents should follow it to the letter.

A board PR is accepted when it's small, sourced and shown working on the real board. Untested or guessed hardware support costs more to review than it saves, and will be closed.

## Before you start

Your board is a good fit if it has:

- **ESP32-S3 with PSRAM.** The framebuffer, the code (run from PSRAM so flash writes don't stall an RGB panel) and the anchor trail live in PSRAM. The current board has 8 MB.
- **8 MB flash or more.** `partitions.csv` has two 3 MB app slots for updates with rollback; the firmware is about 1.7 MB.
- **A touch screen.** Swiping between screens and the Settings gear need touch.
- **A screen near 800×480.** Screens are laid out in grid units and readings pick the largest font that fits, but nothing else has been tried. A much smaller screen may need a smaller font set.

You need the board on your desk. Without one, open an issue instead of a PR.

## Find the facts first, with sources

Collect these from the vendor's schematic, wiki, datasheet or demo code, and note where each came from and when you read it:

- module, flash and PSRAM size and mode (check flash with `esptool flash_id`; vendor pages are sometimes wrong)
- display interface (RGB, SPI, QSPI, MIPI), driver IC, resolution, timings and pixel clock
- every pin: display, touch (controller, I2C pins, interrupt, reset), backlight, any IO expander
- anything shared or muxed (for example a pin shared between USB and CAN)

**Never guess a pin, timing or part number.** If a fact can't be found, say so in the PR and mark it **(verify)** in the SPEC. A wrong pin can damage hardware; a plausible guess hides that it was never checked.

## What the PR contains

One board per PR, and only these:

1. **`src/board/board_<id>.c`**: implements `src/board/board.h` (`board_init`, `board_backlight`) for your board. Base it on `board_lcd7.c`. If you adapt vendor demo code, its license must allow use in an MIT project; keep its headers and say where it came from.
2. **Board selection.** Today `board.h` is fixed to the one board. The first PR for a second board adds a build-time choice, one PlatformIO env per board, with its own sdkconfig, and only that board's file compiled in. Describe the mechanism in the PR. `lcd7` must build and behave exactly as before.
3. **`platformio.ini`**: `[env:<id>]` and `[env:<id>-ota]`, like the `lcd7` pair.
4. **CI**: build the new env in `.github/workflows/ci.yml`. Leave `release.yml` and the web flasher alone; the owner extends releases to new boards.
5. **SPEC**: a §3 section for the board, like §3.1: a facts table with sources and dates, and bring-up notes (what went wrong and how it was fixed). Add it to the README's Hardware section.

Not in a board PR: changes to widgets, the config format, the Signal K client, the built-in config or the existing board, reformatting, renames, refactors or new dependencies that the board doesn't need. If the board really needs a shared change, keep it minimal, explain why in the PR, and show `lcd7` still passes.

## Show it working

Run each of these on the real board and put the results in the PR. `tools/telltale.py` and the device API do the work; `TELLTALE_OTA_PASS` is the device password from Settings.

| Check | How | Passes when |
|---|---|---|
| Builds | `pio run -e <id>` and `sh test/host/run.sh` | No errors, no new warnings |
| Boots | serial log of the first boot | No resets or errors |
| Setup | erase flash, flash, follow the QR codes from a phone | Joins your WiFi; both QR codes scan |
| Signal K | `python tools/telltale.py <ip> status` | `signalk.connected` is true, values arriving |
| Screens | `python tools/telltale.py <ip> screenshot nav.png 0` (and each screen) | Text fits its cards; attach the PNGs |
| Touch | swipe between screens; tap the gear | Screen changes; Settings opens (attach a screenshot of it) |
| WiFi | leave it running 30 min, then `status` | `wifi_drops` is 0. RGB panels can radiate into the antenna: see SPEC §5.5 |
| Memory | `status` after 30 min, and with the setup hotspot on and a phone joined | Report `free_internal`, `free_dma`, `largest_dma`, `min_free_dma` and `free_psram`. WiFi needs DMA-capable internal RAM; on `lcd7` about 58 KB is free, and the hotspot with a phone joined takes 12–15 KB more |
| Update | `pio run -e <id>-ota -t upload --upload-port <ip>` | Restarts into the new build; `GET /api/info` shows it. The screen must not flicker while it writes |

Say plainly what you didn't test, and why.

## Known traps (from the first board)

- **Flash writes stall an RGB panel** whose framebuffer is in PSRAM: the screen flashes during updates. Fixed with `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` (SPEC §3.1).
- **The pixel clock radiates into WiFi.** At default drive strength WiFi lost 10–60 % of pings; the weakest drive on the PCLK pin fixed it (SPEC §5.5, `board_lcd7.c`).
- **GT911 on the legacy I2C driver** rejects the `scl_speed_hz` its config macro sets; clear it (`board_lcd7.c`).
- Flash and PSRAM clock settings must match on the S3 (Waveshare's notes for the first board).
