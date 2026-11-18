# CardputerZero Photo Sync — Design Specification

**Date:** 2026-11-18  
**Project Size:** Project (3 coordinated deliverables: on-device background sync daemon, on-device LVGL companion UI packaged as an `arm64` `.deb`, and macOS receiver LaunchAgent + CLI)

---

## 1. Intent & Why

The M5Stack CardputerZero (powered by the Raspberry Pi CM0, running ARM64 Debian Linux — *not* the ESP32-based Cardputer/ADV) includes an 8MP Sony IMX219 camera and built-in Camera app, plus support for third-party camera tools and SD card storage. Getting photos off the device currently requires manual copying or plugging in cables, and fails whenever the user's Mac is asleep or away from home Wi-Fi.

This project provides automatic, reliable, one-way photo syncing from the CardputerZero to the user's Mac over Tailscale:

- Photos taken with the native Camera app (or any other camera/screenshot app saving into a watched directory) are detected automatically in the background.
- When the Mac is asleep or offline, photos remain safely queued on the CardputerZero and upload automatically as soon as the Mac becomes reachable again.
- An `APPLaunch` companion app gives the user an on-device frontend on the 320×170 screen to configure watched folders, pick the target Mac, trigger a manual sync, and view queue status and logs.

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- **App-agnostic background watching:** Watch one or more user-defined directories on the CardputerZero so photos from the built-in Camera app or any other app are captured automatically without opening the sync UI.
- **Resilient offline queueing:** Persist queued uploads across CardputerZero reboots and Mac sleep/wake cycles; resume cleanly with zero manual intervention once the Mac wakes.
- **Tailscale transport:** Sync transparently over the user's existing Tailscale network (both on LAN and remotely).
- **One-way "upload once" guarantees:** Track uploaded files locally so moving, renaming, or deleting photos on the Mac after arrival never causes re-uploads.
- **Native `APPLaunch` companion UI:** Provide a keyboard-first 320×170 LVGL UI (`1:Status`, `2:Folders`, `3:Logs`) integrated into the CardputerZero launcher.
- **Configurable Mac destination:** Store received photos on the Mac in an easily changeable root folder, defaulting to `~/Pictures/CardputerZero/<watched-folder-name>/`.

### Non-Goals
- Building a custom camera capture UI or replacing the native Camera app.
- Two-way sync or deleting photos on the CardputerZero when deleted on the Mac.
- Cloud relay servers, external object storage, or multi-Mac sync tracking **(User Decision)** — sync targets a single configured Mac; if multi-Mac sync is ever needed later, it is out of scope for v1.
- Publishing or submitting packages to the official `CardputerZeroRepository` AppStore via `CardputerZero-AppBuilder` (`czdev`) **(User Decision)**.

### Anti-Goals
- Uploading partial/in-progress writes while the camera is still flushing an image to disk.
- Silently overwriting different photos that share a filename (e.g., `IMG_0001.jpg` after a counter reset).
- Requiring heavy typing of Linux file paths or Tailscale MagicDNS hostnames on the 46-key matrix keyboard (`fn`/`sym` layers) when interactive pickers can handle selection.

---

## 3. Constraints

- **Device Hardware (CardputerZero):**
  - Broadcom BCM2837 / RP3A0 (Raspberry Pi CM0, quad-core ARM Cortex-A53 @ 1 GHz), **512 MB LPDDR2 RAM**.
  - **1.9-inch 320×170 LCD**, **no touchscreen**.
  - **46-key physical QWERTY matrix keyboard** (`cp0-keys`) where `fn`, `shift`, and `sym` modify keys via hold, tap-latch, or double-tap lock (e.g., arrows are `fn + F/X/Z/C`, Delete is `fn + Backspace`).
- **Device OS & Packaging:**
  - ARM64 Debian Linux running **`APPLaunch.service`** as a `systemd --user` service.
  - Apps register a `.desktop` file under `/usr/share/APPLaunch/applications/` and icon assets under `/usr/share/APPLaunch/share/images/`.
  - Distributed as a local `arm64` Debian package (`cp0-sync_1.0.0_m5stack1_arm64.deb`) built via CMake/CPack (in an ARM64 Docker container or directly on the CardputerZero over SSH) for personal installation via `sudo apt install ./cp0-sync_1.0.0_m5stack1_arm64.deb` **(User Decision)**.
