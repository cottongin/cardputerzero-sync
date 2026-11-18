# CardputerZero Photo Sync — Implementation Summary

**Date:** 2026-11-18 09:15 EST
**Branch:** `feat/cardputerzero-photo-sync`
**Range:** `23db23a` (design spec & plan) → `6685844` (final fix wave)

---

## Task Description

Implemented an end-to-end, zero-external-Python-dependency one-way photo syncing system for the **M5Stack CardputerZero** (Raspberry Pi CM0, `arm64` Debian Linux, 320×170 non-touch LCD, `cp0-keys` 46-key matrix keyboard, `APPLaunch`) and **macOS** over an existing private **Tailscale** tailnet:

1. **`mac-receiver/`**: Background macOS HTTP receiver daemon (`MacReceiverServer`) + `cp0-mac-sync` CLI + `LaunchAgent` installer saving uploaded photos to `~/Pictures/CardputerZero/<folder_label>/<rel_path>` with atomic `.part` staging, SHA-256 verification, non-overwriting collision naming, and debounced macOS Notification Center summaries.
2. **`device-daemon/`**: On-device Python `cp0-syncd` (`systemd --user`) service with SQLite WAL queue (`SyncStore`, keyed on `(folder_id, rel_path, sha256)`), Linux `ctypes` `inotify` + fallback polling watcher (`INotifyWatcher`) supporting removable SD mounts via ancestor watches, sleep-aware wake-probe uploader (`Uploader`), and `AF_UNIX` JSON IPC server (`IpcServer`).
3. **`device-ui/`**: C++17 `320×170` companion UI (`cp0-sync-ui`) styled with `shadcn/ui` Zinc Dark & balanced Zinc Light themes (`[T]` live toggle), segmented `1 Status · 2 Folders · 3 Logs` tabs, Tailscale peer & custom `host[:port]` target picker (`[C]`), directory tree drill-down (`[A]`), non-destructive folder removal confirmation (`[D]`), retry trigger (`[R]`), FreeType2 + 5×7 fallback font rendering, macOS SDL2 simulator, Linux `fbdev`+`evdev` backend, and optional LVGL v9 hooks (`CP0_HAS_LVGL`).
4. **`packaging/`**: Debian `arm64` `.deb` builder (`build-deb.sh`, `Dockerfile.arm64`), `cp0-syncd.service` (`systemd --user`), `APPLaunch` `cp0-sync.desktop` & `80×80` icon (`cp0-sync.png`), Debian maintainer scripts (`control`, `postinst`, `prerm`), and packaging verification tests.

---

## Key Changes Made

| Commit | Summary |
|---|---|
| `23db23a` | Design specification (`docs/superpowers/specs/2026-11-18-cardputerzero-photo-sync-design.md`) and 7-task TDD plan |
| `555919a`, `23ed06f` | `mac-receiver/cp0_mac_receiver/{__init__,config,server}.py` & `tests/test_server.py` (Tailscale/loopback IP allowlist, atomic `.part` upload, `osascript` `on run argv` escaping) |
| `a5f8c5d` | `mac-receiver/cp0_mac_receiver/cli.py`, `bin/{cp0-mac-receiver,cp0-mac-sync}`, `tests/test_cli.py` (`set-dir`, `set-port`, `install-service`, `uninstall-service`, `status`, `logs`) |
| `69224f3` | `device-daemon/cp0_syncd/{__init__,config,store}.py` & `tests/test_store_and_config.py` (overlap guard, unique label disambiguation, SQLite WAL state/dedup/log store) |
| `da4d963`, `c6a672f` | `device-daemon/cp0_syncd/{watcher,uploader,ipc,daemon}.py`, `bin/cp0-syncd`, `tests/test_daemon_integration.py` (Linux `ctypes` `inotify` + fallback poller, crash `UPLOADING` recovery, atomic `_state_lock` wake/probe handling, uniform subdir prefix ignore, `AF_UNIX` JSON IPC) |
| `b6fcf5a`, `8ea46a2` | `device-ui/CMakeLists.txt`, `src/{theme,ipc_client}.{hpp,cpp}`, `tests/test_ipc_and_theme.cpp` (non-blocking IPC client, fail-closed corrupt SQLite fallback, Tailscale peer parser, Zinc Dark/Light palettes) |
| `7d369b2`, `eeacf0e` | `device-ui/src/{ui_controller,app_ui}.{hpp,cpp}`, `src/fb_blit.hpp`, `src/main.cpp`, `tests/test_ui_navigation.cpp` (320×170 renderer, modals, `cp0-keys`/SDL2/`fbdev`+`evdev` loops, bounds/bitfield-checked framebuffer blit, `CP0_HAS_LVGL` lifecycle, cross-tab feedback toasts) |
| `ec8a414` | `packaging/{Dockerfile.arm64,build-deb.sh,cp0-syncd.service,cp0-sync.desktop,cp0-sync.png}`, `packaging/debian/{control,postinst,prerm}`, `packaging/tests/test_packaging.py` |
| `6685844` | Final cross-component fixes: gate SQLite fallback folder synthesis on `!config_loaded`, show top-bar `"Daemon Off"` badge when `from_fallback` is true, and add `60s` socket timeout (`408`), `X-CP0-Mtime` validation / pre-replace `os.utime`, `.expanduser()`, and directory-collision `400`/`500` handling in `server.py` |

