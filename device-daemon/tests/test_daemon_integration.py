"""Integration and unit tests for cp0_syncd watcher, uploader, IPC, and daemon."""

from __future__ import annotations

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

from cp0_syncd.config import DaemonConfig, WatchedFolder, load_daemon_config, save_daemon_config
from cp0_syncd.daemon import SyncDaemon
from cp0_syncd.ipc import IpcServer, default_sock_path
from cp0_syncd.store import SyncStore, compute_sha256
from cp0_syncd.watcher import (
    IN_CLOSE_WRITE,
    IN_CREATE,
    IN_ISDIR,
    IN_MOVED_TO,
    InotifyWatcher,
    PollingWatcher,
    parse_inotify_events,
    scan_folder,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
MAC_RECEIVER_ROOT = REPO_ROOT / "mac-receiver"
if str(MAC_RECEIVER_ROOT) not in sys.path:
    sys.path.insert(0, str(MAC_RECEIVER_ROOT))

from cp0_mac_receiver.config import MacConfig, save_mac_config  # noqa: E402
from cp0_mac_receiver.server import MacReceiverServer  # noqa: E402


def _unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _send_ipc(sock_path: Path, payload: dict[str, object]) -> dict[str, object]:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(3.0)
        client.connect(str(sock_path))
        client.sendall((json.dumps(payload) + "\n").encode("utf-8"))
        with client.makefile("r", encoding="utf-8") as reader:
            line = reader.readline()
    return json.loads(line)


def _wait_until(predicate: object, timeout: float = 3.0, interval: float = 0.02) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():  # type: ignore[operator]
            return True
        time.sleep(interval)
    return bool(predicate())  # type: ignore[operator]


def _pack_inotify_event(wd: int, mask: int, cookie: int, name: bytes) -> bytes:
    if name:
        padded_len = ((len(name) + 1 + 15) // 16) * 16
        padded = name + b"\x00" * (padded_len - len(name))
    else:
        padded_len = 0
        padded = b""
    return struct.pack("=iIII", wd, mask, cookie, padded_len) + padded


class _ProgrammableUploadServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, statuses: list[int], health_status: int = 200) -> None:
        self.statuses = list(statuses)
        self.health_status = health_status
        self.upload_requests: list[dict[str, str]] = []
        self.health_delay = 0.0
        super().__init__(("127.0.0.1", 0), _ProgrammableHandler)


class _ProgrammableHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: object) -> None:
        pass

    def _reply_json(self, code: int, payload: dict[str, object]) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        srv: _ProgrammableUploadServer = self.server  # type: ignore[assignment]
        if srv.health_delay > 0:
            time.sleep(srv.health_delay)
        if srv.health_status != HTTPStatus.OK:
            self._reply_json(srv.health_status, {"error": "health_rejected"})
            return
        self._reply_json(
            HTTPStatus.OK,
            {
                "status": "ok",
                "version": "1.0.0",
                "hostname": "mock-mac",
                "dest_root": "/tmp/MockDest",
            },
        )

    def do_POST(self) -> None:
        srv: _ProgrammableUploadServer = self.server  # type: ignore[assignment]
        length = int(self.headers.get("Content-Length", "0"))
        remaining = length
        while remaining > 0:
            chunk = self.rfile.read(min(65536, remaining))
            if not chunk:
                break
            remaining -= len(chunk)

        srv.upload_requests.append(
            {
                "label": self.headers.get("X-CP0-Folder-Label", ""),
                "rel_path": self.headers.get("X-CP0-Rel-Path", ""),
                "sha256": self.headers.get("X-CP0-SHA256", ""),
            }
        )
        code = srv.statuses.pop(0) if srv.statuses else int(HTTPStatus.OK)
        if code == HTTPStatus.OK:
            self._reply_json(code, {"status": "saved"})
        else:
            self._reply_json(code, {"error": f"status_{code}"})


