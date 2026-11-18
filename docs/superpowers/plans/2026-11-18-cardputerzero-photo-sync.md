# CardputerZero Photo Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an automatic one-way photo syncing system from an M5Stack CardputerZero (CM0 ARM64 Linux) over Tailscale to a macOS receiver, complete with offline queueing, a `cp0-mac-sync` Mac CLI/LaunchAgent, and a `shadcn/ui`-styled 320×170 LVGL v9 companion app for `APPLaunch`.

**Architecture:** A zero-dependency Python 3 `systemd --user` background daemon (`cp0-syncd`) watches user-configured directories via `inotify` + startup walk, queues photos in SQLite (WAL mode), probes the Mac receiver (`cp0-mac-receiver`) over Tailscale, and uploads atomically with SHA-256 verification. On the CardputerZero, a C++17/LVGL v9 companion UI (`cp0-sync-ui`) communicates with `cp0-syncd` over a Unix domain socket (with read-only SQLite fallback) and ships alongside `cp0-syncd` in a local `arm64` `.deb` package.

**Tech Stack:** Python 3.9+ standard library (`sqlite3`, `http.server`, `urllib`, `ctypes` `libc` `inotify`), C++17, LVGL v9, FreeType (`Inter` + `JetBrains Mono`), SDL2 (macOS simulator), Linux `fbdev`/`evdev` (device), CMake + CPack / `dpkg-deb`.

**Spec:** [docs/superpowers/specs/2026-11-18-cardputerzero-photo-sync-design.md](../specs/2026-11-18-cardputerzero-photo-sync-design.md)

---

## Global Constraints

- **Target Device:** M5Stack CardputerZero (Raspberry Pi CM0, quad-core Cortex-A53, 512 MB RAM, ARM64 Debian Linux, 320×170 non-touch LCD, 46-key `cp0-keys` matrix keyboard, `APPLaunch.service` running under `systemd --user`).
- **Python Dependencies:** Zero third-party `pip` packages (`python3 (>= 3.9)` stdlib only) for both `cp0-syncd` and `cp0-mac-receiver`.
- **Default Paths:** CardputerZero first-run watched folder is `/home/pi/Pictures/DCIM/Camera` (resolved via `~/Pictures/DCIM/Camera`, label `Camera`); Mac receiver default root is `~/Pictures/CardputerZero/<folder_label>/<rel_path>`.
- **Network & Security:** Tailscale strictly private tailnet; no pairing `auth_token` or `X-CP0-Token`. Mac receiver listens on `0.0.0.0:8765` (configurable) and enforces Tailscale CIDR allowlist (`100.64.0.0/10`, `fd7a:115c:a1e0::/48`) + loopback (`127.0.0.1`, `::1`).
- **Sync Semantics:** Global one-way "upload once" keyed by `(folder_id, rel_path, sha256)`; never re-uploads if moved/deleted on Mac; never overwrites a different photo on filename collision (`<stem>_<YYYYMMDD_HHMMSS>_<sha8><ext>`).
- **UI Theme:** `shadcn/ui` Zinc Dark (`#09090b` bg, `#18181b` muted, `#27272a` 1px border, `#fafafa` fg) and balanced Zinc Light (`#fafafa` bg, `#ffffff` card, `#f4f4f5` muted, `#e4e4e7` 1px border, `#71717a` focus ring, `#09090b` fg), toggleable live via `[T]` and persisted in `config.json`.
- **Packaging:** Personal local build `cp0-sync_1.0.0_m5stack1_arm64.deb` (no `czdev` / `CardputerZeroRepository` AppStore submission constraints).

## Review Focus

1. **Percent-encoding of spaces/UTF-8 paths in HTTP headers:** `X-CP0-Folder-Label` and `X-CP0-Rel-Path` can contain spaces or non-ASCII characters (e.g., `Trip Photos/café.jpg`); both `Uploader` and `cp0-mac-receiver` must percent-encode/decode header values (`urllib.parse.quote`/`unquote`) rather than failing HTTP header Latin-1 encoding. *(Pinned in Task 1 & Task 4)*
2. **Zero-byte media files from crashed camera writes:** A `0`-byte `.jpg` written before a crash must be skipped with a `WARN` log by `SyncStore`/`Watcher` rather than uploaded as a corrupt empty file. *(Pinned in Task 3)*
3. **Queued `PENDING` file deleted on CardputerZero before Mac wakes:** `Uploader` must catch `FileNotFoundError` when opening a `PENDING` file, delete the stale row from `state.db`, log a warning, and proceed with the remaining queue. *(Pinned in Task 4)*
4. **Stale Unix socket file after unclean shutdown:** `IpcServer` must unlink any pre-existing stale socket path before `bind()` so `cp0-syncd` restarts cleanly after power loss. *(Pinned in Task 4)*
5. **Corrupt or truncated `config.json` on disk:** Both `MacConfig` and `DaemonConfig` must save atomically (`.tmp` + `os.replace`) and fall back safely to defaults on malformed JSON rather than crashing on startup. *(Pinned in Task 1 & Task 3)*

---

### Task 1: Mac Receiver Config, Tailscale IP Filter, & Atomic HTTP Server

**Files:**
- Create: `mac-receiver/cp0_mac_receiver/__init__.py`
- Create: `mac-receiver/cp0_mac_receiver/config.py`
- Create: `mac-receiver/cp0_mac_receiver/server.py`
- Test: `mac-receiver/tests/test_server.py`

**Interfaces:**
- Produces:
  - `MacConfig(dest_root: str, listen_host: str, listen_port: int, notify_on_batch: bool, tailscale_only: bool)`
  - `load_mac_config(path: Path | None = None) -> MacConfig`
  - `save_mac_config(cfg: MacConfig, path: Path | None = None) -> None`
  - `is_allowed_client_ip(ip_str: str, tailscale_only: bool) -> bool`
  - `MacReceiverServer(config_path: Path, notifier: Callable[[int, Path], None] | None = None)` with `.start()`, `.shutdown()`, `.server_address`

