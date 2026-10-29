# telltale_fw

Firmware for a Signal K touch display with screens from a JSON config. Start with [README.md](README.md); the design and its history are in [docs/SPEC.md](docs/SPEC.md).

## Rules

- **Pull requests only.** Branch from `main`, one topic per PR. CI (`.github/workflows/ci.yml`) builds the firmware and runs the host tests, and must pass. The assistant opens PRs and keeps them passing; the owner reviews and merges. Never approve or merge a PR, never push to `main`.
- **Tests with behaviour.** A change that adds or fixes behaviour adds host tests (`test/host/`) where it can.
- **Standalone product.** This repo stands on its own: don't reference the owner's other repos or projects.
- **Stay general.** This is a public product, not one boat's project: no boat names, server hostnames or boat-specific details in docs, examples, test data or screenshots. "Tested on a boat" is fine.
- **No AI attribution** in commits or PRs (no Co-Authored-By lines).
- **Never guess** specs, part numbers, API behaviour or other facts; check the source, a datasheet or the device, or say it's unknown. Mark unchecked claims in the SPEC **(verify)**.
- **Search the whole repo** for related references when changing something (README, SPEC, schema, examples, tests).
- **Knowledge goes in the repo,** not the assistant's local memory: the owner works on several machines. Put lasting rules here and design facts in the SPEC.
- **No secrets in git.** `src/secrets.h` is gitignored and optional; the device password comes from `TELLTALE_OTA_PASS`. Don't print it.
- Ideas from other projects (KIP, Marine-Displays) only, no code copied: their licenses don't allow it.

## Working notes

- Build: `pio run -e lcd7`. A full rebuild with parallel jobs can crash the compiler on `esp_lcd_panel_rgb.c`; `pio run -e lcd7 -j 1` gets through. Incremental builds are fine.
- Host tests: `sh test/host/run.sh` (needs PlatformIO's ESP-IDF installed for cJSON).
- On the device: `tools/telltale.py` (config, status, screenshot) and `pio run -e lcd7-ota -t upload` (update over WiFi). Updates fail while the setup hotspot is on.