class TestIpcAndEnvDefaults(unittest.TestCase):
    def test_stale_socket_unlinked_and_invalid_json_handled(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sock_path = Path(tmp) / "cp0-sync.sock"
            sock_path.write_text("stale", encoding="utf-8")

            srv = IpcServer(sock_path, lambda req: {"ok": True, "cmd": req["cmd"]})
            srv.start()
            try:
                self.assertEqual(_send_ipc(sock_path, {"cmd": "status"}), {"ok": True, "cmd": "status"})

                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.settimeout(2.0)
                    client.connect(str(sock_path))
                    client.sendall(b"{not-json\n")
                    resp = json.loads(client.makefile("r", encoding="utf-8").readline())
                    self.assertFalse(resp["ok"])
                    self.assertIn("error", resp)
            finally:
                srv.stop()
            self.assertFalse(sock_path.exists())

    def test_default_sock_path_honors_env_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            custom_sock = str(Path(tmp) / "custom.sock")
            with mock.patch.dict(os.environ, {"CP0_SYNC_SOCK": custom_sock}, clear=False):
                self.assertEqual(default_sock_path(), Path(custom_sock))

            env_no_sock = {k: v for k, v in os.environ.items() if k != "CP0_SYNC_SOCK"}
            env_no_sock["XDG_RUNTIME_DIR"] = tmp
            with mock.patch.dict(os.environ, env_no_sock, clear=True):
                self.assertEqual(default_sock_path(), Path(tmp) / "cp0-sync.sock")


class TestUploaderAndEndToEndSync(unittest.TestCase):
    def test_sleep_wake_end_to_end_and_missing_file_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cam_dir = Path(tmp) / "Camera"
            dest_root = Path(tmp) / "MacDest"
            cam_dir.mkdir()
            p1 = cam_dir / "IMG_1.jpg"
            p2 = cam_dir / "IMG_2.jpg"
            p1.write_bytes(b"real-photo-1")
            p2.write_bytes(b"real-photo-2")

            port = _unused_tcp_port()
            cfg_path = Path(tmp) / "daemon.json"
            db_path = Path(tmp) / "state.db"
            sock_path = Path(tmp) / "cp0.sock"
            save_daemon_config(
                DaemonConfig(
                    target_host="127.0.0.1",
                    target_port=port,
                    watched_folders=[WatchedFolder(id="cam", path=str(cam_dir), label="Camera")],
                ),
                cfg_path,
            )

            daemon = SyncDaemon(cfg_path, db_path, sock_path)
            try:
                self.assertEqual(daemon.scan_all(), 2)
                self.assertEqual(daemon.uploader.process_once(), "offline")
                self.assertEqual(daemon.store.get_meta("mac_online"), "false")

                # Delete IMG_2.jpg while Mac is sleeping, then wake Mac receiver.
                p2.unlink()
                mac_cfg = Path(tmp) / "mac.json"
                save_mac_config(
                    MacConfig(
                        dest_root=str(dest_root),
                        listen_host="127.0.0.1",
                        listen_port=port,
                        notify_on_batch=False,
                        tailscale_only=True,
                    ),
                    mac_cfg,
                )
                srv = MacReceiverServer(mac_cfg)
                srv.start()
                try:
                    self.assertEqual(daemon.uploader.process_once(), "synced")
                    self.assertEqual(daemon.store.get_meta("mac_online"), "true")
                    self.assertEqual(daemon.store.get_meta("mac_dest_root"), str(dest_root))
                    self.assertEqual((dest_root / "Camera" / "IMG_1.jpg").read_bytes(), b"real-photo-1")
                    self.assertFalse((dest_root / "Camera" / "IMG_2.jpg").exists())
                    self.assertIsNone(daemon.store.next_pending())
                    logs = daemon.store.list_logs(limit=20)
                    self.assertTrue(any(entry["level"] == "WARN" and "IMG_2.jpg" in entry["message"] for entry in logs))
                finally:
                    srv.shutdown()
                    srv.server_close()
            finally:
                daemon.store.close()

    def test_utf8_and_spaces_in_headers_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            watch_dir = Path(tmp) / "Trip Photos"
            nested_dir = watch_dir / "2026 Vacay"
            dest_root = Path(tmp) / "MacDest"
            nested_dir.mkdir(parents=True)
            photo = nested_dir / "café 1.jpg"
            photo.write_bytes(b"utf8-photo-payload")

            mac_cfg = Path(tmp) / "mac.json"
            save_mac_config(
                MacConfig(
                    dest_root=str(dest_root),
                    listen_host="127.0.0.1",
                    listen_port=0,
                    notify_on_batch=False,
                    tailscale_only=True,
                ),
                mac_cfg,
            )
            srv = MacReceiverServer(mac_cfg)
            srv.start()
            try:
                cfg_path = Path(tmp) / "daemon.json"
                db_path = Path(tmp) / "state.db"
                sock_path = Path(tmp) / "cp0.sock"
                save_daemon_config(
                    DaemonConfig(
                        target_host="127.0.0.1",
                        target_port=srv.server_port,
                        watched_folders=[WatchedFolder(id="trip", path=str(watch_dir), label="Trip Photos")],
                    ),
                    cfg_path,
                )
                daemon = SyncDaemon(cfg_path, db_path, sock_path)
                try:
                    self.assertEqual(daemon.scan_all(), 1)
                    self.assertEqual(daemon.uploader.process_once(), "synced")
                    saved = dest_root / "Trip Photos" / "2026 Vacay" / "café 1.jpg"
                    self.assertTrue(saved.exists())
                    self.assertEqual(saved.read_bytes(), b"utf8-photo-payload")
                finally:
                    daemon.store.close()
            finally:
                srv.shutdown()
                srv.server_close()

    def test_403_and_500_pause_queue_while_400_413_422_fail_only_that_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cam_dir = Path(tmp) / "Camera"
            cam_dir.mkdir()
            f1 = cam_dir / "a.jpg"
            f2 = cam_dir / "b.jpg"
            f3 = cam_dir / "c.jpg"
            f1.write_bytes(b"photo-a")
            f2.write_bytes(b"photo-b")
            f3.write_bytes(b"photo-c")

            srv = _ProgrammableUploadServer([403, 500, 413, 400, 422, 200])
            thread = threading.Thread(target=srv.serve_forever, daemon=True)
            thread.start()
            try:
                cfg_path = Path(tmp) / "daemon.json"
                db_path = Path(tmp) / "state.db"
                sock_path = Path(tmp) / "cp0.sock"
                save_daemon_config(
                    DaemonConfig(
                        target_host="127.0.0.1",
                        target_port=srv.server_port,
                        watched_folders=[WatchedFolder(id="cam", path=str(cam_dir), label="Camera")],
                    ),
                    cfg_path,
                )
                daemon = SyncDaemon(cfg_path, db_path, sock_path)
                try:
                    self.assertEqual(daemon.scan_all(), 3)

                    # 403 pauses queue immediately on a.jpg; b.jpg and c.jpg are not attempted.
                    self.assertEqual(daemon.uploader.process_once(), "paused")
                    self.assertEqual(len(srv.upload_requests), 1)
                    snap = daemon.store.get_status_snapshot(daemon.get_config().watched_folders)
                    self.assertEqual(snap["counts"]["pending"], 3)
                    self.assertEqual(snap["counts"]["failed"], 0)

                    # 500 also pauses queue immediately on a.jpg.
                    self.assertEqual(daemon.uploader.process_once(), "paused")
                    self.assertEqual(len(srv.upload_requests), 2)
                    snap = daemon.store.get_status_snapshot(daemon.get_config().watched_folders)
                    self.assertEqual(snap["counts"]["pending"], 3)
                    self.assertEqual(snap["counts"]["failed"], 0)

                    # Now 413, 400, 422? No, next in srv.statuses is 413, 400, 422!
                    # Let's add a 4th file d.jpg to get the 200 OK after a(413), b(400), c(422).
                    f4 = cam_dir / "d.jpg"
                    f4.write_bytes(b"photo-d")
                    self.assertEqual(daemon.scan_all(), 1)

                    self.assertEqual(daemon.uploader.process_once(), "synced")
                    snap = daemon.store.get_status_snapshot(daemon.get_config().watched_folders)
                    self.assertEqual(snap["counts"]["failed"], 3)
                    self.assertEqual(snap["counts"]["synced"], 1)
                    self.assertEqual(snap["counts"]["pending"], 0)
                finally:
                    daemon.store.close()
            finally:
                srv.shutdown()
                srv.server_close()

    def test_backoff_schedule_reset_and_probe_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cam_dir = Path(tmp) / "Camera"
            cam_dir.mkdir()
            (cam_dir / "a.jpg").write_bytes(b"photo-a")

            dead_port = _unused_tcp_port()
            cfg_path = Path(tmp) / "daemon.json"
            db_path = Path(tmp) / "state.db"
            sock_path = Path(tmp) / "cp0.sock"
            save_daemon_config(
                DaemonConfig(
                    target_host="127.0.0.1",
                    target_port=dead_port,
                    watched_folders=[WatchedFolder(id="cam", path=str(cam_dir), label="Camera")],
                ),
                cfg_path,
            )

            daemon = SyncDaemon(cfg_path, db_path, sock_path, probe_timeout=0.15)
            try:
                daemon.scan_all()
                expected_delays = [5, 10, 20, 40, 60, 60]
                for want_sec in expected_delays:
                    self.assertEqual(daemon.uploader.process_once(), "offline")
                    sec = daemon.uploader.next_retry_sec()
                    self.assertGreaterEqual(sec, want_sec - 1)
                    self.assertLessEqual(sec, want_sec)

                # New photo resets backoff back to 5s on next failure.
                daemon.uploader.notify_new_file()
                self.assertEqual(daemon.uploader.next_retry_sec(), 0)
                self.assertEqual(daemon.uploader.process_once(), "offline")
                self.assertLessEqual(daemon.uploader.next_retry_sec(), 5)
            finally:
                daemon.store.close()

            # Verify probe_timeout aborts a hanging /health response quickly.
            slow_srv = _ProgrammableUploadServer([], health_status=200)
            slow_srv.health_delay = 0.6
            t = threading.Thread(target=slow_srv.serve_forever, daemon=True)
            t.start()
            try:
                save_daemon_config(
                    DaemonConfig(
                        target_host="127.0.0.1",
                        target_port=slow_srv.server_port,
                        watched_folders=[WatchedFolder(id="cam", path=str(cam_dir), label="Camera")],
                    ),
                    cfg_path,
                )
                daemon2 = SyncDaemon(cfg_path, Path(tmp) / "state2.db", Path(tmp) / "s2.sock", probe_timeout=0.15)
                try:
                    daemon2.scan_all()
                    t0 = time.monotonic()
                    self.assertEqual(daemon2.uploader.process_once(), "offline")
                    elapsed = time.monotonic() - t0
                    self.assertLess(elapsed, 0.5)
                finally:
                    daemon2.store.close()
            finally:
                slow_srv.shutdown()
                slow_srv.server_close()


class TestSyncDaemonIpcLifecycle(unittest.TestCase):
    def test_all_ipc_commands_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cam_dir = Path(tmp) / "Camera"
            extra_dir = Path(tmp) / "SDCard"
            dest_root = Path(tmp) / "MacDest"
            cam_dir.mkdir()
            extra_dir.mkdir()
            (extra_dir / "sd_1.jpg").write_bytes(b"sd-photo-1")

            cfg_path = Path(tmp) / "daemon.json"
            db_path = Path(tmp) / "state.db"
            sock_path = Path(tmp) / "cp0.sock"
            save_daemon_config(
                DaemonConfig(
                    target_host="",
                    target_port=8765,
                    theme="dark",
                    watched_folders=[WatchedFolder(id="cam", path=str(cam_dir), label="Camera")],
                ),
                cfg_path,
            )

            mac_cfg = Path(tmp) / "mac.json"
            save_mac_config(
                MacConfig(
                    dest_root=str(dest_root),
                    listen_host="127.0.0.1",
                    listen_port=0,
                    notify_on_batch=False,
                    tailscale_only=True,
                ),
                mac_cfg,
            )
            mac_srv = MacReceiverServer(mac_cfg)
            mac_srv.start()

            daemon = SyncDaemon(cfg_path, db_path, sock_path, watcher_poll_interval=0.05)
            daemon.start()
            try:
                status = _send_ipc(sock_path, {"cmd": "status"})
                self.assertEqual(
                    set(status.keys()),
                    {
                        "ok",
                        "target_host",
                        "target_port",
                        "theme",
                        "mac_online",
                        "mac_dest_root",
                        "syncing",
                        "current_file",
                        "pending_count",
                        "pending_bytes",
                        "synced_count",
                        "failed_count",
                        "next_retry_sec",
                        "last_sync_ts",
                        "last_synced_file",
                        "last_error",
                        "folders",
                        "logs",
                    },
                )
                self.assertTrue(status["ok"])
                self.assertEqual(status["theme"], "dark")
                self.assertFalse(status["mac_online"])

                # set_theme validation + persistence
                bad_theme = _send_ipc(sock_path, {"cmd": "set_theme", "theme": "neon"})
                self.assertFalse(bad_theme["ok"])
                good_theme = _send_ipc(sock_path, {"cmd": "set_theme", "theme": "light"})
                self.assertTrue(good_theme["ok"])
                self.assertEqual(load_daemon_config(cfg_path).theme, "light")

                # add_folder validation + rescan
                overlap = _send_ipc(sock_path, {"cmd": "add_folder", "path": str(cam_dir)})
                self.assertFalse(overlap["ok"])
                added = _send_ipc(sock_path, {"cmd": "add_folder", "path": str(extra_dir), "label": "SD"})
                self.assertTrue(added["ok"])
                sd_folder_id = added["folder"]["id"]  # type: ignore[index]
                self.assertEqual(added["enqueued"], 1)

                # set_target validation + immediate probe + upload
                bad_port = _send_ipc(sock_path, {"cmd": "set_target", "host": "127.0.0.1", "port": 99999})
                self.assertFalse(bad_port["ok"])
                bad_host = _send_ipc(sock_path, {"cmd": "set_target", "host": "", "port": 8765})
                self.assertFalse(bad_host["ok"])

                set_tgt = _send_ipc(
                    sock_path,
                    {"cmd": "set_target", "host": "127.0.0.1", "port": mac_srv.server_port},
                )
                self.assertTrue(set_tgt["ok"])
                self.assertTrue(_wait_until(lambda: (dest_root / "SD" / "sd_1.jpg").exists()))

                status_after = _send_ipc(sock_path, {"cmd": "status"})
                self.assertTrue(status_after["mac_online"])
                self.assertEqual(status_after["mac_dest_root"], str(dest_root))
                self.assertEqual(status_after["synced_count"], 1)
                self.assertEqual(status_after["pending_count"], 0)
                self.assertEqual(status_after["last_synced_file"], "sd_1.jpg")
                self.assertGreater(status_after["last_sync_ts"], 0)
                self.assertEqual(
                    set(status_after["folders"][0].keys()),  # type: ignore[index]
                    {"id", "path", "label", "enabled", "exists", "pending", "synced"},
                )

                # retry_failed flips FAILED -> PENDING and uploads via wake
                failed_file = cam_dir / "retried.jpg"
                failed_file.write_bytes(b"retried-bytes")
                sha = compute_sha256(failed_file)
                daemon.store.enqueue_file("cam", str(failed_file), "retried.jpg", 13, 1, sha)
                row = daemon.store.next_pending()
                assert row is not None
                daemon.store.mark_failed(row["id"], "413")
                retry_resp = _send_ipc(sock_path, {"cmd": "retry_failed"})
                self.assertTrue(retry_resp["ok"])
                self.assertEqual(retry_resp["retried"], 1)
                self.assertTrue(_wait_until(lambda: (dest_root / "Camera" / "retried.jpg").exists()))

                # sync_now picks up newly written photo
                (cam_dir / "manual.jpg").write_bytes(b"manual-bytes")
                sync_resp = _send_ipc(sock_path, {"cmd": "sync_now"})
                self.assertTrue(sync_resp["ok"])
                self.assertTrue(_wait_until(lambda: (dest_root / "Camera" / "manual.jpg").exists()))

                # remove_folder removes config entry and purges pending rows without deleting disk files
                mac_srv.shutdown()
                mac_srv.server_close()
                queued_file = extra_dir / "stay_on_disk.jpg"
                queued_file.write_bytes(b"local-only")
                daemon.store.enqueue_file(
                    sd_folder_id,
                    str(queued_file),
                    "stay_on_disk.jpg",
                    10,
                    1,
                    compute_sha256(queued_file),
                )
                rm_resp = _send_ipc(sock_path, {"cmd": "remove_folder", "folder_id": sd_folder_id})
                self.assertTrue(rm_resp["ok"])
                self.assertEqual(rm_resp["purged"], 1)
                self.assertTrue(queued_file.exists())
                self.assertEqual([wf.id for wf in load_daemon_config(cfg_path).watched_folders], ["cam"])

                # Unknown command returns ok=false
                unknown = _send_ipc(sock_path, {"cmd": "bogus"})
                self.assertFalse(unknown["ok"])
            finally:
                daemon.stop()
                mac_srv.shutdown()
                mac_srv.server_close()


class _FakeLibc:
    def __init__(self) -> None:
        self.next_wd = 1
        self.watches: dict[int, tuple[str, int]] = {}
        self.removed_wds: list[int] = []
        self.init_calls = 0

    def inotify_init1(self, flags: int) -> int:
        self.init_calls += 1
        return 99

    def inotify_add_watch(self, fd: int, path_bytes: bytes, mask: int) -> int:
        wd = self.next_wd
        self.next_wd += 1
        self.watches[wd] = (os.fsdecode(path_bytes), mask)
        return wd

    def inotify_rm_watch(self, fd: int, wd: int) -> int:
        self.removed_wds.append(wd)
        self.watches.pop(wd, None)
        return 0


class TestWatchersAndLinuxInotify(unittest.TestCase):
    def test_parse_linux_inotify_event_buffer_with_utf8(self) -> None:
        buf = (
            _pack_inotify_event(1, IN_CREATE, 0, b"partial.jpg")
            + _pack_inotify_event(1, IN_CLOSE_WRITE, 0, "café.jpg".encode("utf-8"))
            + _pack_inotify_event(2, IN_CREATE | IN_ISDIR, 0, b"2026-11")
            + _pack_inotify_event(3, IN_MOVED_TO, 42, b"moved.png")
        )
        events = parse_inotify_events(buf)
        self.assertEqual(len(events), 4)
        self.assertEqual((events[0].wd, events[0].mask, events[0].name), (1, IN_CREATE, "partial.jpg"))
        self.assertEqual((events[1].wd, events[1].mask, events[1].name), (1, IN_CLOSE_WRITE, "café.jpg"))
        self.assertEqual((events[2].wd, events[2].mask, events[2].name), (2, IN_CREATE | IN_ISDIR, "2026-11"))
        self.assertEqual((events[3].wd, events[3].mask, events[3].cookie, events[3].name), (3, IN_MOVED_TO, 42, "moved.png"))

    def test_linux_inotify_dispatch_subdir_watch_and_ancestor_mount(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cam_dir = root / "Camera"
            cam_dir.mkdir()
            unmounted = root / "media" / "sd" / "DCIM"
            (root / "media").mkdir()

            fake_libc = _FakeLibc()
            ready_files: list[tuple[str, Path]] = []
            created_dirs: list[tuple[str, Path]] = []

            watcher = InotifyWatcher(
                on_file_ready=lambda wf, p: ready_files.append((wf.id, p)),
                on_dir_created=lambda wf, p: created_dirs.append((wf.id, p)),
                libc=fake_libc,
            )
            self.assertFalse(watcher.is_using_polling)

            cam_wf = WatchedFolder(id="cam", path=str(cam_dir), label="Camera")
            sd_wf = WatchedFolder(id="sd", path=str(unmounted), label="SD")
            watcher.sync_folders([cam_wf, sd_wf])

            # Locate assigned watch descriptors
            wd_by_path = {path: wd for wd, (path, _) in fake_libc.watches.items()}
            self.assertIn(str(cam_dir), wd_by_path)
            self.assertIn(str(root / "media"), wd_by_path)

            cam_wd = wd_by_path[str(cam_dir)]
            media_wd = wd_by_path[str(root / "media")]

            # 1. File IN_CREATE must be ignored; IN_CLOSE_WRITE and IN_MOVED_TO must fire.
            (cam_dir / "shot.jpg").write_bytes(b"img")
            (cam_dir / "moved.jpg").write_bytes(b"img2")
            watcher.process_inotify_bytes(
                _pack_inotify_event(cam_wd, IN_CREATE, 0, b"shot.jpg")
                + _pack_inotify_event(cam_wd, IN_CLOSE_WRITE, 0, b"shot.jpg")
                + _pack_inotify_event(cam_wd, IN_MOVED_TO, 7, b"moved.jpg")
            )
            self.assertEqual(
                ready_files,
                [("cam", cam_dir / "shot.jpg"), ("cam", cam_dir / "moved.jpg")],
            )

            # 2. Subdir IN_CREATE | IN_ISDIR attaches watch dynamically and triggers on_dir_created.
            subdir = cam_dir / "2026"
            subdir.mkdir()
            watcher.process_inotify_bytes(_pack_inotify_event(cam_wd, IN_CREATE | IN_ISDIR, 0, b"2026"))
            self.assertTrue(any(path == str(subdir) for path, _ in fake_libc.watches.values()))
            self.assertIn(("cam", subdir), created_dirs)

            # 3. Ancestor mount creation resolves step-by-step down to unmounted SD path.
            (root / "media" / "sd").mkdir()
            watcher.process_inotify_bytes(_pack_inotify_event(media_wd, IN_CREATE | IN_ISDIR, 0, b"sd"))
            sd_wd = next(wd for wd, (p, _) in fake_libc.watches.items() if p == str(root / "media" / "sd"))
            unmounted.mkdir()
            watcher.process_inotify_bytes(_pack_inotify_event(sd_wd, IN_CREATE | IN_ISDIR, 0, b"DCIM"))
            self.assertTrue(any(path == str(unmounted) for path, _ in fake_libc.watches.values()))
            self.assertIn(("sd", unmounted), created_dirs)

    def test_polling_watcher_detects_new_mount_and_new_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            missing_mount = Path(tmp) / "sdcard" / "DCIM"
            wf = WatchedFolder(id="sd", path=str(missing_mount), label="SD")
            ready: list[Path] = []
            mounted: list[Path] = []

            watcher = PollingWatcher(
                on_file_ready=lambda _wf, p: ready.append(p),
                on_dir_created=lambda _wf, p: mounted.append(p),
                poll_interval=0.05,
            )
            watcher.sync_folders([wf])
            watcher.start()
            try:
                missing_mount.mkdir(parents=True)
                self.assertTrue(_wait_until(lambda: missing_mount in mounted))
                photo = missing_mount / "IMG_9.jpg"
                photo.write_bytes(b"sd-card-photo")
                self.assertTrue(_wait_until(lambda: photo in ready))
            finally:
                watcher.stop()

    def test_scan_folder_skips_missing_and_hidden_and_dedups(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg = DaemonConfig()
            db = SyncStore(Path(tmp) / "state.db")
            try:
                missing = WatchedFolder(id="miss", path=str(Path(tmp) / "missing"), label="Missing")
                self.assertEqual(scan_folder(missing, cfg, db), 0)

                cam = Path(tmp) / "Camera"
                (cam / ".hidden").mkdir(parents=True)
                (cam / "2026").mkdir()
                (cam / ".hidden" / "ignore.jpg").write_bytes(b"ignored")
                (cam / "2026" / "keep.jpg").write_bytes(b"kept")
                (cam / "2026" / "empty.jpg").write_bytes(b"")
                wf = WatchedFolder(id="cam", path=str(cam), label="Camera")

                with self.assertLogs("cp0_syncd.store", level="WARNING") as warn_logs:
                    self.assertEqual(scan_folder(wf, cfg, db), 1)
                    self.assertEqual(scan_folder(wf, cfg, db), 0)
                self.assertTrue(any("0-byte" in msg for msg in warn_logs.output))
            finally:
                db.close()


class TestDaemonBinaryEntrypoint(unittest.TestCase):
    def test_cp0_syncd_binary_runs_via_symlink_and_stops_on_sigterm(self) -> None:
        bin_path = REPO_ROOT / "device-daemon" / "bin" / "cp0-syncd"
        self.assertTrue(bin_path.exists())
        self.assertTrue(os.access(bin_path, os.X_OK))

        with tempfile.TemporaryDirectory() as tmp:
            link_path = Path(tmp) / "cp0-syncd-link"
            link_path.symlink_to(bin_path)
            cfg_path = Path(tmp) / "config.json"
            db_path = Path(tmp) / "state.db"
            sock_path = Path(tmp) / "cp0.sock"

            env = os.environ.copy()
            env.pop("PYTHONPATH", None)
            env["CP0_SYNC_CONFIG"] = str(cfg_path)
            env["CP0_SYNC_DB"] = str(db_path)
            env["CP0_SYNC_SOCK"] = str(sock_path)

            proc = subprocess.Popen(
                [str(link_path)],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                self.assertTrue(_wait_until(lambda: sock_path.exists(), timeout=5.0))
                resp = _send_ipc(sock_path, {"cmd": "status"})
                self.assertTrue(resp["ok"])
                proc.send_signal(signal.SIGTERM)
                rc = proc.wait(timeout=5.0)
                self.assertEqual(rc, 0)
                self.assertFalse(sock_path.exists())
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=2.0)
                if proc.stdout is not None:
                    proc.stdout.close()
                if proc.stderr is not None:
                    proc.stderr.close()


if __name__ == "__main__":
    unittest.main()
