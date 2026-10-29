# For AI agents

Read this before changing anything. Then read [CONTRIBUTING.md](CONTRIBUTING.md) and, for a new board, [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md).

## Which job is it?

- **Changing what a display shows** (screens, paths, labels, units, thresholds): that's a config file, not a code change and not a PR. See the README, *Changing screens*. Don't edit the firmware, the built-in config or `examples/` for one boat.
- **Supporting a new board:** follow [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md) exactly. It needs the real board.
- **Anything else** (a widget, a fix): [CONTRIBUTING.md](CONTRIBUTING.md). Open an issue first for anything large.

## Rules

- **Never guess.** Pins, timings, part numbers, Signal K paths and API behaviour come from a source you cite (datasheet, schematic, vendor demo, the Signal K spec, the running server) or from the device. If you can't find it, say so; mark it **(verify)** in the SPEC.
- **Claim only what you ran.** Say what you tested on hardware and what you didn't. Don't write "tested" for code that was only built.
- **Small and on topic.** One change per PR. No reformatting, renames, refactors or new dependencies the change doesn't need. Match the surrounding code and comment style.
- **Tests with behaviour.** Logic that can run on a PC gets host tests in `test/host/`.
- **No boat in the code.** Signal K paths, labels, thresholds and units come from config or Signal K metadata, never from code (SPEC §4). The built-in config uses only standard paths from the Signal K specification.
- **Conventional Commits** for the PR title: `feat:`, `fix:`, `docs:`, `test:`, `ci:`, `build:`, `refactor:`, `perf:`, `chore:`. PRs are squash-merged, so the title becomes the commit.
- **No secrets.** Never commit `src/secrets.h`, WiFi passwords or device passwords.
- **Licenses.** Copy code only from sources whose license allows it in an MIT project, keep their headers, and say where it came from.