- [ ] **Step 1: Write failing tests for config, IP allowlist, `/health`, `/upload`, dedup, collision rename, path traversal, and percent-encoded headers**

```python
# mac-receiver/tests/test_server.py
import hashlib, json, os, tempfile, threading, unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import Request, urlopen

from cp0_mac_receiver.config import MacConfig, load_mac_config, save_mac_config
from cp0_mac_receiver.server import MacReceiverServer, is_allowed_client_ip

class TestMacConfigAndFilter(unittest.TestCase):
    def test_corrupt_config_falls_back_to_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "config.json"
            cfg_path.write_text("{broken json")
            cfg = load_mac_config(cfg_path)
            self.assertEqual(cfg.listen_port, 8765)
            self.assertTrue(cfg.tailscale_only)

    def test_tailscale_ip_filter(self):
        self.assertTrue(is_allowed_client_ip("100.101.102.103", True))
        self.assertTrue(is_allowed_client_ip("fd7a:115c:a1e0::1", True))
        self.assertTrue(is_allowed_client_ip("127.0.0.1", True))
        self.assertFalse(is_allowed_client_ip("192.168.1.50", True))
        self.assertTrue(is_allowed_client_ip("192.168.1.50", False))

class TestMacReceiverServer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dest_root = Path(self.tmp.name) / "CardputerZero"
        self.cfg_path = Path(self.tmp.name) / "config.json"
        save_mac_config(MacConfig(dest_root=str(self.dest_root), listen_host="127.0.0.1", listen_port=0, notify_on_batch=True, tailscale_only=True), self.cfg_path)
        self.notified = []
        self.srv = MacReceiverServer(self.cfg_path, notifier=lambda n, d: self.notified.append((n, d)), debounce_sec=0.05)
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.srv.server_port}"

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.tmp.cleanup()

    def _upload(self, label: str, rel_path: str, data: bytes, mtime: float = 1700000000.0):
        sha = hashlib.sha256(data).hexdigest()
        req = Request(f"{self.base_url}/upload", data=data, method="POST", headers={
            "Content-Length": str(len(data)),
            "X-CP0-Folder-Label": quote(label, safe=""),
            "X-CP0-Rel-Path": quote(rel_path, safe="/"),
            "X-CP0-SHA256": sha,
            "X-CP0-Mtime": str(mtime),
        })
        return urlopen(req)

    def test_health_returns_dest_root(self):
        with urlopen(f"{self.base_url}/health") as resp:
            self.assertEqual(resp.status, 200)
            body = json.loads(resp.read())
            self.assertEqual(body["status"], "ok")
            self.assertEqual(body["dest_root"], str(self.dest_root))

    def test_upload_dedup_collision_and_utf8_headers(self):
        payload = b"jpeg-bytes-1"
        r1 = json.loads(self._upload("Trip Photos", "2026/café 1.jpg", payload, 1700000123.0).read())
        self.assertEqual(r1["status"], "saved")
        target = self.dest_root / "Trip Photos" / "2026" / "café 1.jpg"
        self.assertEqual(target.read_bytes(), payload)
        self.assertEqual(int(target.stat().st_mtime), 1700000123)

        r2 = json.loads(self._upload("Trip Photos", "2026/café 1.jpg", payload, 1700000123.0).read())
        self.assertEqual(r2["status"], "already_exists")

        r3 = json.loads(self._upload("Trip Photos", "2026/café 1.jpg", b"jpeg-bytes-different", 1700000456.0).read())
        self.assertEqual(r3["status"], "saved")
        self.assertNotEqual(Path(r3["saved_path"]), target)
        self.assertEqual(target.read_bytes(), payload)

    def test_rejects_path_traversal_and_bad_sha256(self):
        with self.assertRaises(HTTPError) as ctx:
            self._upload("../etc", "passwd", b"bad")
        self.assertEqual(ctx.exception.code, 400)

        req = Request(f"{self.base_url}/upload", data=b"abc", method="POST", headers={
            "Content-Length": "3", "X-CP0-Folder-Label": "Camera",
            "X-CP0-Rel-Path": "a.jpg", "X-CP0-SHA256": "0" * 64, "X-CP0-Mtime": "1700000000",
        })
        with self.assertRaises(HTTPError) as ctx2:
            urlopen(req)
        self.assertEqual(ctx2.exception.code, 400)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `PYTHONPATH=mac-receiver python3 -m unittest discover -s mac-receiver/tests -v`
Expected: FAIL (`ModuleNotFoundError: No module named 'cp0_mac_receiver'`).

- [ ] **Step 3: Implement `config.py` and `server.py` in `mac-receiver/cp0_mac_receiver/`**

- `config.py`: `MacConfig` dataclass with default `dest_root = str(Path.home() / "Pictures" / "CardputerZero")`, `listen_host = "0.0.0.0"`, `listen_port = 8765`, `notify_on_batch = True`, `tailscale_only = True`; atomic `.tmp` + `os.replace` writes; safe fallback on missing/corrupt JSON; default path `~/.config/cp0-mac-sync/config.json` (overrideable via `CP0_MAC_CONFIG`).
- `server.py`: `is_allowed_client_ip()` using Python `ipaddress` against `100.64.0.0/10`, `fd7a:115c:a1e0::/48`, `127.0.0.0/8`, `::1/128` (handling IPv4-mapped IPv6 `::ffff:`); `MacReceiverServer(ThreadingHTTPServer)` reloading config per request so `set-dir` takes effect immediately, streaming `64 KB` chunks to `.cp0-<uuid>.part`, validating `X-CP0-SHA256` and path segments (no `..` / absolute paths after `unquote`), deduplicating identical SHA-256, renaming collisions to `<stem>_<YYYYMMDD_HHMMSS>_<sha8><ext>`, applying `os.utime`, and triggering a debounced `osascript` batch notification.

- [ ] **Step 4: Run tests to verify they pass**

Run: `PYTHONPATH=mac-receiver python3 -m unittest discover -s mac-receiver/tests -v`
Expected: PASS (all tests in `test_server.py` OK).

- [ ] **Step 5: Commit**

```bash
git add .gitignore mac-receiver/cp0_mac_receiver/__init__.py mac-receiver/cp0_mac_receiver/config.py mac-receiver/cp0_mac_receiver/server.py mac-receiver/tests/test_server.py
git commit -m "feat(mac-receiver): add config store, Tailscale IP filter, and atomic HTTP receiver"
```

---

### Task 2: Mac CLI (`cp0-mac-sync`) & `LaunchAgent` Service Installer

**Files:**
- Create: `mac-receiver/cp0_mac_receiver/cli.py`
- Create: `mac-receiver/bin/cp0-mac-receiver`
- Create: `mac-receiver/bin/cp0-mac-sync`
- Test: `mac-receiver/tests/test_cli.py`

**Interfaces:**
- Consumes: `MacConfig`, `load_mac_config`, `save_mac_config` from `cp0_mac_receiver.config`
- Produces:
  - `LAUNCH_AGENT_LABEL = "com.cardputerzero.photosync"`
  - `render_launch_agent_plist(receiver_bin: Path, log_path: Path, config_path: Path) -> str`
  - `run_cli(argv: list[str], config_path: Path | None = None, plist_path: Path | None = None, runner: Callable[..., CompletedProcess] | None = None) -> int`

- [ ] **Step 1: Write failing tests for `cp0-mac-sync` subcommands (`set-dir`, `set-port`, `install-service`, `uninstall-service`, `status`, `logs`)**

```python
# mac-receiver/tests/test_cli.py
import tempfile, unittest
from pathlib import Path
from subprocess import CompletedProcess