- **Network & Mac Environment:**
  - Tailscale is already installed and running on both the CardputerZero and the Mac on a **strictly private tailnet of authorized devices** **(User Decision)**.
  - Mac receiver runs on macOS as a user background agent (`LaunchAgent`).

---

## 4. Approaches Considered & Selected

Three architectural approaches were evaluated:

1. **Approach 1 (Selected by User): Official `CardputerZero/Template` C++ (LVGL UI) + Zero-Dependency Python 3 Sync Daemon & Mac Receiver**
   - *How it works:* The `APPLaunch` frontend (`cp0-sync-ui`) is built in C++ / LVGL using `CardputerZero/Template` (CMake + CPack `.deb`, with local macOS SDL2 simulator support). Both the on-device background `systemd` service (`cp0-syncd`) and the Mac receiver (`cp0-mac-receiver` + `cp0-mac-sync` CLI) are written in pure Python 3 standard library (`sqlite3`, `http.server`/`urllib`, `ctypes` Linux `inotify`).
   - *Tradeoffs:* Native LVGL performance and full `CardputerZero/Template` / `APPLaunch` compatibility without AppStore/CI submission constraints, while keeping networking, hashing, queueing, and filesystem watching in zero-dependency Python (~12 MB RSS) that shares protocol logic cleanly between device and Mac and is easy to inspect/debug on-device.

2. **Approach 2: All-C++ Device Package (LVGL UI + C++ Daemon) + Python Mac Receiver**
   - *How it works:* Both `cp0-sync-ui` and `cp0-syncd` are compiled C++ binaries using `libcurl`, `libsqlite3`, and `<sys/inotify.h>`.
   - *Tradeoffs:* Lowest device RAM usage (~3 MB RSS) and single-language device codebase, but requires `libcurl`/`sqlite3` ARM64 cross-compile sysroot dependencies and recompiling the `.deb` for any daemon change.

3. **Approach 3: All-Python Stack (Python Framebuffer/SDL UI + Python Daemon + Python Mac Receiver)**
   - *How it works:* Everything, including the 320×170 UI, is written in Python (`pygame`/`pillow`).
   - *Tradeoffs:* Avoids C++ cross-compilation, but diverges from the official `CardputerZero/Template` LVGL UX and adds heavy Python graphics dependencies.

---

## 5. Detailed Architecture & Component Design

> **Attribution key:** Items marked **(User Decision)** were explicitly requested or confirmed by the user. Items marked **(Our Call)** are design decisions filled in during specification with rationale provided.

### 5.1 Repository Layout **(Our Call)**
Monorepo under `cardputerzero-sync/`:
- `device-daemon/` — Python 3 `cp0-syncd` service, SQLite queue store, `inotify` watcher, HTTP/Tailscale uploader, Unix socket control server, and unit tests.
- `device-ui/` — `CardputerZero/Template`-based C++/LVGL companion app (`cp0-sync-ui`), SDL desktop simulator preset, `APPLaunch` `.desktop` + icon assets, and CMake/CPack `.deb` packaging config.
- `mac-receiver/` — Python 3 `cp0-mac-receiver` HTTP server, `cp0-mac-sync` CLI helper, `LaunchAgent` plist installer, and unit tests.
- `packaging/` — Debian maintainer scripts (`postinst`, `prerm`, `postrm`), `systemd` user unit file (`cp0-syncd.service`), and shared protocol schema docs.

---

### 5.2 On-Device Background Sync Daemon (`cp0-syncd`)

#### Service Lifecycle & Shared Storage **(Our Call)**
- Installed at `/usr/bin/cp0-syncd` with a **`systemd --user` service** at `/usr/lib/systemd/user/cp0-syncd.service` (matching `APPLaunch.service`, which also runs under `systemd --user`).
- Debian `postinst` runs `systemctl --global enable cp0-syncd.service` so the daemon starts automatically on boot for the device's desktop user under the exact same `$HOME` and UID as `cp0-sync-ui`.
- **Shared state paths** (also overrideable via environment variables `CP0_SYNC_CONFIG`, `CP0_SYNC_DB`, `CP0_SYNC_SOCK` for testing):
  - Config file: `~/.config/cp0-sync/config.json`
  - SQLite database: `~/.local/share/cp0-sync/state.db` (WAL mode enabled so `cp0-sync-ui` can read live status concurrently without locking the daemon)
  - Control Unix domain socket: `${XDG_RUNTIME_DIR:-/tmp}/cp0-sync.sock`

