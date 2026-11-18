# Building & Installation Guide

This guide covers building the ARM64 Debian package for **CardputerZero**, installing the background receiver on **macOS**, running the desktop **SDL2 UI simulator**, and running all test suites.

---

## 1. Prerequisites

### macOS Host
- **Python:** `3.9+` (macOS system/Homebrew Python 3; both `mac-receiver` and `device-daemon` use the Python standard library with zero third-party `pip` dependencies).
- **Tailscale:** Signed into the same private tailnet as your CardputerZero.
- **Docker Desktop (for building the CardputerZero `.deb` on Mac):** Apple Silicon or `linux/arm64` binfmt/emulation support enabled.
- **Homebrew packages (only needed for building/running the macOS SDL2 desktop UI simulator & C++ tests):**
  ```bash
  brew install cmake pkg-config sdl2 freetype sqlite3
  ```

### CardputerZero Device
- M5Stack CardputerZero running Raspberry Pi CM0 **ARM64 Debian Bookworm** (or compatible `arm64` Debian) with `systemd --user`, `APPLaunch`, and `tailscale` connected to your tailnet.
- Runtime packages declared by `packaging/debian/control`: `python3 (>= 3.9)`, `libsqlite3-0`, `libfreetype6`.
- Native build packages (only required if compiling `.deb` directly on the CardputerZero instead of via Docker):
  ```bash
  sudo apt-get update
  sudo apt-get install -y build-essential cmake pkg-config libsqlite3-dev libfreetype-dev dpkg-dev python3
  ```

---

## 2. Optional: Bundling TrueType Fonts (`Inter` + `JetBrains Mono`)

At runtime, `cp0-sync-ui` searches for Sans and Mono fonts in this order (`device-ui/src/app_ui.cpp`):
1. `CP0_FONT_SANS` / `CP0_FONT_MONO` environment variable overrides
2. `/usr/share/cp0-sync/fonts/` (`Inter-Regular.ttf` / `Inter.ttf` and `JetBrainsMono-Regular.ttf` / `JetBrainsMono.ttf`)
3. `assets/fonts/` relative to working directory
4. System Debian (`DejaVuSans.ttf`, `LiberationSans-Regular.ttf`, `/usr/share/fonts/truetype/inter/...`) or macOS (`/System/Library/Fonts/SFNS.ttf`, `Menlo.ttc`, etc.)
5. Built-in crisp 5×7 pixel bitmap font (`kFont5x7`) if no `.ttf` / `.ttc` font is found

If you want `Inter` and `JetBrains Mono` bundled inside the `.deb` at `/usr/share/cp0-sync/fonts/`, place `.ttf` or `.otf` font files in `packaging/fonts/` (or `device-ui/assets/fonts/`) **before** running `packaging/build-deb.sh`:

```text
packaging/fonts/
├── Inter-Regular.ttf
└── JetBrainsMono-Regular.ttf
```

> Note: When building the CardputerZero target inside a clean container or minimal Bookworm image without bundled fonts, installing `fonts-dejavu-core` on the device also activates FreeType anti-aliased rendering automatically without needing custom files in `packaging/fonts/`.

---

## 3. Building the CardputerZero ARM64 `.deb`

### Method A: Cross-Build on macOS via Docker (Recommended)

`packaging/build-deb.sh` uses [`packaging/Dockerfile.arm64`](../packaging/Dockerfile.arm64) (`--platform linux/arm64`, `debian:bookworm-slim`) to compile `device-ui` with `-DCP0_UI_TARGET_DEVICE=ON` (Linux `fbdev` + `evdev`), run C++ unit tests inside the container, and package `cp0-sync_1.0.0_m5stack1_arm64.deb`:

```bash
./packaging/build-deb.sh
```

Output artifact:
```text
packaging/dist/cp0-sync_1.0.0_m5stack1_arm64.deb
```

`(packaging/dist/ and device-ui/build-arm64/ are gitignored.)`

#### `build-deb.sh` Options
| Flag | Description |
|---|---|
| *(none)* | Build inside `--platform linux/arm64` Docker container and copy `.deb` to `packaging/dist/` |
| `--local` | Compile `device-ui` (`CP0_UI_TARGET_DEVICE=ON`) and build `.deb` directly on a Linux ARM64 host |
| `--stage-only <dir>` | Stage package file tree without calling `dpkg-deb` (requires existing binary or `--ui-bin`) |
| `--ui-bin <path>` | Use a prebuilt `cp0-sync-ui` binary instead of recompiling |
| `--out-dir <dir>` | Write `.deb` into a custom directory instead of `packaging/dist/` |

### Method B: Build Directly on CardputerZero (or ARM64 Linux Host)

```bash
git clone <your-repo-url> cardputerzero-sync
cd cardputerzero-sync
sudo apt-get update && sudo apt-get install -y build-essential cmake pkg-config libsqlite3-dev libfreetype-dev dpkg-dev python3
./packaging/build-deb.sh --local
```

### Package Contents (`cp0-sync_1.0.0_m5stack1_arm64.deb`)
- `/usr/bin/cp0-sync-ui` — 320×170 companion UI (`fbdev` + `evdev` binary)
- `/usr/bin/cp0-syncd` — Background Python sync daemon CLI entrypoint
- `/usr/lib/cp0-sync/cp0_syncd/*.py` — Sync daemon Python modules
- `/usr/lib/systemd/user/cp0-syncd.service` — `systemd --user` service unit
- `/usr/share/APPLaunch/applications/cp0-sync.desktop` — `Photo Sync` app launcher entry
- `/usr/share/APPLaunch/share/images/cp0-sync.png` — `80×80` app icon
- `/usr/share/cp0-sync/fonts/*` — Optional bundled fonts (if provided at build time)

