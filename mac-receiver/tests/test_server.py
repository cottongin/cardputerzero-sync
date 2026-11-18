import hashlib
import json
import os
import tempfile
import threading
import time
import unittest
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
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.srv.server_port}"

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.thread.join(timeout=2.0)
        self.tmp.cleanup()

    def _upload(self, label: str, rel_path: str, data: bytes, mtime: float = 1700000000.0):
        sha = hashlib.sha256(data).hexdigest()
        req = Request(
            f"{self.base_url}/upload",
            data=data,
            method="POST",
            headers={
                "Content-Length": str(len(data)),
                "X-CP0-Folder-Label": quote(label, safe=""),
                "X-CP0-Rel-Path": quote(rel_path, safe="/"),
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

        with self._upload("Trip Photos", "2026/café 1.jpg", b"jpeg-bytes-different", 1700000456.0) as resp3:
            r3 = json.loads(resp3.read())
        self.assertEqual(r3["status"], "saved")
        saved_path = Path(r3["saved_path"])
        self.assertNotEqual(saved_path, target)
        self.assertEqual(saved_path.read_bytes(), b"jpeg-bytes-different")
        self.assertEqual(target.read_bytes(), payload)

    def test_rejects_path_traversal_and_bad_sha256(self):
        with self.assertRaises(HTTPError) as ctx:
            self._upload("../etc", "passwd", b"bad")
        self.assertEqual(ctx.exception.code, 400)
        ctx.exception.close()

        with self.assertRaises(HTTPError) as ctx_abs:
            self._upload("Camera", "/etc/passwd", b"bad")
        self.assertEqual(ctx_abs.exception.code, 400)
        ctx_abs.exception.close()

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
