# Usage, Configuration & Operations Guide

---

## 1. Mac Receiver Operations (`cp0-mac-sync`)

Manage where photos land, check Tailscale/receiver health, view logs, and control the macOS background service using `cp0-mac-sync`.

### Command Reference

| Command | Description |
|---|---|
| `cp0-mac-sync status` | Show `LaunchAgent` state (`com.cardputerzero.photosync`), live HTTP `/health` check (`0.0.0.0:<port>`), destination directory, Tailscale CLI status, and active config path. |
| `cp0-mac-sync set-dir <path>` | Create `<path>` if needed and update `dest_root` in `config.json`. Picked up **automatically on the next upload** (no daemon restart required). |
| `cp0-mac-sync set-port <1-65535>` | Update `listen_port` in `config.json` and restart the `LaunchAgent` if installed (`launchctl kickstart -k`). Default: `8765`. |
| `cp0-mac-sync logs [-n LINES] [-f]` | Print recent lines (`-n 100` default) or tail live receiver logs (`~/Library/Logs/cp0-mac-sync.log`). |
| `cp0-mac-sync install-service` | Write `~/Library/LaunchAgents/com.cardputerzero.photosync.plist` and load/enable the service (`gui/<uid>/com.cardputerzero.photosync`). |
| `cp0-mac-sync uninstall-service` | Stop and unload the `LaunchAgent` and remove its `.plist`. |

### Running `cp0-mac-receiver` Manually (Foreground / Debug)

For foreground debugging without `launchd`:

```bash
./mac-receiver/bin/cp0-mac-receiver --config ~/.config/cp0-mac-sync/config.json --log-file /tmp/cp0-rec.log
```

### Destination Layout & Collision Safety

Uploaded files are written into:

```text
<dest_root>/<folder_label>/<rel_path>
```

For example, if `dest_root` is `~/Pictures/CardputerZero` and CardputerZero watches `/home/pi/Pictures/DCIM/Camera` (label `Camera`):
- `IMG_0042.JPG` → `~/Pictures/CardputerZero/Camera/IMG_0042.JPG`
- `2026-11/IMG_0043.JPG` → `~/Pictures/CardputerZero/Camera/2026-11/IMG_0043.JPG`

Collision & safety guarantees (`mac-receiver/cp0_mac_receiver/server.py`):
- **Atomic staging:** Body streams to a `.cp0-*.part` temp file, hashes SHA-256, validates `X-CP0-SHA256`, applies `os.utime` (`X-CP0-Mtime`) when valid, `fsync`s, and atomically replaces into place (`60s` socket timeout cleans up `.part` files if a connection drops mid-upload).
- **Idempotent duplicate:** If a file already exists at the destination with the **same SHA-256**, it returns `200 OK` (`status: "duplicate"`) without creating extra copies.
- **Collision rename:** If a file exists at the destination with a **different SHA-256**, it is saved alongside it as `<stem>_<YYYYMMDD_HHMMSS>_<sha8><ext>` (never overwriting).
- **Tailscale IP allowlist (`tailscale_only: true`):** Only accepts HTTP requests from Tailscale CGNAT (`100.64.0.0/10`), Tailscale IPv6 (`fd7a:115c:a1e0::/48`), or loopback (`127.0.0.0/8`, `::1/128`); all other client IPs receive `403 Forbidden`.
- **Notifications:** When `notify_on_batch: true`, debounced macOS Notification Center alerts (`2s` debounce window) summarize batch uploads (e.g., `"Synced 5 photos from CardputerZero"`).

---

## 2. CardputerZero Companion UI (`cp0-sync-ui`)

Launch **Photo Sync** from `APPLaunch` or run `cp0-sync-ui` from a shell. The UI refreshes daemon state every `1000 ms` over the daemon's Unix domain socket (falling back cleanly to read-only SQLite + `config.json` display when `cp0-syncd` is stopped).

### Top-Bar Status Pills