#### Configuration Schema (`~/.config/cp0-sync/config.json`) **(Our Call + User Decisions)**
- `target_host`: Tailscale MagicDNS hostname or `100.x.y.z` IP of the Mac (initially empty until configured in UI or CLI; daemon stays idle and logs `No target host configured` without error loops).
- `target_port`: Integer, default `8765`.
- `theme`: `"dark"` or `"light"` (default `"dark"`), controlling the UI's Zinc Dark vs. Zinc Light palette **(User Decision)**.
- **No `auth_token` required **(User Decision)**:** Because the tailnet contains strictly private, authorized devices, transport security relies on Tailscale WireGuard encryption plus the Mac receiver's Tailscale CIDR allowlist (`100.64.0.0/10`, `fd7a:115c:a1e0::/48`, and loopback).
- `watched_folders`: List of objects `{ "id": "<uuid-or-slug>", "path": "/abs/path", "label": "Camera", "enabled": true }`.
  - **Label uniqueness:** Derived initially from folder basename (`Camera`, `DCIM`, `Screenshots`), automatically disambiguated if two watched paths share a basename (e.g., `DCIM`, `DCIM-2`) so Mac subfolders never collide.
  - **First-run default **(User Decision)**:** On first boot when no config exists, `cp0-syncd` pre-populates a single default watched folder: `~/Pictures/DCIM/Camera` (i.e., `/home/pi/Pictures/DCIM/Camera`, label `Camera`), matching the native CardputerZero Camera app. Additional folders (such as SD card paths or other app output directories) can be added anytime via Tab `2:Folders`.
  - **Overlap prevention:** `add_folder` resolves canonical paths (`os.path.realpath`) and warns/blocks (or skips duplicate ingestion) if a newly added folder overlaps an already-watched ancestor/descendant directory.
- `extensions`: Case-insensitive media allowlist, defaulting to `[".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".dng", ".raw", ".mp4", ".mov"]`.
- `ignore_prefixes`: `[".", "~"]`, `ignore_suffixes`: `[".part", ".tmp", ".crdownload"]`.

#### File Watching & Queue Ingestion **(Our Call)**
- **Startup reconciliation:** On daemon startup (and whenever a watched folder is added), recursively walk each enabled watched folder (`os.walk` with `followlinks=False`). Any matching file not already recorded as `SYNCED` (same path, size, and `mtime_ns` — or matching SHA-256) is enqueued as `PENDING`.
- **Real-time `inotify` watcher & mount resilience:** Pure-Python `ctypes` bindings against `libc.so.6` (`inotify_init1`, `inotify_add_watch`, `inotify_rm_watch`) watching each directory and dynamically attaching watches to newly created subdirectories (`IN_CREATE | IN_ISDIR`).
  - If a configured watched folder does not currently exist (e.g., an unmounted SD card path), the daemon keeps it in config (showing `! unmounted/missing` in UI) and watches its nearest existing ancestor directory (or re-checks on a periodic timer) so inserting an SD card later automatically attaches watches and triggers a scan.
  - Only reacts to **`IN_CLOSE_WRITE`** and **`IN_MOVED_TO`** events (never `IN_CREATE` for files), guaranteeing the camera app has finished writing and closed the file descriptor before hashing or uploading.
  - Enqueues newly closed/moved media files immediately into SQLite (`status='PENDING'`) and wakes the uploader thread.

#### SQLite State Schema (`state.db`) **(Our Call)**
- `files` table:
  - `id` INTEGER PRIMARY KEY
  - `folder_id` TEXT NOT NULL
  - `abs_path` TEXT NOT NULL
  - `rel_path` TEXT NOT NULL (relative to watched folder root, preserving any camera date subdirectories)
  - `size_bytes` INTEGER NOT NULL
  - `mtime_ns` INTEGER NOT NULL
  - `sha256` TEXT NOT NULL
  - `status` TEXT NOT NULL (`PENDING`, `UPLOADING`, `SYNCED`, `FAILED`)
  - `retry_count` INTEGER DEFAULT 0
  - `last_error` TEXT
  - `discovered_at` INTEGER NOT NULL (Unix epoch ms)
  - `synced_at` INTEGER
  - Unique index on `(folder_id, rel_path, sha256)` — ensures global one-way "upload once" behavior for the configured single-Mac target **(User Decision)** even if files are later moved or deleted on the Mac, while re-uploading if a file at the same path is genuinely modified/replaced with different content.
