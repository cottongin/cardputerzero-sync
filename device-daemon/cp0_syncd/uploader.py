"""Adaptive wake-probe HTTP uploader for CardputerZero photos."""

from __future__ import annotations

import hashlib
import http.client
import json
import logging
import math
import os
import threading
import time
from http import HTTPStatus
from pathlib import Path
from typing import Callable
from urllib.parse import quote

from .config import DaemonConfig, WatchedFolder
from .store import SyncStore

logger = logging.getLogger(__name__)

BACKOFF_SCHEDULE = (5, 10, 20, 40, 60)
CHUNK_SIZE = 65536
PERMANENT_FAIL_STATUSES = frozenset({400, 413, 422})


def _extract_error_message(body: bytes) -> str:
    if not body:
        return ""
    try:
        parsed = json.loads(body.decode("utf-8"))
        if isinstance(parsed, dict) and "error" in parsed:
            return str(parsed["error"])
    except (UnicodeDecodeError, json.JSONDecodeError):
        pass
    return body.decode("utf-8", errors="replace").strip()[:200]


def _hash_open_file(handle: object) -> str:
    digest = hashlib.sha256()
    handle.seek(0)  # type: ignore[attr-defined]
    while True:
        chunk = handle.read(CHUNK_SIZE)  # type: ignore[attr-defined]
        if not chunk:
            break
        digest.update(chunk)
    handle.seek(0)  # type: ignore[attr-defined]
    return digest.hexdigest()


