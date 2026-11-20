# Development

## Layout

| Folder | What is there |
| --- | --- |
| `firmware/` | The board's firmware (PlatformIO, Arduino, LVGL) and its simulator |
| `firmware/src/pet/` | The pets and the office view |
| `firmware/sim/` | Scenarios for the simulator |
| `firmware/test/` | Host unit tests |
| `desktop/` | The app (Rust, GPUI): four crates, `clawdbuddy-core`, `-ble`, `-flash`, `-app` |
| `tools/ci/`, `tools/packaging/` | What the release job runs |
| `docs/` | [The pet view](pet-view.md), [the office view](office-view.md), [the Bluetooth protocol](protocol.md) |

## Firmware

Needs [PlatformIO](https://platformio.org/install/cli).

```bash
pio run -d firmware -e waveshare_lcd_4              # build
pio run -d firmware -e waveshare_lcd_4 -t upload    # build and flash over USB
pio test -d firmware -e native                      # unit tests, on your computer
```

There are three environments: `waveshare_lcd_4` (the board), `sim` (the
simulator) and `native` (the tests).

## Simulator

Needs SDL2 (`brew install sdl2`, or `apt install libsdl2-dev`).

```bash
pio run -d firmware -e sim
cd firmware && SIM_SCENARIO=sim/team.jsonl .pio/build/sim/program
```

| Variable | Meaning |
| --- | --- |
| `SIM_SCENARIO` | A `.jsonl` file: one payload per line, each with `hold_ms` |
| `SIM_TAPS` | Scripted taps: `ms@x,y;ms@x,y` |
| `SIM_SHOTS` | Screenshots: `ms:path.bmp,ms:path.bmp` |
| `SIM_TRACE=1` | Print what the director decides |
| `SDL_VIDEODRIVER=dummy` | Run without a window |

The mouse is the touch screen; `m` raises an alert, `a` cycles activity,
`esc` quits. The pictures in the README were made this way.

## The app

Needs [Rust](https://rustup.rs); the version in `desktop/rust-toolchain.toml`
installs itself. On macOS it also needs Xcode with its Metal Toolchain
(`xcodebuild -downloadComponent MetalToolchain`).

```bash
cd desktop
cargo test --workspace
cargo run --release -p clawdbuddy-app
```

`tools/packaging/local-mac-app.sh` builds an ad-hoc signed
`desktop/target/dist/ClawdBuddy.app` for the Mac you are on.

## Releases

Pushing a tag like `v0.1.0` runs `.github/workflows/release.yml`, which
publishes a GitHub release with:

- the firmware image for the board and `manifest.json`, which the app's Flash
  page reads;
- the app for macOS (`.dmg`, universal), Windows (`.msi`, `.zip`) and Linux
  (`.deb`, AppImage, `.tar.gz`).

The app looks for releases in `tfranovic/ClawdBuddy` (`release_repo` in its
config). The tag is the version: the firmware and the app both report it.

## The app's log

The app writes `daemon.log` in its folder (`~/.config/clawdbuddy` on macOS
and Linux): what it found at start, the link to the board coming and going,
alerts, and every payload it wrote, labelled by why (`Sending` after a usage
poll, `Event` for a hook, `Attention`, `Heartbeat`).

- Lines are `[HH:MM:SS] message`. A `--- 2026-11-20 ---` line marks the start
  and each new day.
- A payload is logged without the theme's colours (`th`, `fc`) while they are
  the ones last logged.
- The previous run's log is kept as `daemon.log.1`. A log that reaches 4 MB
  is rolled there too, so the two never take more than 8 MB.
- `CLAWDBUDDY_LOG=debug` (or `error`, `warn`, `trace`, `off`) sets the level.