- `logs` table:
  - `id` INTEGER PRIMARY KEY AUTOINCREMENT
  - `ts` INTEGER NOT NULL (Unix epoch ms)
  - `level` TEXT NOT NULL (`INFO`, `WARN`, `ERROR`)
  - `message` TEXT NOT NULL
  - Automatically pruned to the newest 500 rows to bound SD/eMMC wear and disk usage.
- `meta` table:
  - Key-value runtime summary (`mac_online`, `mac_dest_root`, `last_probe_ts`, `next_retry_ts`, `last_sync_ts`, `last_synced_file`) written by the daemon so Tab `1:Status` renders instantly without expensive queries.

#### Wake Probe, Upload Loop & Backoff **(Our Call)**
- When `PENDING` rows exist and `target_host` is configured:
  1. Daemon sends `GET http://<target_host>:<target_port>/health` with a short **3-second connect/read timeout**.
  2. If unreachable (Mac sleeping, offline, or Tailscale tunnel asleep): marks `mac_online = false` in `meta`, sleeps with adaptive backoff (**5s → 10s → 20s → 40s → 60s max**, resetting to 5s whenever a new photo is captured), and keeps everything queued safely in SQLite.
  3. If `200 OK`: marks `mac_online = true`, caches `mac_dest_root` from the health JSON (for display on Tab `1:Status`), and streams `PENDING` files sequentially in FIFO order (`discovered_at ASC`) via `POST /upload`.
  4. **Failure handling:** Network drop, `5xx`, or auth/access rejection (`401`/`403`) keeps the file as `PENDING` (pausing the upload queue with backoff and surfacing an error state in `meta`/`logs` rather than poisoning individual files). Only per-file payload rejections (`400`/`413`/`422`) mark that specific file `FAILED` with `last_error` so it doesn't block the rest of the queue; the user can retry failed items from Tab `3:Logs` (`[R]`).
  5. **Immediate wake on command:** `Sync Now` or config edits over the Unix socket interrupt the backoff sleep immediately (`threading.Event.set()`), resetting backoff and probing at once.

#### Local Control IPC Protocol (`/tmp/.../cp0-sync.sock`) **(Our Call)**
Newline-delimited JSON request/response over Unix stream socket:
- `{"cmd": "status"}` → returns current `meta` + queue counts + watched folder list with per-folder `pending`/`synced` counts.
- `{"cmd": "sync_now"}` → interrupts sleep, resets retry backoff, triggers immediate folder rescan + `/health` probe + queue flush.
- `{"cmd": "add_folder", "path": "...", "label": "..."}` → validates directory exists, saves config, attaches `inotify` watch, triggers rescan.
- `{"cmd": "remove_folder", "folder_id": "..."}` → removes folder from `config.json`, detaches `inotify` watch, purges un-synced `PENDING` queue entries for that folder (never touches files on disk).
- `{"cmd": "set_target", "host": "...", "port": 8765}` → saves `config.json` and immediately probes new target.
- `{"cmd": "set_theme", "theme": "dark" | "light"}` → persists the active UI theme in `config.json`.
- `{"cmd": "retry_failed"}` → flips `FAILED` rows back to `PENDING` and triggers `sync_now`.

---

### 5.3 Mac Receiver & CLI (`cp0-mac-receiver` & `cp0-mac-sync`)

#### User-Specified Requirements **(User Decision)**
- Lightweight receiver daemon running on the Mac (`LaunchAgent`), communicating over Tailscale.
- Stores synced photos under `~/Pictures/CardputerZero/` by default, grouped into subfolders by watched folder label (`~/Pictures/CardputerZero/<folder_label>/<rel_path>`).
- Destination root path on the Mac must be easy to set and change.

#### Configuration & Service Management **(Our Call)**
- **Config file:** `~/.config/cp0-mac-sync/config.json`:
  ```json
  {
    "dest_root": "/Users/<user>/Pictures/CardputerZero",
    "listen_host": "0.0.0.0",
    "listen_port": 8765,
    "notify_on_batch": true,
    "tailscale_only": true
  }
  ```