from cp0_mac_receiver.cli import run_cli
from cp0_mac_receiver.config import load_mac_config

class TestMacCli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg_path = Path(self.tmp.name) / "config.json"
        self.plist_path = Path(self.tmp.name) / "com.cardputerzero.photosync.plist"
        self.calls = []

    def tearDown(self):
        self.tmp.cleanup()

    def _fake_runner(self, cmd, **kwargs):
        self.calls.append(cmd)
        return CompletedProcess(cmd, 0, stdout="", stderr="")

    def test_set_dir_creates_directory_and_updates_config(self):
        new_dir = Path(self.tmp.name) / "Photos" / "CP0"
        rc = run_cli(["set-dir", str(new_dir)], config_path=self.cfg_path, plist_path=self.plist_path, runner=self._fake_runner)
        self.assertEqual(rc, 0)
        self.assertTrue(new_dir.is_dir())
        self.assertEqual(load_mac_config(self.cfg_path).dest_root, str(new_dir.resolve()))

    def test_install_and_uninstall_service_writes_and_removes_plist(self):
        rc = run_cli(["install-service"], config_path=self.cfg_path, plist_path=self.plist_path, runner=self._fake_runner)
        self.assertEqual(rc, 0)
        plist_text = self.plist_path.read_text()
        self.assertIn("com.cardputerzero.photosync", plist_text)
        self.assertIn("KeepAlive", plist_text)
        self.assertTrue(any("launchctl" in c[0] for c in self.calls))

        rc2 = run_cli(["uninstall-service"], config_path=self.cfg_path, plist_path=self.plist_path, runner=self._fake_runner)
        self.assertEqual(rc2, 0)
        self.assertFalse(self.plist_path.exists())

    def test_set_port_validates_range_and_restarts_service(self):
        run_cli(["install-service"], config_path=self.cfg_path, plist_path=self.plist_path, runner=self._fake_runner)
        self.calls.clear()
        self.assertNotEqual(run_cli(["set-port", "99999"], config_path=self.cfg_path, plist_path=self.plist_path, runner=self._fake_runner), 0)
        rc = run_cli(["set-port", "9100"], config_path=self.cfg_path, plist_path=self.plist_path, runner=self._fake_runner)
        self.assertEqual(rc, 0)
        self.assertEqual(load_mac_config(self.cfg_path).listen_port, 9100)
        self.assertTrue(any("kickstart" in " ".join(c) or "bootstrap" in " ".join(c) for c in self.calls))
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `PYTHONPATH=mac-receiver python3 -m unittest discover -s mac-receiver/tests -v`
Expected: FAIL (`ModuleNotFoundError: No module named 'cp0_mac_receiver.cli'`).

- [ ] **Step 3: Implement `cli.py` and executable entrypoints `bin/cp0-mac-receiver` and `bin/cp0-mac-sync`**

- `cli.py`: `argparse` subcommands `install-service`, `uninstall-service`, `set-dir <path>`, `set-port <port>`, `status`, `logs [--follow]`; generates plist via `plistlib.dumps`, manages `launchctl bootout`/`bootstrap`/`kickstart` for `gui/<uid>`, queries `tailscale status --json` (optional best-effort) and local `/health` in `status`.
- `bin/cp0-mac-receiver`: `#!/usr/bin/env python3` entrypoint setting up rotating logger (`~/Library/Logs/cp0-mac-sync.log`, 5 MB × 2 backups) and running `MacReceiverServer`.
- `bin/cp0-mac-sync`: `#!/usr/bin/env python3` entrypoint invoking `run_cli(sys.argv[1:])`, `chmod +x` both scripts.

- [ ] **Step 4: Run tests to verify they pass**

Run: `PYTHONPATH=mac-receiver python3 -m unittest discover -s mac-receiver/tests -v`
Expected: PASS (`test_server.py` and `test_cli.py` all OK).

- [ ] **Step 5: Commit**

```bash
git add mac-receiver/cp0_mac_receiver/cli.py mac-receiver/bin/cp0-mac-receiver mac-receiver/bin/cp0-mac-sync mac-receiver/tests/test_cli.py
git commit -m "feat(mac-receiver): add cp0-mac-sync CLI and LaunchAgent management"
```

---

### Task 3: On-Device Config & SQLite Queue Store (`device-daemon/`)