| Badge | Meaning |
|---|---|
| **`Mac Online`** | Daemon running, target Mac configured, and last upload/probe succeeded. |
| **`Syncing`** | Daemon is actively streaming queued photos over Tailscale. |
| **`Mac Sleeping`** | Target Mac unreachable; queued photos waiting safely until next wake-probe or `[Enter]`. |
| **`No Target`** | Daemon running, no Mac host configured yet — press **`C`** on `1 Status`. |
| **`Daemon Off`** | `cp0-syncd` IPC socket unreachable (shows read-only cached counts/logs below if `state.db` exists). |

### Tabs & Workflows

#### Tab `1 Status`
Displays two Bento summary tiles (**Pending Queue** file count + total bytes, **Total Synced** count + 1-way verified status), your **Target** host (`host:port`) with retry countdown/uploading/cached indicator, destination directory on the Mac, and last synced filename or error summary.
- Press **`C`** to open the **Target Mac Picker**:
  - Automatically queries `tailscale status --json` and sorts **online macOS peers first**.
  - Navigate with `j`/`k` or `Up`/`Down`, press **`Enter`** to select `<peer-ip-or-dns>:8765`.
  - Press **`T`** inside the picker to manually enter `<host>` or `<host>:<port>` (or IPv6 `[host]:port`).

#### Tab `2 Folders`
Lists all watched directories with badges (`N queued`, `synced`, `N err`, or `! missing`). On first run, `cp0-syncd` creates `/home/pi/Pictures/DCIM/Camera` (label `Camera`) automatically.
- **Add folder (`A`):** Opens the on-device filesystem directory picker starting at your home directory (`~`):
  - `j`/`k` or `Up`/`Down`: move highlight
  - `Enter`: drill down into highlighted subdirectory
  - `Backspace`: go up to parent directory
  - `Space`: select & add the highlighted directory
  - `.`: select & add the directory you are currently inside
  - `T`: switch to manual path typing (supports `~/...` expansion)
  - Nested or overlapping watched directories (parent or child of an existing watched folder) are rejected cleanly with an error toast to avoid double-uploading; duplicate base folder names (e.g., two `Camera` directories) automatically get unique labels (`Camera`, `Camera_2`, ...).
- **Remove folder (`D` or `Delete`):** Opens confirmation modal **`[Y] Confirm / [N] Cancel`**. Removing a watched folder stops watching it and purges `PENDING`/`UPLOADING` queue items, **never deleting photos on disk** and retaining historical `SYNCED` dedup records so re-adding the folder later won't re-upload old files.

#### Tab `3 Logs`
Displays timestamped `INFO`, `WARN`, and `ERROR` entries from SQLite (`state.db` retains the most recent 500 log rows).
- `j`/`k` or `Up`/`Down`: scroll through recent log rows.
- **`R`:** Reset all **`FAILED`** rows back to `PENDING` and trigger an immediate sync/wake probe.

#### Theme Toggle (`T`) & Exit (`Esc` / `Q`)
- Press **`T`** on any main tab (with no modal open) to toggle live between **Zinc Dark** (`#09090b`) and **Zinc Light** (`#fafafa`). The choice is saved to `config.json` via `cp0-syncd`.
- Press **`Esc`** or **`Q`** on main tabs to exit `cp0-sync-ui` cleanly (leaving `cp0-syncd` running in the background) and return to `APPLaunch`.

---

## 3. Daemon CLI & Runtime Behavior (`cp0-syncd`)

### `cp0-syncd` CLI Flags

```bash
cp0-syncd [--config PATH] [--db PATH] [--sock PATH] [--verbose]
```

### File Filtering & Queue Lifecycle
- **Watched media extensions (`DaemonConfig.extensions`):** `.jpg`, `.jpeg`, `.png`, `.webp`, `.gif`, `.bmp`, `.dng`, `.raw`, `.mp4`, `.mov` (case-insensitive).
- **Ignored names:** Prefixes `.` or `~` (`DEFAULT_IGNORE_PREFIXES`), suffixes `.part`, `.tmp`, `.crdownload` (`DEFAULT_IGNORE_SUFFIXES`), and **0-byte files** (logged once as `WARN` without enqueueing).
- **Watch engine:** On Linux, uses native `ctypes` `inotify` (`IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF`) plus `IN_ONLYDIR` ancestor watches so removable SD card mounts are detected automatically; falls back to periodic directory polling on non-Linux hosts.
- **Wake-probe backoff:** When a Mac sleeps or refuses connections, `Uploader` pauses uploads, logs cleanly, probes `GET /health` (`3s` timeout) on backoff `(5s → 10s → 20s → 40s → 60s max)` or wakes immediately on new captured photos or **`Enter`** in the UI.
- **HTTP failure triage:** Network errors, `403`, `408`, and `5xx` pause the queue and retry once the Mac recovers; `400`, `413`, and `422` (and missing local files) mark only that single file `FAILED` so later photos keep flowing.
- **Crash recovery:** On startup, any rows left in `UPLOADING` state from an unexpected power loss automatically reset to `PENDING` before scanning begins.

