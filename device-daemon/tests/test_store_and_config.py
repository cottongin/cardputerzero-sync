"""Unit tests for device-daemon configuration and SQLite WAL queue store."""

from __future__ import annotations

import json
import os
import sqlite3
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from cp0_syncd.config import (
    DaemonConfig,
    WatchedFolder,
    add_watched_folder,
    default_config_path,
    load_daemon_config,
    remove_watched_folder,
    save_daemon_config,
)
from cp0_syncd.store import (
    SyncStore,
    compute_sha256,
    default_db_path,
    is_media_file,
)


class TestDaemonConfig(unittest.TestCase):
    def test_first_run_creates_default_camera_folder_and_theme(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / "home"
            cfg_path = Path(tmp) / "config.json"
            cfg = load_daemon_config(cfg_path, home_dir=home)
            self.assertEqual(cfg.theme, "dark")
            self.assertEqual(cfg.target_port, 8765)
            self.assertEqual(cfg.target_host, "")
            self.assertEqual(len(cfg.watched_folders), 1)
            self.assertEqual(
                cfg.watched_folders[0].path,
                str(home / "Pictures" / "DCIM" / "Camera"),
            )
            self.assertEqual(cfg.watched_folders[0].label, "Camera")
            self.assertTrue(cfg.watched_folders[0].enabled)
            self.assertFalse(hasattr(cfg, "auth_token"))

    def test_corrupt_config_falls_back_to_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "config.json"
            cfg_path.write_text("{corrupt", encoding="utf-8")
            with self.assertLogs("cp0_syncd.config", level="WARNING"):
                cfg = load_daemon_config(cfg_path, home_dir=Path(tmp))
            self.assertEqual(cfg.theme, "dark")
            self.assertEqual(cfg.target_port, 8765)
            self.assertEqual(len(cfg.watched_folders), 1)
            self.assertEqual(cfg_path.read_text(encoding="utf-8"), "{corrupt")

    def test_save_and_load_roundtrip_preserves_empty_folders_and_theme(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "nested" / "config.json"
            original = DaemonConfig(
                target_host="my-mac.tailnet.ts.net",
                target_port=9000,
                theme="light",
                watched_folders=[],
            )
            save_daemon_config(original, cfg_path)
            raw = json.loads(cfg_path.read_text(encoding="utf-8"))
            self.assertNotIn("auth_token", raw)

            loaded = load_daemon_config(cfg_path, home_dir=Path(tmp))
            self.assertEqual(loaded.target_host, "my-mac.tailnet.ts.net")
            self.assertEqual(loaded.target_port, 9000)
            self.assertEqual(loaded.theme, "light")
            self.assertEqual(loaded.watched_folders, [])

    def test_env_overrides_default_paths(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg_override = str(Path(tmp) / "custom-config.json")
            db_override = str(Path(tmp) / "custom-state.db")
            with mock.patch.dict(
                os.environ,
                {"CP0_SYNC_CONFIG": cfg_override, "CP0_SYNC_DB": db_override},
                clear=False,
            ):
                self.assertEqual(default_config_path(), Path(cfg_override))
                self.assertEqual(default_db_path(), Path(db_override))

    def test_overlap_rejected_and_duplicate_labels_disambiguated(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d1 = Path(tmp) / "a" / "Camera"
            d2 = Path(tmp) / "b" / "Camera"
            d3 = Path(tmp) / "c" / "Camera"
            child = d1 / "sub"
            parent = Path(tmp) / "a"
            d1.mkdir(parents=True)
            d2.mkdir(parents=True)
            d3.mkdir(parents=True)
            child.mkdir()

            cfg = DaemonConfig(watched_folders=[])
            cfg, wf1 = add_watched_folder(cfg, str(d1))
            cfg, wf2 = add_watched_folder(cfg, str(d2))
            cfg, wf3 = add_watched_folder(cfg, str(d3))
            self.assertEqual(wf1.label, "Camera")
            self.assertEqual(wf2.label, "Camera-2")
            self.assertEqual(wf3.label, "Camera-3")

            with self.assertRaises(ValueError):
                add_watched_folder(cfg, str(child))
            with self.assertRaises(ValueError):
                add_watched_folder(cfg, str(parent))
            with self.assertRaises(ValueError):
                add_watched_folder(cfg, str(d1))

            symlink_d1 = Path(tmp) / "cam-link"
            symlink_d1.symlink_to(d1)
            with self.assertRaises(ValueError):
                add_watched_folder(cfg, str(symlink_d1))

    def test_add_nonexistent_or_file_raises_and_remove_folder(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "missing"
            not_dir = Path(tmp) / "file.txt"
            not_dir.write_text("hi", encoding="utf-8")
            valid = Path(tmp) / "Screenshots"
            valid.mkdir()

            cfg = DaemonConfig(watched_folders=[])
            with self.assertRaises(ValueError):
                add_watched_folder(cfg, str(missing))
            with self.assertRaises(ValueError):
                add_watched_folder(cfg, str(not_dir))

            cfg, wf = add_watched_folder(cfg, str(valid), label="Shots")
            self.assertEqual(wf.label, "Shots")
            self.assertEqual(len(cfg.watched_folders), 1)

            cfg = remove_watched_folder(cfg, wf.id)
            self.assertEqual(cfg.watched_folders, [])


class TestSyncStore(unittest.TestCase):
    def test_dedup_zero_byte_skip_and_log_cap(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            db = SyncStore(Path(tmp) / "state.db")
            try:
                img = Path(tmp) / "IMG_0001.jpg"
                img.write_bytes(b"photo-bytes")
                zero = Path(tmp) / "empty.jpg"
                zero.write_bytes(b"")
                cfg = DaemonConfig()

                self.assertTrue(is_media_file(img, cfg))
                with self.assertLogs("cp0_syncd.store", level="WARNING"):
                    self.assertFalse(is_media_file(zero, cfg))

                sha = compute_sha256(img)
                self.assertEqual(
                    db.enqueue_file("cam", str(img), "IMG_0001.jpg", 11, 1, sha),
                    "enqueued",
                )
                self.assertEqual(
                    db.enqueue_file("cam", str(img), "IMG_0001.jpg", 11, 1, sha),
                    "already_queued",
                )
                row = db.next_pending()
                self.assertIsNotNone(row)
                assert row is not None
                db.mark_synced(row["id"])
                self.assertEqual(
                    db.enqueue_file("cam", str(img), "IMG_0001.jpg", 11, 1, sha),
                    "already_synced",
                )

                # Different content at same path should enqueue cleanly
                img.write_bytes(b"updated-photo-bytes")
                sha2 = compute_sha256(img)
                self.assertEqual(
                    db.enqueue_file("cam", str(img), "IMG_0001.jpg", 19, 2, sha2),
                    "enqueued",
                )

                for i in range(550):
                    db.log("INFO", f"msg {i}")
                self.assertEqual(db.log_count(), 500)
                recent = db.list_logs(limit=10)
                self.assertEqual(len(recent), 10)
                self.assertEqual(recent[0]["message"], "msg 549")
            finally:
                db.close()

    def test_wal_mode_and_enqueue_zero_byte_logs_warn(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            db_path = Path(tmp) / "state.db"
            db = SyncStore(db_path)
            try:
                conn = sqlite3.connect(str(db_path))
                mode = conn.execute("PRAGMA journal_mode;").fetchone()[0]
                conn.close()
                self.assertEqual(mode.lower(), "wal")

                status = db.enqueue_file("cam", "/tmp/zero.jpg", "zero.jpg", 0, 1, "0" * 64)
                self.assertEqual(status, "skipped_empty")
                logs = db.list_logs(limit=5)
                self.assertEqual(len(logs), 1)
                self.assertEqual(logs[0]["level"], "WARN")
            finally:
                db.close()

    def test_media_filtering_rules(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg = DaemonConfig()
            for name, expected in [
                ("IMG_1.JPEG", True),
                ("clip.mp4", True),
                (".hidden.jpg", False),
                ("~temp.png", False),
                ("photo.jpg.part", False),
                ("photo.jpg.tmp", False),
                ("photo.jpg.crdownload", False),
                ("notes.txt", False),
            ]:
                p = Path(tmp) / name
                p.write_bytes(b"abc")
                self.assertEqual(is_media_file(p, cfg), expected, msg=name)

            subdir = Path(tmp) / "sub.jpg"
            subdir.mkdir()
            self.assertFalse(is_media_file(subdir, cfg))
            self.assertFalse(is_media_file(Path(tmp) / "nonexistent.jpg", cfg))

    def test_queue_lifecycle_meta_and_status_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cam_dir = Path(tmp) / "Camera"
            cam_dir.mkdir()
            missing_dir = Path(tmp) / "SDCard"

            db = SyncStore(Path(tmp) / "state.db")
            try:
                db.enqueue_file("cam", str(cam_dir / "a.jpg"), "a.jpg", 100, 10, "a" * 64)
                db.enqueue_file("cam", str(cam_dir / "b.jpg"), "b.jpg", 200, 20, "b" * 64)
                db.enqueue_file("sd", str(missing_dir / "c.jpg"), "c.jpg", 300, 30, "c" * 64)

                first = db.next_pending()
                assert first is not None
                self.assertEqual(first["rel_path"], "a.jpg")
                db.mark_uploading(first["id"])
                db.mark_retry(first["id"], "timeout")

                # Re-fetch after retry: still PENDING with retry_count == 1
                again = db.next_pending()
                assert again is not None
                self.assertEqual(again["id"], first["id"])
                self.assertEqual(again["retry_count"], 1)
                self.assertEqual(again["last_error"], "timeout")
                db.mark_synced(again["id"])

                second = db.next_pending()
                assert second is not None
                self.assertEqual(second["rel_path"], "b.jpg")
                db.mark_failed(second["id"], "413 too large")

                # Meta roundtrip
                self.assertIsNone(db.get_meta("mac_online"))
                db.set_meta("mac_online", "true")
                db.set_meta("mac_online", "false")
                self.assertEqual(db.get_meta("mac_online"), "false")

                folders = [
                    WatchedFolder(id="cam", path=str(cam_dir), label="Camera"),
                    WatchedFolder(id="sd", path=str(missing_dir), label="SDCard"),
                ]
                snap = db.get_status_snapshot(folders)
                self.assertEqual(snap["counts"]["synced"], 1)
                self.assertEqual(snap["counts"]["failed"], 1)
                self.assertEqual(snap["counts"]["pending"], 1)
                self.assertEqual(snap["counts"]["pending_bytes"], 300)
                self.assertEqual(snap["meta"]["mac_online"], "false")
                self.assertEqual(len(snap["folders"]), 2)
                self.assertTrue(snap["folders"][0]["exists"])
                self.assertFalse(snap["folders"][1]["exists"])
                self.assertEqual(snap["folders"][0]["synced"], 1)
                self.assertEqual(snap["folders"][0]["failed"], 1)
                self.assertEqual(snap["folders"][1]["pending"], 1)

                # retry_all_failed flips FAILED -> PENDING
                self.assertEqual(db.retry_all_failed(), 1)
                # purge_pending_for_folder deletes pending for sd without touching cam
                self.assertEqual(db.purge_pending_for_folder("sd"), 1)
                # delete_file removes remaining pending b.jpg
                rem = db.next_pending()
                assert rem is not None
                self.assertEqual(rem["rel_path"], "b.jpg")
                db.delete_file(rem["id"])
                self.assertIsNone(db.next_pending())
            finally:
                db.close()


if __name__ == "__main__":
    unittest.main()