**Files:**
- Create: `device-daemon/cp0_syncd/__init__.py`
- Create: `device-daemon/cp0_syncd/config.py`
- Create: `device-daemon/cp0_syncd/store.py`
- Test: `device-daemon/tests/test_store_and_config.py`

**Interfaces:**
- Produces:
  - `WatchedFolder(id: str, path: str, label: str, enabled: bool)`
  - `DaemonConfig(target_host: str, target_port: int, theme: str, watched_folders: list[WatchedFolder], extensions: list[str], ignore_prefixes: list[str], ignore_suffixes: list[str])`
  - `load_daemon_config(path: Path | None = None, home_dir: Path | None = None) -> DaemonConfig`
  - `save_daemon_config(cfg: DaemonConfig, path: Path | None = None) -> None`
  - `add_watched_folder(cfg: DaemonConfig, folder_path: str, label: str | None = None) -> tuple[DaemonConfig, WatchedFolder]` (raises `ValueError` on overlap or non-existent dir; auto-disambiguates duplicate labels)
  - `remove_watched_folder(cfg: DaemonConfig, folder_id: str) -> DaemonConfig`
  - `SyncStore(db_path: Path)` with WAL mode, `enqueue_file(folder_id, abs_path, rel_path, size_bytes, mtime_ns, sha256) -> str`, `next_pending() -> dict | None`, `mark_uploading(file_id)`, `mark_synced(file_id)`, `mark_retry(file_id, err)`, `mark_failed(file_id, err)`, `delete_file(file_id)`, `purge_pending_for_folder(folder_id)`, `retry_all_failed() -> int`, `log(level, message)` (pruned to 500 rows), `set_meta(key, value)`, `get_status_snapshot(watched_folders) -> dict`, `is_media_file(path: Path, cfg: DaemonConfig) -> bool`

- [ ] **Step 1: Write failing tests for default config, label disambiguation, overlap rejection, zero-byte skipping, WAL SQLite dedup, and 500-row log cap**

```python
# device-daemon/tests/test_store_and_config.py
import tempfile, unittest
from pathlib import Path

from cp0_syncd.config import (
    DaemonConfig, add_watched_folder, load_daemon_config, save_daemon_config,
)
from cp0_syncd.store import SyncStore, compute_sha256, is_media_file

class TestDaemonConfig(unittest.TestCase):
    def test_first_run_creates_default_camera_folder_and_theme(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / "home"
            cfg_path = Path(tmp) / "config.json"
            cfg = load_daemon_config(cfg_path, home_dir=home)
            self.assertEqual(cfg.theme, "dark")
            self.assertEqual(cfg.target_port, 8765)
            self.assertEqual(len(cfg.watched_folders), 1)
            self.assertEqual(cfg.watched_folders[0].path, str(home / "Pictures" / "DCIM" / "Camera"))
            self.assertEqual(cfg.watched_folders[0].label, "Camera")

    def test_corrupt_config_falls_back_to_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "config.json"
            cfg_path.write_text("{corrupt")
            cfg = load_daemon_config(cfg_path, home_dir=Path(tmp))
            self.assertEqual(cfg.theme, "dark")

    def test_overlap_rejected_and_duplicate_labels_disambiguated(self):
        with tempfile.TemporaryDirectory() as tmp:
            d1 = Path(tmp) / "a" / "Camera"
            d2 = Path(tmp) / "b" / "Camera"
            child = d1 / "sub"
            d1.mkdir(parents=True); d2.mkdir(parents=True); child.mkdir()
            cfg = DaemonConfig(watched_folders=[])
            cfg, wf1 = add_watched_folder(cfg, str(d1))
            cfg, wf2 = add_watched_folder(cfg, str(d2))
            self.assertEqual(wf1.label, "Camera")
            self.assertEqual(wf2.label, "Camera-2")
            with self.assertRaises(ValueError):
                add_watched_folder(cfg, str(child))

class TestSyncStore(unittest.TestCase):
    def test_dedup_zero_byte_skip_and_log_cap(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = SyncStore(Path(tmp) / "state.db")
            img = Path(tmp) / "IMG_0001.jpg"
            img.write_bytes(b"photo-bytes")
            zero = Path(tmp) / "empty.jpg"
            zero.write_bytes(b"")
            cfg = DaemonConfig()

            self.assertTrue(is_media_file(img, cfg))
            self.assertFalse(is_media_file(zero, cfg))

            sha = compute_sha256(img)
            self.assertEqual(db.enqueue_file("cam", str(img), "IMG_0001.jpg", 11, 1, sha), "enqueued")
            row = db.next_pending()
            db.mark_synced(row["id"])
            self.assertEqual(db.enqueue_file("cam", str(img), "IMG_0001.jpg", 11, 1, sha), "already_synced")

            for i in range(550):
                db.log("INFO", f"msg {i}")
            self.assertEqual(db.log_count(), 500)
            db.close()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `PYTHONPATH=device-daemon python3 -m unittest discover -s device-daemon/tests -v`
Expected: FAIL (`ModuleNotFoundError: No module named 'cp0_syncd'`).

- [ ] **Step 3: Implement `config.py` and `store.py` in `device-daemon/cp0_syncd/`**

- `config.py`: `WatchedFolder` + `DaemonConfig` dataclasses, atomic save (`.tmp` + `os.replace`), safe fallback on corrupt JSON, first-run default `<home>/Pictures/DCIM/Camera` (label `"Camera"`), canonical `realpath` ancestor/descendant overlap detection in `add_watched_folder`, and `Camera-2` label disambiguation.
- `store.py`: `sqlite3` connection in `WAL` mode (`busy_timeout=5000`), `files` table with unique index on `(folder_id, rel_path, sha256)`, `logs` table pruned to newest 500 rows, `meta` key-value table, and `is_media_file()` filtering extensions, hidden/temp prefixes/suffixes, and `0`-byte files.

- [ ] **Step 4: Run tests to verify they pass**

Run: `PYTHONPATH=device-daemon python3 -m unittest discover -s device-daemon/tests -v`
Expected: PASS (`test_store_and_config.py` OK).

- [ ] **Step 5: Commit**

```bash
git add device-daemon/cp0_syncd/__init__.py device-daemon/cp0_syncd/config.py device-daemon/cp0_syncd/store.py device-daemon/tests/test_store_and_config.py
git commit -m "feat(device-daemon): add daemon config, overlap guard, and SQLite WAL queue store"
```

---

### Task 4: On-Device `inotify` Watcher, Wake-Probe Uploader, & Unix Socket IPC

**Files:**
- Create: `device-daemon/cp0_syncd/watcher.py`
- Create: `device-daemon/cp0_syncd/uploader.py`
- Create: `device-daemon/cp0_syncd/ipc.py`
- Create: `device-daemon/cp0_syncd/daemon.py`
- Create: `device-daemon/bin/cp0-syncd`
- Test: `device-daemon/tests/test_daemon_integration.py`

**Interfaces:**
- Consumes: `DaemonConfig`, `load_daemon_config`, `save_daemon_config`, `add_watched_folder`, `remove_watched_folder`, `SyncStore`, `compute_sha256`, `is_media_file` from Task 3
- Produces:
  - `scan_folder(folder: WatchedFolder, cfg: DaemonConfig, store: SyncStore) -> int`
  - `InotifyWatcher(on_file_ready: Callable[[WatchedFolder, Path], None], on_dir_created: Callable[[WatchedFolder, Path], None] | None = None)` (uses `ctypes` `libc.so.6` on Linux with graceful `PollingWatcher` fallback for non-Linux/macOS unit tests; tracks nearest existing ancestor for unmounted paths)
  - `Uploader(store: SyncStore, get_config: Callable[[], DaemonConfig], wake_event: threading.Event, probe_timeout: float = 3.0)` with `process_once() -> str` (backoff sequence `[5, 10, 20, 40, 60]`)
  - `IpcServer(sock_path: Path, handler: Callable[[dict], dict])` (unlinks stale socket before bind, newline-delimited JSON)
  - `SyncDaemon` orchestrating watcher + uploader + IPC server for `bin/cp0-syncd`

- [ ] **Step 1: Write failing integration tests covering sleep/wake retry, missing-file purge, 403/500 vs 413 handling, stale socket cleanup, IPC mutations, and end-to-end upload to `MacReceiverServer`**

```python
# device-daemon/tests/test_daemon_integration.py
import hashlib, json, socket, tempfile, threading, unittest
from pathlib import Path