- **Logging:** `~/Library/Logs/cp0-mac-sync.log` (rotated at 5 MB, keeping 2 backups).
- **`LaunchAgent`:** `~/Library/LaunchAgents/com.cardputerzero.photosync.plist` with `RunAtLoad = true` and `KeepAlive = true`, managed via `launchctl bootstrap`/`bootout`.
- **`cp0-mac-sync` CLI** (installed to `/usr/local/bin/cp0-mac-sync` or `~/.local/bin/cp0-mac-sync`):
  - `cp0-mac-sync install-service` — initializes config, creates default `~/Pictures/CardputerZero`, installs and loads the `LaunchAgent`.
  - `cp0-mac-sync uninstall-service` — unloads and removes the `LaunchAgent` (keeps synced photos intact).
  - `cp0-mac-sync set-dir <path>` — validates/creates `<path>`, updates `config.json`, and reloads/notifies the receiver immediately (no manual plist editing).
  - `cp0-mac-sync set-port <port>` — updates port and restarts the `LaunchAgent`.
  - `cp0-mac-sync status` — prints service running state, Tailscale IP/MagicDNS name (via `tailscale status --json` if available), current `dest_root`, and recent upload count.
  - `cp0-mac-sync logs [--follow]` — displays or tails `~/Library/Logs/cp0-mac-sync.log`.

#### HTTP Receiver Protocol & Security **(Our Call + User Decision)**
Pure Python `ThreadingHTTPServer`:
- **Source IP filtering **(User Decision + Our Call)**:** Because the user's tailnet contains strictly private/authorized devices, no pairing token is used. When `tailscale_only` is `true` (default), the receiver rejects any client IP outside Tailscale CGNAT (`100.64.0.0/10`), Tailscale IPv6 (`fd7a:115c:a1e0::/48`), or loopback (`127.0.0.1`, `::1`) with `403 Forbidden`. Listening on `0.0.0.0:8765` avoids binding failures if the Tailscale interface comes up after user login, while the CIDR allowlist guarantees LAN/public interfaces cannot reach it.
- **`GET /health`:** Returns `200 OK` JSON:
  ```json
  {
    "status": "ok",
    "version": "1.0.0",
    "hostname": "<mac-hostname>",
    "dest_root": "/Users/<user>/Pictures/CardputerZero"
  }
  ```
- **`POST /upload`:**
  - **Headers:** `Content-Length`, `X-CP0-Folder-Label`, `X-CP0-Rel-Path`, `X-CP0-SHA256`, `X-CP0-Mtime` (Unix epoch seconds float/int).
  - **Path traversal sanitization:** Rejects (`400`) empty/absolute paths or any `..` segment in `folder_label` or `rel_path`; enforces resolved destination strictly inside `dest_root`.
  - **Streaming & atomic commit:**
    1. Streams request body in 64 KB chunks to `<target_dir>/.cp0-<uuid>.part` while computing SHA-256 incrementally.
    2. If stream disconnects prematurely or computed SHA-256 != `X-CP0-SHA256`, deletes `.part` file and returns `400`/`499`.
    3. If destination path already exists:
       - Computes existing file's SHA-256; if identical, deletes `.part` and returns `200 {"status": "already_exists"}` (deduplication on retry).
       - If different content exists at that name, renames destination to `<stem>_<YYYYMMDD_HHMMSS>_<sha8><ext>` so no photo is ever overwritten.
    4. Atomically renames `.part` → final path (`os.replace`) and applies original capture timestamp via `os.utime`.
    5. Returns `200 {"status": "saved", "saved_path": "..."}`.
- **Batch macOS Notification:** A 5-second debounce timer fires a single native macOS notification via `osascript` (`display notification "Synced N photos to CardputerZero" with title "CardputerZero Sync"`) once a burst of uploads settles, rather than spamming one banner per photo.

---

### 5.4 CardputerZero Companion UI (`cp0-sync-ui`)

