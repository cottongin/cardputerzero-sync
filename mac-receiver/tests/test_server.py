import hashlib
import json
import os
import tempfile
import threading
import time
import unittest
from datetime import datetime, timezone
from pathlib import Path
from subprocess import CompletedProcess
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import Request, urlopen

from cp0_mac_receiver.config import MacConfig, load_mac_config, save_mac_config
from cp0_mac_receiver.server import (
    MacReceiverServer,
    _default_osascript_notifier,
    is_allowed_client_ip,
)


class TestMacConfigAndFilter(unittest.TestCase):
    def test_corrupt_config_falls_back_to_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "config.json"
            cfg_path.write_text("{broken json")
            with self.assertLogs("cp0_mac_receiver.config", level="WARNING"):
                cfg = load_mac_config(cfg_path)
            self.assertEqual(cfg.listen_port, 8765)
            self.assertTrue(cfg.tailscale_only)

    def test_missing_config_returns_defaults_and_save_roundtrips(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "nested" / "config.json"
            cfg = load_mac_config(cfg_path)
            self.assertEqual(cfg.listen_host, "0.0.0.0")
            self.assertEqual(cfg.listen_port, 8765)
            self.assertTrue(cfg.notify_on_batch)
            self.assertTrue(cfg.tailscale_only)

            custom = MacConfig(
                dest_root=str(Path(tmp) / "Photos"),
                listen_host="127.0.0.1",
                listen_port=9123,
                notify_on_batch=False,
                tailscale_only=False,
            )
            save_mac_config(custom, cfg_path)
            loaded = load_mac_config(cfg_path)
            self.assertEqual(loaded, custom)

    def test_env_override_cp0_mac_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            env_cfg_path = Path(tmp) / "env-dir" / "config.json"
            custom = MacConfig(
                dest_root=str(Path(tmp) / "EnvPhotos"),
                listen_host="127.0.0.1",
                listen_port=9001,
                notify_on_batch=False,
                tailscale_only=True,
            )
            with patch.dict(os.environ, {"CP0_MAC_CONFIG": str(env_cfg_path)}):
                save_mac_config(custom)
                self.assertTrue(env_cfg_path.is_file())
                loaded = load_mac_config()
            self.assertEqual(loaded, custom)

    def test_tailscale_ip_filter(self):
        self.assertTrue(is_allowed_client_ip("100.101.102.103", True))
        self.assertTrue(is_allowed_client_ip("fd7a:115c:a1e0::1", True))
        self.assertTrue(is_allowed_client_ip("127.0.0.1", True))
        self.assertTrue(is_allowed_client_ip("::1", True))
        self.assertTrue(is_allowed_client_ip("::ffff:100.101.102.103", True))
        self.assertTrue(is_allowed_client_ip("::ffff:127.0.0.1", True))
        self.assertFalse(is_allowed_client_ip("192.168.1.50", True))
        self.assertFalse(is_allowed_client_ip("::ffff:192.168.1.50", True))
        self.assertFalse(is_allowed_client_ip("not-an-ip", True))
        self.assertTrue(is_allowed_client_ip("192.168.1.50", False))

    def test_default_osascript_notifier_passes_message_via_argv(self):
        calls = []

        def fake_run(cmd, **kwargs):
            calls.append(cmd)
            return CompletedProcess(cmd, 0, stdout="", stderr="")

        with patch("cp0_mac_receiver.server.subprocess.run", side_effect=fake_run):
            _default_osascript_notifier(2, Path('/tmp/bad\\"folder'))

        self.assertEqual(len(calls), 1)
        cmd = calls[0]
        self.assertEqual(cmd[0], "osascript")
        self.assertEqual(cmd[1], "-e")
        self.assertIn("on run argv", cmd[2])
        self.assertEqual(cmd[3], 'Synced 2 photos to bad\\"folder')


class TestMacReceiverServer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dest_root = Path(self.tmp.name) / "CardputerZero"
        self.cfg_path = Path(self.tmp.name) / "config.json"
        save_mac_config(
            MacConfig(
                dest_root=str(self.dest_root),
                listen_host="127.0.0.1",
                listen_port=0,
                notify_on_batch=True,
                tailscale_only=True,
            ),
            self.cfg_path,
        )
        self.notified = []
        self.notify_event = threading.Event()

        def _notifier(count: int, dest: Path) -> None:
            self.notified.append((count, dest))
            self.notify_event.set()

        self.srv = MacReceiverServer(
            self.cfg_path,
            notifier=_notifier,
            debounce_sec=0.05,
        )
        self.srv.start()
        self.srv.start()  # idempotent second call
        self.base_url = f"http://127.0.0.1:{self.srv.server_address[1]}"

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.tmp.cleanup()

    def _upload(
        self,
        label: str,
        rel_path: str,
        data: bytes,
        mtime: float = 1700000000.0,
        encode: bool = True,
    ):
        sha = hashlib.sha256(data).hexdigest()
        req = Request(
            f"{self.base_url}/upload",
            data=data,
            method="POST",
            headers={
                "Content-Length": str(len(data)),
                "X-CP0-Folder-Label": quote(label, safe="") if encode else label,
                "X-CP0-Rel-Path": quote(rel_path, safe="/") if encode else rel_path,
                "X-CP0-SHA256": sha,
                "X-CP0-Mtime": str(mtime),
            },
        )
        return urlopen(req)

    def test_health_returns_dest_root(self):
        with urlopen(f"{self.base_url}/health") as resp:
            self.assertEqual(resp.status, 200)
            body = json.loads(resp.read())
            self.assertEqual(body["status"], "ok")
            self.assertEqual(body["dest_root"], str(self.dest_root))
            self.assertIn("version", body)
            self.assertIn("hostname", body)

    def test_rejects_disallowed_client_ip_with_403(self):
        with (
            patch("cp0_mac_receiver.server.is_allowed_client_ip", return_value=False),
            self.assertLogs("cp0_mac_receiver.server", level="WARNING"),
        ):
            with self.assertRaises(HTTPError) as ctx_health:
                urlopen(f"{self.base_url}/health")
            self.assertEqual(ctx_health.exception.code, 403)
            self.assertEqual(json.loads(ctx_health.exception.read())["error"], "forbidden")
            ctx_health.exception.close()

            with self.assertRaises(HTTPError) as ctx_upload:
                self._upload("Camera", "a.jpg", b"abc")
            self.assertEqual(ctx_upload.exception.code, 403)
            self.assertEqual(json.loads(ctx_upload.exception.read())["error"], "forbidden")
            ctx_upload.exception.close()

    def test_upload_dedup_collision_and_utf8_headers(self):
        payload = b"jpeg-bytes-1"
        with self._upload("Trip Photos", "2026/café 1.jpg", payload, 1700000123.0) as resp1:
            r1 = json.loads(resp1.read())
        self.assertEqual(r1["status"], "saved")
        target = self.dest_root / "Trip Photos" / "2026" / "café 1.jpg"
        self.assertEqual(target.read_bytes(), payload)
        self.assertEqual(int(target.stat().st_mtime), 1700000123)

        with self._upload("Trip Photos", "2026/café 1.jpg", payload, 1700000123.0) as resp2:
            r2 = json.loads(resp2.read())
        self.assertEqual(r2["status"], "already_exists")

        diff_payload = b"jpeg-bytes-different"
        collision_mtime = 1700000456.0
        stamp = datetime.fromtimestamp(collision_mtime, tz=timezone.utc).strftime("%Y%m%d_%H%M%S")
        sha8 = hashlib.sha256(diff_payload).hexdigest()[:8]
        expected_collision_name = f"café 1_{stamp}_{sha8}.jpg"

        with self._upload("Trip Photos", "2026/café 1.jpg", diff_payload, collision_mtime) as resp3:
            r3 = json.loads(resp3.read())
        self.assertEqual(r3["status"], "saved")
        saved_path = Path(r3["saved_path"])
        self.assertNotEqual(saved_path, target)
        self.assertEqual(saved_path.name, expected_collision_name)
        self.assertEqual(saved_path.read_bytes(), diff_payload)
        self.assertEqual(target.read_bytes(), payload)

        # Retry of the colliding payload at the same path/mtime must deduplicate cleanly.
        with self._upload("Trip Photos", "2026/café 1.jpg", diff_payload, collision_mtime) as resp4:
            r4 = json.loads(resp4.read())
        self.assertEqual(r4["status"], "already_exists")
        self.assertEqual(list(target.parent.glob("*.part")), [])

    def test_rejects_path_traversal_percent_encoded_and_bad_sha256(self):
        bad_paths = [
            ("../etc", "passwd", True),
            ("Camera", "/etc/passwd", True),
            ("a%2Fb", "img.jpg", False),
            ("Camera", "2026/..%2f..%2fetc/passwd", False),
            ("Camera", "%FF%FE.jpg", False),
        ]
        for label, rel_path, encode in bad_paths:
            with self.assertRaises(HTTPError) as ctx:
                self._upload(label, rel_path, b"bad", encode=encode)
            self.assertEqual(ctx.exception.code, 400)
            ctx.exception.close()

        req = Request(
            f"{self.base_url}/upload",
            data=b"abc",
            method="POST",
            headers={
                "Content-Length": "3",
                "X-CP0-Folder-Label": "Camera",
                "X-CP0-Rel-Path": "a.jpg",
                "X-CP0-SHA256": "0" * 64,
                "X-CP0-Mtime": "1700000000",
            },
        )
        with self.assertRaises(HTTPError) as ctx2:
            urlopen(req)
        self.assertEqual(ctx2.exception.code, 400)
        ctx2.exception.close()

        camera_dir = self.dest_root / "Camera"
        if camera_dir.exists():
            self.assertEqual(list(camera_dir.rglob("*.part")), [])
            self.assertFalse((camera_dir / "a.jpg").exists())

    def test_debounced_batch_notification_fires_once_after_burst(self):
        with self._upload("Camera", "img1.jpg", b"photo-1", 1700000001.0) as r1:
            self.assertEqual(r1.status, 200)
        with self._upload("Camera", "img2.jpg", b"photo-2", 1700000002.0) as r2:
            self.assertEqual(r2.status, 200)

        self.assertTrue(self.notify_event.wait(timeout=1.0), "debounced notification did not fire")
        # Give any extra timer a moment to confirm only one batch fired
        time.sleep(0.08)
        self.assertEqual(len(self.notified), 1)
        count, dest = self.notified[0]
        self.assertEqual(count, 2)
        self.assertEqual(dest, self.dest_root)

    def test_config_reload_picks_up_new_dest_root(self):
        new_dest = Path(self.tmp.name) / "NewDest"
        save_mac_config(
            MacConfig(
                dest_root=str(new_dest),
                listen_host="127.0.0.1",
                listen_port=0,
                notify_on_batch=True,
                tailscale_only=True,
            ),
            self.cfg_path,
        )
        with urlopen(f"{self.base_url}/health") as resp:
            body = json.loads(resp.read())
            self.assertEqual(body["dest_root"], str(new_dest))

        with self._upload("DCIM", "p1.jpg", b"new-dest-bytes") as resp:
            res = json.loads(resp.read())
        self.assertEqual(res["status"], "saved")
        self.assertTrue((new_dest / "DCIM" / "p1.jpg").is_file())


if __name__ == "__main__":
    unittest.main()