from cp0_syncd.config import DaemonConfig, WatchedFolder, save_daemon_config
from cp0_syncd.daemon import SyncDaemon
from cp0_syncd.ipc import IpcServer
from cp0_syncd.store import SyncStore

class TestDaemonIntegration(unittest.TestCase):
    def test_stale_socket_unlinked_and_ipc_commands_work(self):
        with tempfile.TemporaryDirectory() as tmp:
            sock_path = Path(tmp) / "cp0-sync.sock"
            sock_path.write_text("stale")
            srv = IpcServer(sock_path, lambda req: {"ok": True, "cmd": req["cmd"]})
            srv.start()
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
                    c.connect(str(sock_path))
                    c.sendall(b'{"cmd":"status"}\n')
                    resp = json.loads(c.makefile().readline())
                    self.assertEqual(resp, {"ok": True, "cmd": "status"})
            finally:
                srv.stop()

    def test_sleep_wake_end_to_end_and_missing_file_cleanup(self):
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mac-receiver"))
        from cp0_mac_receiver.config import MacConfig, save_mac_config
        from cp0_mac_receiver.server import MacReceiverServer

        with tempfile.TemporaryDirectory() as tmp:
            cam_dir = Path(tmp) / "Camera"
            dest_root = Path(tmp) / "MacDest"
            cam_dir.mkdir()
            p1 = cam_dir / "IMG_1.jpg"
            p2 = cam_dir / "IMG_2.jpg"
            p1.write_bytes(b"real-photo-1")
            p2.write_bytes(b"real-photo-2")

            # Allocate ephemeral unused port, then close before starting MacReceiverServer later
            probe_sock = socket.socket()
            probe_sock.bind(("127.0.0.1", 0))
            port = probe_sock.getsockname()[1]
            probe_sock.close()

            cfg_path = Path(tmp) / "daemon.json"
            db_path = Path(tmp) / "state.db"
            sock_path = Path(tmp) / "cp0.sock"
            save_daemon_config(DaemonConfig(
                target_host="127.0.0.1",
                target_port=port,
                watched_folders=[WatchedFolder(id="cam", path=str(cam_dir), label="Camera")],
            ), cfg_path)

            daemon = SyncDaemon(cfg_path, db_path, sock_path)
            daemon.scan_all()
            daemon.uploader.process_once()
            self.assertEqual(daemon.store.get_meta("mac_online"), "false")

            # Delete IMG_2.jpg while Mac is sleeping, then wake Mac receiver
            p2.unlink()
            mac_cfg = Path(tmp) / "mac.json"
            save_mac_config(MacConfig(dest_root=str(dest_root), listen_host="127.0.0.1", listen_port=port, notify_on_batch=False, tailscale_only=True), mac_cfg)
            srv = MacReceiverServer(mac_cfg)
            t = threading.Thread(target=srv.serve_forever, daemon=True)
            t.start()
            try:
                daemon.uploader.process_once()
                self.assertEqual(daemon.store.get_meta("mac_online"), "true")
                self.assertEqual((dest_root / "Camera" / "IMG_1.jpg").read_bytes(), b"real-photo-1")
                self.assertFalse((dest_root / "Camera" / "IMG_2.jpg").exists())
                self.assertIsNone(daemon.store.next_pending())
            finally:
                srv.shutdown()
                srv.server_close()
                daemon.store.close()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `PYTHONPATH=device-daemon:mac-receiver python3 -m unittest discover -s device-daemon/tests -v`
Expected: FAIL (`ModuleNotFoundError: No module named 'cp0_syncd.watcher'`).