class Uploader:
    """Sequential FIFO photo uploader with /health wake-probe and adaptive backoff."""

    def __init__(
        self,
        store: SyncStore,
        get_config: Callable[[], DaemonConfig],
        wake_event: threading.Event,
        probe_timeout: float = 3.0,
        upload_timeout: float = 30.0,
    ) -> None:
        self._store = store
        self._get_config = get_config
        self._wake_event = wake_event
        self._probe_timeout = float(probe_timeout)
        self._upload_timeout = float(upload_timeout)

        self._state_lock = threading.Lock()
        self._process_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None

        self._syncing = False
        self._current_file = ""
        self._backoff_index = 0
        self._current_delay = 0
        self._next_retry_deadline = 0.0
        self._probe_requested = False
        self._logged_no_target = False

    @property
    def syncing(self) -> bool:
        """Return True while actively uploading or probing."""
        with self._state_lock:
            return self._syncing

    @property
    def current_file(self) -> str:
        """Return the relative path of the file currently being uploaded, or ''."""
        with self._state_lock:
            return self._current_file

    def next_retry_sec(self) -> int:
        """Return whole seconds remaining until the next backoff retry, or 0."""
        with self._state_lock:
            if self._next_retry_deadline <= 0.0:
                return 0
            remaining = self._next_retry_deadline - time.monotonic()
            return max(0, int(math.ceil(remaining))) if remaining > 0 else 0

    def notify_new_file(self) -> None:
        """Reset retry backoff and wake the uploader when a new photo arrives."""
        self.wake(reset_backoff=True, probe=False)

    def wake(self, reset_backoff: bool = True, probe: bool = False) -> None:
        """Interrupt backoff sleep immediately and optionally trigger a /health probe."""
        if reset_backoff:
            self._store.set_meta("next_retry_ts", "0")
        with self._state_lock:
            if reset_backoff:
                self._backoff_index = 0
                self._current_delay = 0
                self._next_retry_deadline = 0.0
            if probe:
                self._probe_requested = True
            self._wake_event.set()

    def start(self) -> None:
        """Start the background upload loop thread."""
        if self._thread is not None and self._thread.is_alive():
            return
        cfg = self._get_config()
        with self._state_lock:
            self._stop_event.clear()
            if cfg.target_host.strip():
                self._probe_requested = True
            self._wake_event.set()
        self._thread = threading.Thread(target=self._run_loop, name="cp0-uploader", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """Stop the background upload loop cleanly."""
        with self._state_lock:
            self._stop_event.set()
            self._wake_event.set()
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout=5.0)
        self._thread = None

    def _run_loop(self) -> None:
        while not self._stop_event.is_set():
            with self._state_lock:
                probe = self._probe_requested
                self._probe_requested = False
                self._wake_event.clear()

            has_pending = self._store.next_pending() is not None
            if has_pending or probe:
                result = self.process_once(probe_when_empty=probe)
            else:
                result = "idle"

            with self._state_lock:
                if self._stop_event.is_set():
                    break
                woken = self._wake_event.is_set() or self._probe_requested
                wait_sec = max(1, self._current_delay) if result in ("offline", "paused") else 60.0
            if woken:
                continue

            self._wake_event.wait(timeout=wait_sec)

    def _reset_backoff(self) -> None:
        with self._state_lock:
            self._backoff_index = 0
            self._current_delay = 0
            self._next_retry_deadline = 0.0
        self._store.set_meta("next_retry_ts", "0")

    def _record_backoff(self) -> int:
        with self._state_lock:
            delay = BACKOFF_SCHEDULE[min(self._backoff_index, len(BACKOFF_SCHEDULE) - 1)]
            if self._backoff_index < len(BACKOFF_SCHEDULE) - 1:
                self._backoff_index += 1
            self._current_delay = delay
            self._next_retry_deadline = time.monotonic() + delay
        next_ms = int(time.time() * 1000) + delay * 1000
        self._store.set_meta("next_retry_ts", str(next_ms))
        return delay

    def _set_syncing(self, syncing: bool, current_file: str = "") -> None:
        with self._state_lock:
            self._syncing = syncing
            self._current_file = current_file

    def _probe_health(self, host: str, port: int) -> tuple[bool, str]:
        conn: http.client.HTTPConnection | None = None
        try:
            conn = http.client.HTTPConnection(host, port, timeout=self._probe_timeout)
            conn.request("GET", "/health", headers={"Connection": "close"})
            resp = conn.getresponse()
            body = resp.read()
        except (OSError, http.client.HTTPException, TimeoutError) as exc:
            return (False, str(exc) or exc.__class__.__name__)
        finally:
            if conn is not None:
                conn.close()

        if resp.status != HTTPStatus.OK:
            detail = _extract_error_message(body)
            return (False, f"HTTP {resp.status}: {detail}" if detail else f"HTTP {resp.status}")

        try:
            data = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            return (False, f"Invalid /health JSON: {exc}")

        if not isinstance(data, dict) or data.get("status") != "ok":
            return (False, "Unexpected /health payload")

        dest_root = data.get("dest_root", "")
        return (True, str(dest_root) if isinstance(dest_root, str) else "")

    def process_once(self, probe_when_empty: bool = False) -> str:
        """Probe Mac /health if needed and upload queued PENDING files sequentially."""
        with self._process_lock:
            cfg = self._get_config()
            host = cfg.target_host.strip()
            if not host:
                self._set_syncing(False, "")
                self._reset_backoff()
                self._store.set_meta("mac_online", "false")
                if not self._logged_no_target:
                    self._store.log("INFO", "No target host configured")
                    self._logged_no_target = True
                return "no_target"

            self._logged_no_target = False
            first_row = self._store.next_pending()
            if first_row is None and not probe_when_empty:
                self._reset_backoff()
                return "idle"

            ok, detail = self._probe_health(host, cfg.target_port)
            self._store.set_meta("last_probe_ts", str(int(time.time() * 1000)))
            if not ok:
                self._set_syncing(False, "")
                self._store.set_meta("mac_online", "false")
                self._store.set_meta("last_error", detail)
                self._record_backoff()
                return "offline"

            self._reset_backoff()
            self._store.set_meta("mac_online", "true")
            self._store.set_meta("mac_dest_root", detail)
            if first_row is None:
                self._store.set_meta("last_error", "")
                return "online"

            try:
                while not self._stop_event.is_set():
                    cfg = self._get_config()
                    host = cfg.target_host.strip()
                    if not host:
                        return "no_target"
                    folders_by_id = {wf.id: wf for wf in cfg.watched_folders}

                    row = self._store.next_pending()
                    if row is None:
                        return "synced"

                    folder = folders_by_id.get(str(row["folder_id"]))
                    if folder is None:
                        self._store.purge_pending_for_folder(str(row["folder_id"]))
                        continue

                    outcome = self._upload_row(row, folder, host, cfg.target_port)
                    if outcome in ("paused", "offline"):
                        return outcome
                return "stopped"
            finally:
                self._set_syncing(False, "")

    def _upload_row(
        self,
        row: dict[str, object],
        folder: WatchedFolder,
        host: str,
        port: int,
    ) -> str:
        file_id = int(row["id"])  # type: ignore[arg-type]
        rel_path = str(row["rel_path"])
        abs_path = Path(str(row["abs_path"]))

        try:
            handle = open(abs_path, "rb")
        except FileNotFoundError:
            self._store.delete_file(file_id)
            self._store.log("WARN", f"Removed missing queued file: {rel_path}")
            return "missing"
        except OSError as exc:
            err = f"Read error for {rel_path}: {exc}"
            self._store.mark_retry(file_id, err)
            self._store.set_meta("last_error", err)
            self._store.log("WARN", err)
            self._record_backoff()
            return "paused"

        with handle:
            st = os.fstat(handle.fileno())
            if st.st_size <= 0:
                self._store.delete_file(file_id)
                self._store.log("WARN", f"Removed 0-byte queued file: {rel_path}")
                return "missing"

            if st.st_size != int(row["size_bytes"]) or st.st_mtime_ns != int(row["mtime_ns"]):  # type: ignore[arg-type]
                sha256 = _hash_open_file(handle)
            else:
                sha256 = str(row["sha256"])

            size_bytes = st.st_size
            mtime_sec = st.st_mtime_ns / 1_000_000_000

            self._set_syncing(True, rel_path)
            self._store.mark_uploading(file_id)

            conn: http.client.HTTPConnection | None = None
            try:
                conn = http.client.HTTPConnection(host, port, timeout=self._upload_timeout)
                conn.putrequest("POST", "/upload")
                conn.putheader("Content-Length", str(size_bytes))
                conn.putheader("X-CP0-Folder-Label", quote(folder.label, safe=""))
                conn.putheader("X-CP0-Rel-Path", quote(rel_path, safe="/"))
                conn.putheader("X-CP0-SHA256", sha256)
                conn.putheader("X-CP0-Mtime", f"{mtime_sec:.6f}")
                conn.putheader("Connection", "close")
                conn.endheaders()

                sent = 0
                while sent < size_bytes:
                    chunk = handle.read(min(CHUNK_SIZE, size_bytes - sent))
                    if not chunk:
                        raise OSError("File truncated while streaming upload")
                    conn.send(chunk)
                    sent += len(chunk)

                resp = conn.getresponse()
                body = resp.read()
            except (OSError, http.client.HTTPException, TimeoutError) as exc:
                err = f"Upload network error ({rel_path}): {exc}"
                self._store.mark_retry(file_id, err)
                self._store.set_meta("mac_online", "false")
                self._store.set_meta("last_error", err)
                self._store.log("WARN", err)
                self._record_backoff()
                return "offline"
            finally:
                if conn is not None:
                    conn.close()

        if 200 <= resp.status < 300:
            self._store.mark_synced(file_id)
            self._store.set_meta("last_sync_ts", str(int(time.time() * 1000)))
            self._store.set_meta("last_synced_file", rel_path)
            self._store.set_meta("last_error", "")
            self._reset_backoff()
            self._store.log("INFO", f"Synced {folder.label}/{rel_path}")
            return "synced"

        detail = _extract_error_message(body)
        err = f"HTTP {resp.status}: {detail}" if detail else f"HTTP {resp.status}"
        if resp.status in PERMANENT_FAIL_STATUSES:
            self._store.mark_failed(file_id, err)
            self._store.set_meta("last_error", err)
            self._store.log("ERROR", f"Rejected {rel_path}: {err}")
            return "failed"

        self._store.mark_retry(file_id, err)
        self._store.set_meta("last_error", err)
        self._store.log("ERROR", f"Paused on {rel_path}: {err}")
        self._record_backoff()
        return "paused"