*(41 files added across implementation commits prior to this summary.)*

---

## Verification Results

Observed on `6685844`:
- **Mac Receiver (`PYTHONPATH=mac-receiver python3 -m unittest discover -s mac-receiver/tests -v`, `BypassSandbox: true`):** `24/24` passed in `6.319s` (`OK`).
- **Device Daemon (`PYTHONPATH=device-daemon:mac-receiver python3 -W error -m unittest discover -s device-daemon/tests -v`, `BypassSandbox: true`):** `25/25` passed in `4.766s` (`OK`, zero warnings).
- **Companion UI (`cmake --build device-ui/build && ctest --test-dir device-ui/build --output-on-failure -V`, `BypassSandbox: true`):** Clean build (`-Wall -Wextra -Wpedantic`, zero warnings) and `2/2` tests passed in `0.18 sec` (`test_ipc_and_theme` `0.16s`, `test_ui_navigation` `0.01s`).
- **Packaging (`python3 -m unittest discover -s packaging/tests -v`):** `7/7` passed in `0.134s` (`OK`).
- **ARM64 Container Verification (observed at `ec8a414` before final `mac-receiver` / `device-ui` `6685844` edits):** Built `cp0-sync_1.0.0_m5stack1_arm64.deb` (`98704` bytes) under `linux/arm64` `debian:bookworm-slim` (`CP0_UI_TARGET_DEVICE=ON` zero `g++` warnings, `ctest` `2/2`, `dpkg-deb -I/-c`, `dpkg -i`/`-r`, and `device-daemon/tests` `25/25` exercising real Linux kernel `ctypes` `inotify`).

---

## Lessons Learned

1. **Pre-empt Python/C++ schema & fallback drift:** Pinning the 18-key `status` IPC JSON schema early kept Python `daemon.py` and C++ `ipc_client.cpp` in sync, and checking SQLite fallback behavior against `remove_folder` caught subtle ghost-folder and stale `"Mac Online"` badge mismatches across offline daemon restarts.
2. **Isolating hardware-specific code behind testable C++ headers:** Extracting `UiController` and `fb_blit.hpp` (`pack_fb_channel` / `blit_framebuffer`) allowed deterministic `ctest` coverage on macOS for keyboard modal flows, RGB/BGR bitfields, and stride bounds before compiling the Linux `fbdev`/`evdev` target inside ARM64 Docker.
3. **Sandboxed socket testing & GPG commits:** Loopback TCP and `AF_UNIX` tests predictably hit macOS sandbox `EPERM`, so pure logic/UI/packaging tests run inside the sandbox while live socket suites and GPG-signed commits isolate cleanly to single `BypassSandbox: true` steps.

---

## Follow-Up Items

- **On real CardputerZero hardware:** Re-build the `.deb` via `./packaging/build-deb.sh` (so `6685844` changes are included), optionally place `Inter-Regular.ttf` / `JetBrainsMono-Regular.ttf` in `packaging/fonts/` for FreeType glyphs instead of the 5×7 bitmap fallback, install `cp0-sync_1.0.0_m5stack1_arm64.deb` via `sudo dpkg -i`, and test `/dev/fb0`, `cp0-keys` `evdev`, `APPLaunch`, and `systemctl --user status cp0-syncd`.
- **On Mac:** Install the receiver daemon via `mac-receiver/bin/cp0-mac-sync install-service` and verify live end-to-end Tailscale transfers from the device.
- **Deferred minor improvements (non-blocking):** Unicode multi-byte UTF-8 glyph rendering in `app_ui.cpp`, clamping `log_scroll` upper bound to `logs.size() - 6` in `ui_controller.cpp`, explicit `libc6, libstdc++6` in `packaging/debian/control`, `LC_NUMERIC` / `EINTR` hardening in `ipc_client.cpp`, transaction context around `SyncStore.log`, and separating `launchd` stdout/stderr log path from `RotatingFileHandler`.