---

## 4. Configuration Files & Environment Variables

### On Mac
| Item | Default Path | Override Env Var |
|---|---|---|
| Config JSON | `~/.config/cp0-mac-sync/config.json` | `CP0_MAC_CONFIG` |
| Rotating Logs | `~/Library/Logs/cp0-mac-sync.log` *(5 MB × 2 backups)* | — |
| LaunchAgent | `~/Library/LaunchAgents/com.cardputerzero.photosync.plist` | — |
| Synced Photos | `~/Pictures/CardputerZero/<folder_label>/...` | *(via `cp0-mac-sync set-dir`)* |

**Mac `config.json` schema (`MacConfig`):**
```json
{
  "dest_root": "/Users/you/Pictures/CardputerZero",
  "listen_host": "0.0.0.0",
  "listen_port": 8765,
  "notify_on_batch": true,
  "tailscale_only": true
}
```

### On CardputerZero
| Item | Default Path | Override Env Var |
|---|---|---|
| Config JSON | `~/.config/cp0-sync/config.json` | `CP0_SYNC_CONFIG` |
| SQLite WAL DB | `~/.local/share/cp0-sync/state.db` | `CP0_SYNC_DB` |
| Unix IPC Socket | `$XDG_RUNTIME_DIR/cp0-sync.sock` (fallback `/tmp/cp0-sync.sock`) | `CP0_SYNC_SOCK` |
| Sans / Mono Fonts | `/usr/share/cp0-sync/fonts/` → system fonts → 5×7 fallback | `CP0_FONT_SANS`, `CP0_FONT_MONO` |
| Framebuffer (`device` build) | `/dev/fb0` | `CP0_FB_DEVICE` |
| Keyboard `evdev` (`device` build) | Auto-scans `/dev/input/event*` for `EV_KEY` | `CP0_EVDEV_DEVICE` |

---

## 5. Troubleshooting

### UI displays **`Daemon Off`**
Check whether the background `systemd --user` service is running on CardputerZero:
```bash
systemctl --user status cp0-syncd.service
journalctl --user -u cp0-syncd.service -n 50 --no-pager
```
Restart it if needed:
```bash
systemctl --user restart cp0-syncd.service
```

### Status stays on **`Mac Sleeping`** while the Mac is awake
1. On Mac, verify the receiver is running and listening:
   ```bash
   cp0-mac-sync status
   ```
2. Verify macOS Firewall / Little Snitch isn't blocking incoming connections to Python on TCP port `8765`.
3. From CardputerZero over SSH, test reachability over Tailscale:
   ```bash
   curl -i http://<mac-tailscale-ip-or-magicdns>:8765/health
   ```
   A healthy response returns HTTP `200` with `{"ok": true, "version": "1.0.0", "dest_root": "..."}`. If you get `403`, confirm traffic is traveling over Tailscale (`100.64.0.0/10`) rather than an unallowed LAN interface.

### One photo shows **`FAILED`** in Logs
Open **`3 Logs`** in `cp0-sync-ui` to view the error reason (e.g., permission denied, missing source file, destination path conflict), resolve the issue, and press **`R`** to re-queue all `FAILED` entries.

### Re-uploading previously synced photos
Because one-way sync tracks `(folder_id, rel_path, sha256)` in `state.db` so deleted files on your Mac aren't re-copied, intentionally re-syncing everything requires resetting `state.db`:
```bash
systemctl --user stop cp0-syncd.service
rm ~/.local/share/cp0-sync/state.db*
systemctl --user start cp0-syncd.service
```