- [ ] **Step 3: Implement `watcher.py`, `uploader.py`, `ipc.py`, `daemon.py`, and `bin/cp0-syncd`**

- `watcher.py`: Recursive `scan_folder()` (skips if path missing); `InotifyWatcher` via `ctypes` `libc.so.6` (`inotify_init1`, `inotify_add_watch`, `inotify_rm_watch`, `IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF`) attaching watches to subdirectories dynamically and tracking nearest existing ancestor when a configured SD card mount is missing, plus a clean `PollingWatcher` fallback when not on Linux.
- `uploader.py`: `GET /health` probe (`3s` timeout) caching `mac_dest_root` in `meta`; FIFO `POST /upload` streaming via `http.client` / `urllib` with percent-encoded `X-CP0-Folder-Label` & `X-CP0-Rel-Path`; catches `FileNotFoundError` to remove vanished files cleanly; keeps `PENDING` + pauses queue on network/`5xx`/`401`/`403`; marks `FAILED` only on `400`/`413`/`422`; adaptive backoff `[5, 10, 20, 40, 60]` interrupted immediately by `wake_event`.
- `ipc.py` & `daemon.py`: Unix stream server (unlinks existing socket before `bind()`), implements `status`, `sync_now`, `add_folder`, `remove_folder`, `set_target`, `set_theme`, `retry_failed`.
- `bin/cp0-syncd`: `#!/usr/bin/env python3` entrypoint handling `SIGTERM`/`SIGINT` cleanly.

- [ ] **Step 4: Run all Python tests across both `mac-receiver` and `device-daemon`**

Run: `PYTHONPATH=device-daemon:mac-receiver python3 -m unittest discover -s device-daemon/tests -v && PYTHONPATH=mac-receiver python3 -m unittest discover -s mac-receiver/tests -v`
Expected: PASS (all daemon and Mac receiver tests OK).

- [ ] **Step 5: Commit**

```bash
git add device-daemon/cp0_syncd/watcher.py device-daemon/cp0_syncd/uploader.py device-daemon/cp0_syncd/ipc.py device-daemon/cp0_syncd/daemon.py device-daemon/bin/cp0-syncd device-daemon/tests/test_daemon_integration.py
git commit -m "feat(device-daemon): add inotify watcher, adaptive wake uploader, and IPC daemon"
```

---

### Task 5: Companion UI IPC Client, Tailscale Peer Discovery, & `shadcn/ui` Zinc Theme Tokens

**Files:**
- Create: `device-ui/CMakeLists.txt`
- Create: `device-ui/src/ipc_client.hpp`
- Create: `device-ui/src/ipc_client.cpp`
- Create: `device-ui/src/theme.hpp`
- Create: `device-ui/src/theme.cpp`
- Test: `device-ui/tests/test_ipc_and_theme.cpp`

**Interfaces:**
- Consumes: Unix socket JSON protocol from Task 4 (`status`, `sync_now`, `add_folder`, `remove_folder`, `set_target`, `set_theme`, `retry_failed`) and SQLite WAL fallback `~/.local/share/cp0-sync/state.db`
- Produces:
  - `struct DaemonStatus`, `struct FolderStatus`, `struct LogEntry`, `struct TailscalePeer`
  - `class IpcClient` (`fetch_status()`, `send_command(json)`, `read_sqlite_fallback()`, `parse_tailscale_peers(json_str)`, `list_tailscale_peers()`)
  - `enum class ThemeMode { Dark, Light }`, `struct ShadcnPalette` (exact hex tokens from Spec Section 5.4), `get_palette(ThemeMode)`, `toggle_theme(ThemeMode)`

- [ ] **Step 1: Write failing C++ tests for IPC JSON parsing, SQLite fallback reading, Tailscale peer sorting (macOS/Darwin first, self excluded), and Zinc Dark/Light palette tokens**

```cpp
// device-ui/tests/test_ipc_and_theme.cpp
#include "ipc_client.hpp"
#include "theme.hpp"
#include <cassert>

int main() {
    auto dark = cp0::get_palette(cp0::ThemeMode::Dark);
    auto light = cp0::get_palette(cp0::ThemeMode::Light);
    assert(dark.background == 0x09090b && dark.border == 0x27272a && dark.foreground == 0xfafafa);
    assert(light.background == 0xfafafa && light.card == 0xffffff && light.border == 0xe4e4e7 && light.ring == 0x71717a);
    assert(cp0::toggle_theme(cp0::ThemeMode::Dark) == cp0::ThemeMode::Light);

    const char* ts_json = R"({"Self":{"ID":"self1"},"Peer":{"p1":{"HostName":"linux-box","DNSName":"linux.ts.net.","OS":"linux","Online":true,"TailscaleIPs":["100.64.0.2"]},"p2":{"HostName":"macbook","DNSName":"macbook.ts.net.","OS":"macOS","Online":true,"TailscaleIPs":["100.64.0.3"]}}})";
    auto peers = cp0::parse_tailscale_peers(ts_json);
    assert(peers.size() == 2);
    assert(peers[0].hostname == "macbook" && peers[0].os == "macOS");
    return 0;
}
```
*(Include tests for `IpcClient` round-tripping against a real temporary `AF_UNIX` server socket and reading fallback rows from a temporary `sqlite3` DB.)*

- [ ] **Step 2: Configure CMake and run tests to verify failure**

Run: `cmake -S device-ui -B device-ui/build -DCP0_UI_BUILD_TESTS=ON -DCP0_UI_BUILD_APP=OFF && cmake --build device-ui/build`
Expected: FAIL (source files do not exist yet).

- [ ] **Step 3: Implement `CMakeLists.txt`, `theme.hpp/.cpp`, and `ipc_client.hpp/.cpp`**

