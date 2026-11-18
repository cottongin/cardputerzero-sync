> [!IMPORTANT]  
> This project was developed entirely with AI coding assistance (Gemini 4 Argon) and has not undergone rigorous review. It is provided as-is and may require adjustments for other environments.

# CardputerZero Photo Sync

Automatic one-way background photo syncing from an **M5Stack CardputerZero** (Raspberry Pi CM0 / ARM64 Debian Linux) to a **Mac** over **Tailscale**.

Photos saved in watched folders on CardputerZero (default: `/home/pi/Pictures/DCIM/Camera`) are queued locally in SQLite whenever your Mac is sleeping or offline, then streamed over Tailscale to a background macOS HTTP receiver into `~/Pictures/CardputerZero/<folder_label>/<rel_path>` without overwriting collisions.

---

## Architecture Overview

| Component | Path | Runs On | Purpose |
|---|---|---|---|
| **Mac Receiver & CLI** | [`mac-receiver/`](mac-receiver/) | macOS (`LaunchAgent`) | Zero-dependency Python 3 (`>= 3.9`) HTTP receiver (`cp0-mac-receiver`) restricted to Tailscale/loopback IPs, atomic `.part` + SHA-256 verification, macOS notifications, and `cp0-mac-sync` management CLI. |
| **Device Sync Daemon** | [`device-daemon/`](device-daemon/) | CardputerZero (`systemd --user`) | Zero-dependency Python 3 daemon (`cp0-syncd`) with SQLite WAL deduplication queue, Linux `ctypes` `inotify` (+ polling fallback), sleep-aware `/health` wake-probe uploader, and `AF_UNIX` JSON IPC. |
| **Companion UI** | [`device-ui/`](device-ui/) | CardputerZero (`/dev/fb0` + `cp0-keys`) & macOS (`--sim`) | C++17 `320×170` UI (`cp0-sync-ui`) in `shadcn/ui` Zinc Dark/Light themes with FreeType2 (+ 5×7 bitmap fallback) and optional `CP0_HAS_LVGL` hooks. |
| **Debian Packaging** | [`packaging/`](packaging/) | ARM64 Docker / Bookworm | Builds `cp0-sync_1.0.0_m5stack1_arm64.deb` with `cp0-syncd.service` and `APPLaunch` menu entry (`Photo Sync`). |

---

## Quick Start

### 1. Start the Receiver on Your Mac

Ensure Tailscale is connected on both your Mac and CardputerZero, then install and start the macOS `LaunchAgent`:

```bash
./mac-receiver/bin/cp0-mac-sync install-service
./mac-receiver/bin/cp0-mac-sync status
```

*(Optional)* Add `mac-receiver/bin` to your `PATH` (or symlink `cp0-mac-sync` into `~/.local/bin` / `/usr/local/bin`) and change the default destination (`~/Pictures/CardputerZero`) anytime:

```bash
cp0-mac-sync set-dir ~/Pictures/CardputerZero
```

### 2. Build & Install the `.deb` on CardputerZero

Build `cp0-sync_1.0.0_m5stack1_arm64.deb` on your Mac via Docker Desktop (`linux/arm64`) and install it on the CardputerZero:

```bash
# On Mac
./packaging/build-deb.sh
scp packaging/dist/cp0-sync_1.0.0_m5stack1_arm64.deb pi@<cardputerzero-tailscale-host>:/tmp/

# On CardputerZero
sudo dpkg -i /tmp/cp0-sync_1.0.0_m5stack1_arm64.deb
systemctl --user status cp0-syncd.service
```

### 3. Select Your Target Mac on CardputerZero

1. Launch **Photo Sync** from the CardputerZero `APPLaunch` launcher (or run `cp0-sync-ui`).
2. On tab **`1 Status`**, press **`C`** to pick your Mac from discovered online Tailscale peers (or press **`T`** inside the picker to enter `<host>[:port]` manually).
3. Photos taken into `/home/pi/Pictures/DCIM/Camera` (or any folder added under **`2 Folders`** via **`A`**) now queue offline and upload automatically whenever your Mac is awake on Tailscale.

---

## Companion UI Keyboard Shortcuts (`320×170`)

| Key | Where | Action |
|---|---|---|
| `1`, `2`, `3` | Main tabs | Switch between **Status**, **Folders**, and **Logs** |
| `Left` / `Right` | Main tabs | Cycle between tabs |
| `Enter` | Global (no modal) | Trigger immediate sync / wake-probe |
| `T` | Main tabs | Toggle **Zinc Dark** (`#09090b`) / **Zinc Light** (`#fafafa`) theme |
| `C` | `1 Status` | Open Target Mac Tailscale peer picker (`[T]` inside modal for manual `host[:port]`) |
| `A` | `2 Folders` | Open directory browser (`Enter` drill down, `Backspace` up, `Space` watch highlighted, `.` watch current dir, `T` type path) |
| `D` / `Delete` | `2 Folders` | Remove selected watched folder (`Y` confirm / `N` cancel — never deletes photos) |
| `j` / `k` or `Up` / `Down` | Lists / Modals | Move cursor or scroll log rows |
| `R` | `3 Logs` | Re-queue all `FAILED` files for retry |
| `Esc` / `Q` | Modal / Main tabs | Close active modal, or exit `cp0-sync-ui` cleanly back to `APPLaunch` |

---

## Documentation

- **[Building & Installation Guide](docs/building-and-installation.md)** — Prerequisites, building `cp0-sync_1.0.0_m5stack1_arm64.deb` (Docker `linux/arm64` or native on device), font bundling, macOS SDL2 simulator builds, and test suites.
- **[Usage, Configuration & Operations Guide](docs/usage-and-operations.md)** — `cp0-mac-sync` CLI reference, UI workflows, deduplication & sleep-backoff behavior, config paths, environment variables, and troubleshooting.
- **[Design Specification](docs/superpowers/specs/2026-11-18-cardputerzero-photo-sync-design.md)** — Full hardware constraints, HTTP/IPC contracts, SQLite schema, and visual design tokens.
- **[Implementation Plan](docs/superpowers/plans/2026-11-18-cardputerzero-photo-sync.md)** — 7-task architecture and TDD plan.

---

## Repository Layout

```text
.
├── mac-receiver/       # macOS HTTP receiver, cp0-mac-sync CLI, & tests
├── device-daemon/      # CardputerZero systemd user daemon, SQLite queue, & tests
├── device-ui/          # C++17 320x170 companion UI (fbdev/evdev & SDL2 simulator)
├── packaging/          # ARM64 Debian .deb builder, systemd unit, APPLaunch entry
└── docs/               # Build/install guide, usage docs, spec, and implementation plan
```