#### Visual & Design System — `shadcn/ui` Zinc Style for LVGL v9 **(User Decision + Our Calls)**
- Built on `CardputerZero/Template` using C++ / LVGL v9 for the **320×170** display **(User Decision)**.
- **Layout & Component Geometry (Option A: Bento Cards + Segmented Tabs) **(User Decision)**:**
  - **Top bar (24 px):** Segmented `TabsList` pill container (`radius: 5px`, `padding: 2px`) on the left (`1 Status`, `2 Folders`, `3 Logs` with `radius: 4px` active pill) + right-aligned pill status `Badge` (`radius: 999px`, `1px` border: `● Mac Online` / `● Mac Sleeping` / `↻ Syncing` / `○ No Target`).
  - **Main content (126 px):** Full 320px width dedicated to the active tab:
    - `1 Status`: Two side-by-side bordered `Card` stat tiles (`Pending Queue` and `Total Synced`, `radius: 6px`, `1px` border) above a full-width bordered `Target` summary card (`Target <host>`, retry/last-sync indicator, and `→ <dest_root>/<folder>`).
    - `2 Folders`: Stacked bordered `Card` rows (`radius: 6px`, `1px` border) showing full path on the left and a pill `Badge` (`queued` / `synced` / `missing`) on the right.
    - `3 Logs`: Monospace log list separated by `1px` hairline row dividers (`HH:MM:SS`, `OK`/`IN`/`WN`/`ER` tag, message).
  - **Bottom key-hint bar (20 px):** `<kbd>` keycap badges (`radius: 3px`, `1px` border) paired with context-sensitive shortcut labels.
- **Dual Toggleable Theme Tokens (Zinc Dark & Standard Zinc Light) **(User Decision)**:**
  - **Zinc Dark (`"theme": "dark"`, default):**
    - `--background`: `#09090b` (`zinc-950`), `--card`: `#09090b`, `--muted`: `#18181b` (`zinc-900`), `--border`: `#27272a` (`1px`), `--ring` (focused row/control border): `#d4d4d8`, `--foreground`: `#fafafa`, `--muted-foreground`: `#a1a1aa`, active tab pill: `#27272a` fill / `#fafafa` text.
    - Semantic accents (badges/dots only): Online/Synced `#4ade80`, Sleeping/Queued `#fbbf24`, Missing/Error `#f87171`.
  - **Zinc Light (`"theme": "light"`, balanced standard `shadcn/ui` light skin):**
    - `--background`: `#fafafa` (`zinc-50`), `--card`: `#ffffff`, `--muted`: `#f4f4f5` (`zinc-100`), `--border`: `#e4e4e7` (`1px`), `--ring` (focused row/control border): `#71717a` (balanced neutral focus ring rather than stark high-contrast black), `--foreground`: `#09090b`, `--muted-foreground`: `#71717a`, active tab pill: `#ffffff` fill with `#e4e4e7` border / `#09090b` text.
    - Semantic accents (badges/dots only): Online/Synced `#16a34a` (`#dcfce7` badge fill), Sleeping/Queued `#d97706` (`#fef3c7` badge fill), Missing/Error `#dc2626` (`#fee2e2` badge fill).
- **Typography **(Our Call)**:** Proportional sans-serif (`Inter` bundled and loaded via LVGL FreeType) for UI chrome, headers, tabs, and badges paired with monospace (`JetBrains Mono` via FreeType) for paths, stat counts, hostnames, `<kbd>` keycaps, and log timestamps.
- **Data access **(Our Call)**:** Polls `cp0-syncd` over the Unix socket every 1 second while open (with direct read-only SQLite fallback to display cached state even if the daemon is restarting) and sends mutations (`sync_now`, `add_folder`, `remove_folder`, `set_target`, `set_theme`, `retry_failed`) over the socket.

#### Tab Views & Keyboard Map **(Our Call)**
Designed around the `cp0-keys` 46-key matrix so common actions do not require multi-key `fn` chords unless desired:

- **Global shortcuts (when no modal is open):**
  - `1` / `2` / `3`: Jump directly to `Status`, `Folders`, or `Logs` tab.
  - `Enter`: Trigger `Sync Now` (`sync_now` IPC) and flash feedback in the status banner.
  - `T`: Toggle live between **Zinc Dark** and **Zinc Light** and persist `"theme"` via `set_theme` IPC **(User Decision)**. (When inside the Target Config or Directory Picker modal, `T` retains its modal role of opening manual text input.)
  - `Esc` or `Q`: Quit `cp0-sync-ui` back to `APPLaunch` (leaving `cp0-syncd` running in the background).
  - Navigation: Both `LV_KEY_UP`/`DOWN` (`fn+F`/`fn+X`) and `k`/`j` move selection/scroll; `LV_KEY_LEFT`/`RIGHT` (`fn+Z`/`fn+C`) also switch tabs.