- `CMakeLists.txt`: C++17 project `cp0-sync-ui`; options `CP0_UI_BUILD_TESTS` (default `ON`) and `CP0_UI_BUILD_APP` (default `ON`); links `SQLite::SQLite3` for core library (`cp0_ui_core`); when `CP0_UI_BUILD_APP=ON`, links LVGL v9 (`FetchContent` or local `LVGL_DIR`), FreeType2, and SDL2 (macOS/sim) or Linux `fbdev`/`evdev` (`CP0_UI_TARGET_DEVICE=ON`).
- `theme.hpp/.cpp`: Exact `ShadcnPalette` color tables for Zinc Dark and balanced Zinc Light matching Spec Section 5.4.
- `ipc_client.hpp/.cpp`: Non-blocking `AF_UNIX` client (`1s` timeout), minimal dependency-free JSON parser/serializer (or single-header `nlohmann/json.hpp` vendored or small parser—keep zero external network/pip requirements), `sqlite3` read-only fallback (`SQLITE_OPEN_READONLY`), and `tailscale status --json` peer extraction prioritizing `macOS`/`darwin` online peers at top.

- [ ] **Step 4: Build and run `ctest` to verify tests pass**

Run: `cmake -S device-ui -B device-ui/build -DCP0_UI_BUILD_TESTS=ON -DCP0_UI_BUILD_APP=OFF && cmake --build device-ui/build && ctest --test-dir device-ui/build --output-on-failure`
Expected: PASS (`test_ipc_and_theme` passes).

- [ ] **Step 5: Commit**

```bash
git add device-ui/CMakeLists.txt device-ui/src/ipc_client.hpp device-ui/src/ipc_client.cpp device-ui/src/theme.hpp device-ui/src/theme.cpp device-ui/tests/test_ipc_and_theme.cpp
git commit -m "feat(device-ui): add C++ IPC client, Tailscale peer parser, and shadcn Zinc themes"
```

---

### Task 6: Companion UI 320×170 LVGL Views, Modals, & Keyboard Controller

**Files:**
- Create: `device-ui/src/ui_controller.hpp`
- Create: `device-ui/src/ui_controller.cpp`
- Create: `device-ui/src/app_ui.hpp`
- Create: `device-ui/src/app_ui.cpp`
- Create: `device-ui/src/main.cpp`
- Test: `device-ui/tests/test_ui_navigation.cpp`

**Interfaces:**
- Consumes: `IpcClient`, `DaemonStatus`, `ThemeMode`, `ShadcnPalette`, `get_palette`, `toggle_theme` from Task 5
- Produces:
  - `enum class ActiveTab { Status = 0, Folders = 1, Logs = 2 }`
  - `enum class ModalKind { None, TargetPicker, DirPicker, ConfirmRemove, TextInput }`
  - `class UiController`: pure state/keyboard state machine handling `1`/`2`/`3`, `Enter`, `T`, `C`, `A`, `D`/`Delete`, `R`, `j`/`k`/arrow keys, `Space`, `Backspace`, `Esc`/`Q`, directory traversal, and emitting `IpcAction` requests
  - `class LvglAppView` (`app_ui.hpp/.cpp`): 320×170 LVGL v9 Bento card + segmented `TabsList` renderer with live `apply_theme(ThemeMode)` switching and FreeType font loading (`Inter` + `JetBrains Mono`, falling back to LVGL built-in Montserrat/Unscii if `.ttf` absent)
  - `cp0-sync-ui` executable (`main.cpp`) supporting `--sim` / macOS SDL2 window (`320×170`) and Linux `fbdev` + `evdev` (`cp0-keys`)

- [ ] **Step 1: Write failing C++ tests for keyboard state transitions (`1/2/3`, `[T]` theme toggle outside vs inside modal, `[A]` directory drill-down & `Space` watch, `[D]` confirm Y/N, `[C]` peer selection, `[R]` retry failed, `Esc`/`Q`)**

```cpp
// device-ui/tests/test_ui_navigation.cpp
#include "ui_controller.hpp"
#include <cassert>
#include <filesystem>

int main() {
    cp0::UiController ctrl;
    assert(ctrl.state().tab == cp0::ActiveTab::Status);
    assert(ctrl.state().theme == cp0::ThemeMode::Dark);

    // 'T' toggles theme on main tab, emits SetTheme action
    auto action = ctrl.handle_key('t', cp0::KeyCode::Char);
    assert(ctrl.state().theme == cp0::ThemeMode::Light);
    assert(action.type == cp0::ActionType::SetTheme);

    // '2' jumps to Folders, 'A' opens DirPicker, 'T' inside modal opens TextInput instead of toggling theme
    ctrl.handle_key('2', cp0::KeyCode::Char);
    assert(ctrl.state().tab == cp0::ActiveTab::Folders);
    ctrl.handle_key('a', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::DirPicker);
    auto modal_t = ctrl.handle_key('t', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::TextInput);
    assert(ctrl.state().theme == cp0::ThemeMode::Light); // unchanged inside modal
    (void)modal_t;

    // 'Esc' closes modal, 'Esc' again requests Quit
    ctrl.handle_key(0, cp0::KeyCode::Escape);
    assert(ctrl.state().modal == cp0::ModalKind::None);
    auto quit_act = ctrl.handle_key(0, cp0::KeyCode::Escape);
    assert(quit_act.type == cp0::ActionType::Quit);
    return 0;
}
```
*(Also test `[D]` -> `[Y]` folder removal confirmation and `DirPicker` `Enter` / `Backspace` / `Space` directory selection against a `std::filesystem::temp_directory_path()` folder tree.)*

- [ ] **Step 2: Build and run `ctest` to verify failure**

Run: `cmake --build device-ui/build && ctest --test-dir device-ui/build --output-on-failure`
Expected: FAIL (`ui_controller.hpp` missing).

- [ ] **Step 3: Implement `ui_controller.hpp/.cpp`, `app_ui.hpp/.cpp`, `main.cpp`, and verify SDL2 simulator build**

