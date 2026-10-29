# Contributing

Contributions are welcome, from people and from AI agents working for them. AI agents: read [AGENTS.md](AGENTS.md) first.

## Your board isn't supported?

See [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md): what a board needs, the facts to collect with sources, what the PR contains, and the checks to run on the real board.

## Your Signal K paths are different?

That's expected, and it never needs a code change. Every boat names some things its own way (a battery might be `electrical.batteries.house` on one boat and `electrical.batteries.shunt` on another), so screens bind paths in the config, not in the firmware.

- **Find your paths:** your Signal K server lists every path with its current value at `http://<server>/signalk/v1/api/vessels/self`, and its Data Browser shows them live.
- **Use them:** write them into your own config and send it (README, *Changing screens*). An AI assistant can do this from the list above.
- **Units and decimals** come from the server's metadata (set them in Signal K's display units) unless your config overrides them.
- **Values your boat doesn't have** (net power, totals, rates) are made on the Signal K server by a plugin, not on the display (SPEC §6.5).
- **Don't send a PR** to add your boat's paths to the firmware, the built-in config or `examples/`. The built-in config uses only standard paths from the Signal K specification, so it works on any boat.

If a path is standard but Telltale shows it wrong (units, staleness, a widget that can't display that kind of value), that's a bug: open an issue with the path, what the server sends (`telltale.py <ip> status`) and a screenshot.

## Pull requests

- Open an issue first for anything bigger than a fix.
- One topic per PR, with a [Conventional Commits](https://www.conventionalcommits.org/) title (`feat: …`, `fix: …`, `docs: …`). PRs are squash-merged; the title becomes the commit.
- CI builds the firmware and runs the host tests (`sh test/host/run.sh`); it must pass. New behaviour comes with host tests where it can.
- Say what you tested on a real device and what you didn't.
- Follow the design rules in [docs/SPEC.md](docs/SPEC.md) §4.
