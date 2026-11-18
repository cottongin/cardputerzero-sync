"""Orchestrator daemon wiring watcher, SQLite store, uploader, and IPC server."""

from __future__ import annotations

import logging
import os
import threading
from dataclasses import replace
from pathlib import Path
from typing import Any

from .config import (
    DEFAULT_TARGET_PORT,
    VALID_THEMES,
    DaemonConfig,
    WatchedFolder,
    add_watched_folder,
    default_config_path,
    load_daemon_config,
    remove_watched_folder,
    save_daemon_config,
)
from .ipc import IpcServer, default_sock_path
from .store import SyncStore, compute_sha256, default_db_path, is_media_file
from .uploader import Uploader
from .watcher import InotifyWatcher, scan_folder

logger = logging.getLogger(__name__)


def _safe_int(value: str | None, default: int = 0) -> int:
    if value is None:
        return default
    try:
        return int(value)
    except ValueError:
        return default


class SyncDaemon:
    """On-device CardputerZero photo sync daemon."""

    def __init__(
        self,
        config_path: Path | None = None,
        db_path: Path | None = None,
        sock_path: Path | None = None,
        probe_timeout: float = 3.0,
        watcher_poll_interval: float = 0.5,
    ) -> None:
        self.config_path = Path(config_path).expanduser() if config_path is not None else default_config_path()
        self.db_path = Path(db_path).expanduser() if db_path is not None else default_db_path()
        self.sock_path = Path(sock_path).expanduser() if sock_path is not None else default_sock_path()

        self._config_lock = threading.RLock()
        self.config: DaemonConfig = load_daemon_config(self.config_path)
        self.store = SyncStore(self.db_path)
        self.wake_event = threading.Event()

        self.uploader = Uploader(
            store=self.store,
            get_config=self.get_config,
            wake_event=self.wake_event,
            probe_timeout=probe_timeout,
        )
        self.watcher = InotifyWatcher(
            on_file_ready=self._on_file_ready,
            on_dir_created=self._on_dir_created,
            poll_interval=watcher_poll_interval,
        )
        self.ipc_server = IpcServer(self.sock_path, self.handle_ipc)
        self._started = False

    def get_config(self) -> DaemonConfig:
        """Return the current DaemonConfig safely across threads."""
        with self._config_lock:
            return self.config

    def scan_all(self) -> int:
        """Recursively scan all enabled watched folders and enqueue unseen media files."""
        cfg = self.get_config()
        total = 0
        for folder in cfg.watched_folders:
            total += scan_folder(folder, cfg, self.store)
        if total > 0:
            self.uploader.notify_new_file()
        return total

    def _on_file_ready(self, folder: WatchedFolder, file_path: Path) -> None:
        cfg = self.get_config()
        active = next((wf for wf in cfg.watched_folders if wf.id == folder.id and wf.enabled), None)
        if active is None:
            return
        if not is_media_file(file_path, cfg, store=self.store):
            return

        root = Path(os.path.expanduser(active.path))
        try:
            st = file_path.stat()
            if st.st_size <= 0:
                return
            sha256 = compute_sha256(file_path)
            rel_path = file_path.relative_to(root).as_posix()
        except (OSError, ValueError):
            return

        outcome = self.store.enqueue_file(
            active.id,
            str(file_path),
            rel_path,
            st.st_size,
            st.st_mtime_ns,
            sha256,
        )
        if outcome == "enqueued":
            self.uploader.notify_new_file()

    def _on_dir_created(self, folder: WatchedFolder, _dir_path: Path) -> None:
        cfg = self.get_config()
        active = next((wf for wf in cfg.watched_folders if wf.id == folder.id and wf.enabled), None)
        if active is None:
            return
        added = scan_folder(active, cfg, self.store)
        if added > 0:
            self.uploader.notify_new_file()

    def start(self) -> None:
        """Start watcher, initial scan, IPC server, and uploader loop."""
        if self._started:
            return
        self._started = True
        self.store.log("INFO", "cp0-syncd started")
        self.watcher.sync_folders(self.get_config().watched_folders)
        self.watcher.start()
        self.scan_all()
        self.ipc_server.start()
        self.uploader.start()

    def stop(self) -> None:
        """Stop IPC server, watcher, uploader, and close the SQLite store."""
        self.ipc_server.stop()
        self.watcher.stop()
        self.uploader.stop()
        self.store.close()
        self._started = False

    def run_forever(self, stop_event: threading.Event | None = None) -> None:
        """Run the daemon until stop_event is set."""
        waiter = stop_event if stop_event is not None else threading.Event()
        self.start()
        try:
            waiter.wait()
        finally:
            self.stop()

    def handle_ipc(self, req: dict[str, Any]) -> dict[str, Any]:
        """Dispatch a parsed IPC JSON request dict and return a JSON-serializable response."""
        cmd = req.get("cmd")
        if not isinstance(cmd, str) or not cmd:
            return {"ok": False, "error": "Missing 'cmd' field"}

        if cmd == "status":
            return self._cmd_status()
        if cmd == "sync_now":
            return self._cmd_sync_now()
        if cmd == "add_folder":
            return self._cmd_add_folder(req)
        if cmd == "remove_folder":
            return self._cmd_remove_folder(req)
        if cmd == "set_target":
            return self._cmd_set_target(req)
        if cmd == "set_theme":
            return self._cmd_set_theme(req)
        if cmd == "retry_failed":
            return self._cmd_retry_failed()
        return {"ok": False, "error": f"Unknown command: {cmd}"}

    def _cmd_status(self) -> dict[str, Any]:
        cfg = self.get_config()
        snap = self.store.get_status_snapshot(cfg.watched_folders)
        meta: dict[str, str] = snap["meta"]
        counts: dict[str, int] = snap["counts"]
        return {
            "ok": True,
            "target_host": cfg.target_host,
            "target_port": cfg.target_port,
            "theme": cfg.theme,
            "mac_online": meta.get("mac_online") == "true",
            "mac_dest_root": meta.get("mac_dest_root", ""),
            "syncing": self.uploader.syncing,
            "current_file": self.uploader.current_file,
            "pending_count": counts["pending"],
            "pending_bytes": counts["pending_bytes"],
            "synced_count": counts["synced"],
            "failed_count": counts["failed"],
            "next_retry_sec": self.uploader.next_retry_sec(),
            "last_sync_ts": _safe_int(meta.get("last_sync_ts"), 0),
            "last_synced_file": meta.get("last_synced_file", ""),
            "last_error": meta.get("last_error", ""),
            "folders": [
                {
                    "id": str(item["id"]),
                    "path": str(item["path"]),
                    "label": str(item["label"]),
                    "enabled": bool(item["enabled"]),
                    "exists": bool(item["exists"]),
                    "pending": int(item["pending"]),
                    "synced": int(item["synced"]),
                }
                for item in snap["folders"]
            ],
            "logs": snap["logs"],
        }

    def _cmd_sync_now(self) -> dict[str, Any]:
        added = self.scan_all()
        self.uploader.wake(reset_backoff=True, probe=True)
        return {"ok": True, "enqueued": added}

    def _cmd_add_folder(self, req: dict[str, Any]) -> dict[str, Any]:
        path = req.get("path")
        label = req.get("label")
        if label is not None and not isinstance(label, str):
            return {"ok": False, "error": "Folder label must be a string"}

        with self._config_lock:
            try:
                new_cfg, folder = add_watched_folder(self.config, path, label=label)  # type: ignore[arg-type]
            except ValueError as exc:
                return {"ok": False, "error": str(exc)}
            save_daemon_config(new_cfg, self.config_path)
            self.config = new_cfg

        self.watcher.add_folder(folder)
        added = scan_folder(folder, new_cfg, self.store)
        self.store.log("INFO", f"Added watched folder: {folder.path} ({folder.label})")
        self.uploader.wake(reset_backoff=True, probe=False)
        return {
            "ok": True,
            "folder": {
                "id": folder.id,
                "path": folder.path,
                "label": folder.label,
                "enabled": folder.enabled,
            },
            "enqueued": added,
        }

    def _cmd_remove_folder(self, req: dict[str, Any]) -> dict[str, Any]:
        folder_id = req.get("folder_id")
        if not isinstance(folder_id, str) or not folder_id.strip():
            return {"ok": False, "error": "Missing or invalid 'folder_id'"}

        with self._config_lock:
            if not any(wf.id == folder_id for wf in self.config.watched_folders):
                return {"ok": False, "error": f"Unknown folder_id: {folder_id}"}
            new_cfg = remove_watched_folder(self.config, folder_id)
            save_daemon_config(new_cfg, self.config_path)
            self.config = new_cfg

        self.watcher.remove_folder(folder_id)
        purged = self.store.purge_pending_for_folder(folder_id)
        self.store.log("INFO", f"Removed watched folder {folder_id} (purged {purged} pending)")
        return {"ok": True, "purged": purged}

    def _cmd_set_target(self, req: dict[str, Any]) -> dict[str, Any]:
        raw_host = req.get("host")
        if not isinstance(raw_host, str):
            return {"ok": False, "error": "Target host must be a string"}
        host = raw_host.strip()
        if not host or any(ch.isspace() for ch in host) or "/" in host:
            return {"ok": False, "error": "Invalid target host"}

        port = req.get("port", DEFAULT_TARGET_PORT)
        if not isinstance(port, int) or isinstance(port, bool) or not (1 <= port <= 65535):
            return {"ok": False, "error": "Target port must be an integer between 1 and 65535"}

        with self._config_lock:
            new_cfg = replace(self.config, target_host=host, target_port=port)
            save_daemon_config(new_cfg, self.config_path)
            self.config = new_cfg

        self.store.set_meta("mac_online", "false")
        self.store.set_meta("mac_dest_root", "")
        self.store.set_meta("last_error", "")
        self.store.log("INFO", f"Updated target to {host}:{port}")
        self.uploader.wake(reset_backoff=True, probe=True)
        return {"ok": True, "target_host": host, "target_port": port}

    def _cmd_set_theme(self, req: dict[str, Any]) -> dict[str, Any]:
        theme = req.get("theme")
        if not isinstance(theme, str) or theme not in VALID_THEMES:
            return {"ok": False, "error": "Theme must be 'dark' or 'light'"}

        with self._config_lock:
            new_cfg = replace(self.config, theme=theme)
            save_daemon_config(new_cfg, self.config_path)
            self.config = new_cfg

        return {"ok": True, "theme": theme}

    def _cmd_retry_failed(self) -> dict[str, Any]:
        retried = self.store.retry_all_failed()
        if retried > 0:
            self.store.log("INFO", f"Queued {retried} failed files for retry")
        self.uploader.wake(reset_backoff=True, probe=True)
        return {"ok": True, "retried": retried}