---

## 4. Installing & Uninstalling on CardputerZero

Copy the `.deb` over Tailscale and install:

```bash
scp packaging/dist/cp0-sync_1.0.0_m5stack1_arm64.deb pi@<cardputerzero-host>:/tmp/
ssh pi@<cardputerzero-host> "sudo dpkg -i /tmp/cp0-sync_1.0.0_m5stack1_arm64.deb"
```

If `dpkg` reports missing shared runtime dependencies (`libfreetype6` or `libsqlite3-0`), resolve them with:

```bash
sudo apt-get install -f -y
```

### What `postinst` / `prerm` Do
- **`postinst` (`configure`):** Runs `systemctl --global enable cp0-syncd.service || true` and best-effort restarts `cp0-syncd.service` for active logged-in user sessions via `loginctl`.
- **`prerm` (`remove` / `deconfigure`):** Stops active user instances, runs `systemctl --global disable cp0-syncd.service || true`, and cleans Python `__pycache__` files under `/usr/lib/cp0-sync` without deleting your `~/.config/cp0-sync/` or `~/.local/share/cp0-sync/` data.

Verify the background service is active under your user session:

```bash
systemctl --user status cp0-syncd.service
```

To uninstall later:

```bash
sudo dpkg -r cp0-sync
```

---

## 5. Installing the Mac Receiver

On your Mac, run the `LaunchAgent` installer from the repository:

```bash
./mac-receiver/bin/cp0-mac-sync install-service
./mac-receiver/bin/cp0-mac-sync status
```

This creates and loads `~/Library/LaunchAgents/com.cardputerzero.photosync.plist` (`RunAtLoad=true`, `KeepAlive=true`), starting `mac-receiver/bin/cp0-mac-receiver` automatically at login.

*(Optional)* Symlink the CLI scripts into your `PATH` (both entrypoints resolve the `mac-receiver/` package cleanly through symlinks):

```bash
mkdir -p ~/.local/bin
ln -sf "$(pwd)/mac-receiver/bin/cp0-mac-sync" ~/.local/bin/cp0-mac-sync
ln -sf "$(pwd)/mac-receiver/bin/cp0-mac-receiver" ~/.local/bin/cp0-mac-receiver
```

To stop and remove the `LaunchAgent`:

```bash
./mac-receiver/bin/cp0-mac-sync uninstall-service
```

---

## 6. Building & Running the Desktop Simulator (`device-ui`)

You can run and visually inspect the 320×170 UI natively on macOS using SDL2 without CardputerZero hardware:

```bash
cmake -S device-ui -B device-ui/build -DCP0_UI_BUILD_TESTS=ON -DCP0_UI_BUILD_APP=ON
cmake --build device-ui/build
```

Run a scaled `3×` simulator window (connected to `~/.config/cp0-sync/` or offline fallback):

```bash
./device-ui/build/cp0-sync-ui --sim --scale 3
```

Render a headless `320×170` PPM snapshot without opening a window:

```bash
./device-ui/build/cp0-sync-ui --render-test /tmp/status_dark.ppm --theme dark --tab status
./device-ui/build/cp0-sync-ui --render-test /tmp/folders_light.ppm --theme light --tab folders
```

### CMake Options Reference (`device-ui/CMakeLists.txt`)
| Option | Default | Notes |
|---|---|---|
| `CP0_UI_BUILD_TESTS` | `ON` | Builds `test_ipc_and_theme` and `test_ui_navigation` |
| `CP0_UI_BUILD_APP` | `ON` | Builds the `cp0-sync-ui` executable (set `OFF` to build only `cp0_ui_core` + tests) |
| `CP0_UI_TARGET_DEVICE` | `OFF` | Linux only: targets `/dev/fb0` + `/dev/input/event*` instead of SDL2 (errors cleanly on non-Linux hosts) |
| *(Optional LVGL)* | auto-detected | If system `lvgl` (`find_package(lvgl)` / `pkg-config lvgl`) is present, enables `CP0_HAS_LVGL=1` style hooks alongside the built-in `320×170` renderer; never downloads over network via `FetchContent` |

---

## 7. Running All Test Suites

All test suites use zero third-party Python dependencies (`unittest` stdlib) and CTest:

```bash
# 1. Mac Receiver unit & HTTP server tests (24 tests)
PYTHONPATH=mac-receiver python3 -m unittest discover -s mac-receiver/tests -v

# 2. Device Daemon config, SQLite store, watcher, uploader & IPC tests (25 tests)
PYTHONPATH=device-daemon:mac-receiver python3 -W error -m unittest discover -s device-daemon/tests -v

# 3. Companion UI C++ tests (2 CTest targets: test_ipc_and_theme, test_ui_navigation)
cmake --build device-ui/build && ctest --test-dir device-ui/build --output-on-failure -V

# 4. Packaging validation tests (7 tests)
python3 -m unittest discover -s packaging/tests -v
```

*(Note on macOS sandboxing: `test_server.py`, `test_daemon_integration.py`, and `test_ipc_and_theme` open local TCP loopback or `AF_UNIX` sockets; if running inside a restricted sandbox that blocks sockets with `Operation not permitted` / `EPERM`, run them outside the sandbox.)*