- Implement `UiController` cleanly separated from LVGL objects so every keyboard shortcut and modal flow is unit-tested in `ctest`.
- Implement `LvglAppView` creating the 320×170 widget hierarchy matching Spec Section 5.4 (`Top bar 24px`, `Main content 126px`, `Footer 20px` with `<kbd>` keycaps), wiring `lv_style_t` styles for Zinc Dark and Zinc Light so `[T]` swaps styles cleanly without recreating the screen, and loading FreeType fonts (`Inter` + `JetBrains Mono`) from `/usr/share/cp0-sync/fonts/`, `/usr/share/fonts/`, or `assets/fonts/` when present, with graceful LVGL built-in Montserrat/Unscii fallback when TTFs are absent.
- Wire `main.cpp` using `lv_sdl_window_create(320, 170)` + `lv_sdl_keyboard_create()` on macOS/desktop simulator and `lv_linux_fbdev_create()` + `lv_evdev_create()` on CardputerZero Linux, polling `IpcClient::fetch_status()` every `1000 ms` via `lv_timer_create`.

- [ ] **Step 4: Build both `ctest` unit tests and full SDL2 app target (`CP0_UI_BUILD_APP=ON`)**

Run: `cmake -S device-ui -B device-ui/build -DCP0_UI_BUILD_TESTS=ON -DCP0_UI_BUILD_APP=ON && cmake --build device-ui/build && ctest --test-dir device-ui/build --output-on-failure`
Expected: PASS (`test_ipc_and_theme` and `test_ui_navigation` pass; `cp0-sync-ui` binary links cleanly).

- [ ] **Step 5: Commit**

```bash
git add device-ui/CMakeLists.txt device-ui/src/ui_controller.hpp device-ui/src/ui_controller.cpp device-ui/src/app_ui.hpp device-ui/src/app_ui.cpp device-ui/src/main.cpp device-ui/tests/test_ui_navigation.cpp
git commit -m "feat(device-ui): add 320x170 LVGL shadcn Bento UI, modals, and keyboard controller"
```

---

### Task 7: Debian (`arm64` `.deb`) Packaging, `APPLaunch` Integration, & `systemd --user` Unit

**Files:**
- Create: `packaging/cp0-syncd.service`
- Create: `packaging/cp0-sync.desktop`
- Create: `packaging/cp0-sync.png` (generated cleanly via Python `struct`/`zlib` PNG writer or SVG→PNG so zero extra assets needed)
- Create: `packaging/debian/control`
- Create: `packaging/debian/postinst`
- Create: `packaging/debian/prerm`
- Create: `packaging/build-deb.sh`
- Create: `packaging/Dockerfile.arm64`
- Test: `packaging/tests/test_packaging.py`

**Interfaces:**
- Consumes: `device-daemon/bin/cp0-syncd`, `device-daemon/cp0_syncd/`, `device-ui/` CMake build
- Produces: `cp0-sync_1.0.0_m5stack1_arm64.deb` containing `/usr/bin/cp0-sync-ui`, `/usr/bin/cp0-syncd`, `/usr/lib/cp0-sync/cp0_syncd/...`, `/usr/lib/systemd/user/cp0-syncd.service`, `/usr/share/APPLaunch/applications/cp0-sync.desktop`, `/usr/share/APPLaunch/share/images/cp0-sync.png`

- [ ] **Step 1: Write failing validation tests verifying packaging metadata, systemd user unit paths, desktop entry fields, maintainer script permissions/hooks, and `cp0-sync.png` validity**

```python
# packaging/tests/test_packaging.py
import struct, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class TestPackagingAssets(unittest.TestCase):
    def test_systemd_user_unit_and_desktop_entry(self):
        unit = (ROOT / "packaging" / "cp0-syncd.service").read_text()
        self.assertIn("ExecStart=/usr/bin/cp0-syncd", unit)
        self.assertIn("WantedBy=default.target", unit)

        desktop = (ROOT / "packaging" / "cp0-sync.desktop").read_text()
        self.assertIn("Exec=/usr/bin/cp0-sync-ui", desktop)
        self.assertIn("Icon=/usr/share/APPLaunch/share/images/cp0-sync.png", desktop)

    def test_debian_control_and_maintainer_scripts(self):
        ctrl = (ROOT / "packaging" / "debian" / "control").read_text()
        self.assertIn("Package: cp0-sync", ctrl)
        self.assertIn("Architecture: arm64", ctrl)
        self.assertIn("python3 (>= 3.9)", ctrl)

        postinst = (ROOT / "packaging" / "debian" / "postinst").read_text()
        self.assertIn("systemctl --global enable cp0-syncd.service", postinst)

        png = (ROOT / "packaging" / "cp0-sync.png").read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest discover -s packaging/tests -v`
Expected: FAIL (`FileNotFoundError`).

- [ ] **Step 3: Create packaging files, icon PNG, maintainer scripts, and ARM64 Docker `.deb` build script**

- `packaging/cp0-syncd.service`: `systemd --user` service (`Restart=on-failure`, `RestartSec=5`, `ExecStart=/usr/bin/cp0-syncd`, `WantedBy=default.target`).
- `packaging/cp0-sync.desktop`: `Type=Application`, `Name=Photo Sync`, `Exec=/usr/bin/cp0-sync-ui`, `Icon=/usr/share/APPLaunch/share/images/cp0-sync.png`, `Terminal=false`.
- `packaging/cp0-sync.png`: crisp 64×64 zinc/white camera-sync icon PNG.
- `packaging/debian/control`, `postinst` (`chmod 0755`, runs `systemctl --global enable cp0-syncd.service` and starts active user units safely with `|| true`), `prerm` (`chmod 0755`, stops/disables user service on remove).
- `packaging/Dockerfile.arm64` & `packaging/build-deb.sh`: builds `cp0-sync_1.0.0_m5stack1_arm64.deb` in `--platform linux/arm64` Debian Bookworm container (and also supports running directly on CardputerZero over SSH).

- [ ] **Step 4: Run packaging tests (and build `.deb` if Docker `linux/arm64` daemon is available)**

Run: `python3 -m unittest discover -s packaging/tests -v`
Expected: PASS (`test_packaging.py` OK).

- [ ] **Step 5: Commit**

```bash
git add packaging/
git commit -m "feat(packaging): add systemd user unit, APPLaunch entry, and arm64 deb builder"
```