- **Tab `1:Status`:**
  - Displays: `QUEUE: <N> pending (<X.X> MB)` | `SYNCED: <M>`, `TARGET: <host>:<port>`, `DEST: <mac_dest_root>/<folder>` (fetched from `/health`), and a status strip showing `Last sync: ...` or `Next retry in <N>s` / current upload filename.
  - `[C]` opens **Target Mac Config Modal**:
    - Runs `tailscale status --json` locally to list online/offline Tailscale peers (filtering to non-self nodes, prioritizing macOS/Darwin peers at the top), so the user can pick their Mac with arrow/`j`/`k` + `Enter` without typing `.ts.net` addresses.
    - Also includes a `[T] Type host manually` option with an LVGL text input field + port field.

- **Tab `2:Folders`:**
  - Scrollable list of watched folders showing path (left) and status badge (`3 queued` amber / `✓ synced` green / `! missing` red).
  - `[A]` opens **Keyboard Directory Picker Modal**:
    - Root choices `/home`, `/media`, `/` (or starts in `$HOME`).
    - Arrow/`j`/`k` moves cursor, `Enter` drills into directory, `Backspace` goes up parent directory (`..`), **`Space`** watches the highlighted directory (or current directory), **`[T]`** opens a manual path text input fallback.
  - `[D]` or `Delete` (`fn+Backspace`): Opens a brief `[Y/N]` confirmation bar to remove the selected folder from the watch list (explicitly noting local files on disk are not deleted).

- **Tab `3:Logs`:**
  - Scrollable monospace log viewer reading the newest entries from `state.db` (`HH:MM:SS` + concise message, color-coded by `INFO`/`WARN`/`ERROR`).
  - `[R]` retries all `FAILED` files (`retry_failed` IPC).

---

### 5.5 Packaging & Launcher Integration **(Our Call + User Decision)**
- **Package name, version, & build path:** `cp0-sync_1.0.0_m5stack1_arm64.deb` generated locally via CMake/CPack (ARM64 Docker build script or native build on CardputerZero over SSH) for personal installation via `sudo apt install ./cp0-sync_1.0.0_m5stack1_arm64.deb` **(User Decision)**. No `CardputerZeroRepository` or `czdev` CI submission layout constraints apply.
- **Files installed on CardputerZero:**
  - `/usr/bin/cp0-sync-ui` (compiled C++/LVGL binary)
  - `/usr/bin/cp0-syncd` (Python 3 daemon entrypoint, `chmod +x`)
  - `/usr/lib/systemd/user/cp0-syncd.service` (`WantedBy=default.target`, `After=network-online.target`)
  - `/usr/share/APPLaunch/applications/cp0-sync.desktop`
  - `/usr/share/APPLaunch/share/images/cp0-sync.png` (custom app icon sized for `APPLaunch`)
- **Debian `Depends`:** `python3 (>= 3.9)` (zero third-party pip packages required) plus standard LVGL/runtime shared deps resolved via `dpkg-shlibdeps`.
- **Maintainer scripts:**
  - `postinst`: runs `systemctl --global enable cp0-syncd.service` and restarts the user service for active graphical sessions if present.
  - `prerm`: stops/disables `cp0-syncd.service` cleanly on package removal.

---

## 6. What's Left to the Builder

- LVGL v9 `lv_style_t` token table, bundled FreeType `.ttf` subsetting/loading (`Inter` + `JetBrains Mono`), and live theme switcher implementing the specified `shadcn/ui` Zinc Dark and Zinc Light palettes on the 320×170 display.
- CMake toolchain/preset configuration for both local macOS SDL2 simulator testing and local ARM64 `.deb` packaging (Docker `linux/arm64` build script or native CardputerZero SSH build).
- Concrete implementation of Python modules (`cp0-syncd`, `cp0-mac-receiver`, `cp0-mac-sync`), plist template, and `postinst`/`prerm` packaging scripts.
- Automated unit and end-to-end integration tests:
  - Python tests for `inotify` close-write handling, SQLite queue persistence/dedup, atomic `.part` transfer, collision renaming, Tailscale CIDR allowlist, and sleep/wake retry recovery.
  - C++ UI state/IPC parsing tests and SDL2 simulator verification at 320×170.
