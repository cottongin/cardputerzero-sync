"""SQLite WAL queue store and media file filtering for cp0-syncd."""

from __future__ import annotations

import hashlib
import logging
import os
import sqlite3
import stat
import threading
import time
from pathlib import Path
from typing import Any

from .config import DaemonConfig, WatchedFolder

logger = logging.getLogger(__name__)

ENV_DB_PATH = "CP0_SYNC_DB"
MAX_LOG_ROWS = 500
CHUNK_SIZE = 65536


def default_db_path() -> Path:
    """Return the default SQLite state database path, honoring CP0_SYNC_DB if set."""
    override = os.environ.get(ENV_DB_PATH, "").strip()
    if override:
        return Path(override).expanduser()
    return Path.home() / ".local" / "share" / "cp0-sync" / "state.db"


def compute_sha256(path: Path, chunk_size: int = CHUNK_SIZE) -> str:
    """Compute the lowercase hex SHA-256 digest of a file incrementally."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def is_media_file(
    path: Path,
    cfg: DaemonConfig,
    store: SyncStore | None = None,
) -> bool:
    """Return True if path is a non-empty regular media file permitted by DaemonConfig."""
    name = path.name
    if not name:
        return False

    if any(name.startswith(prefix) for prefix in cfg.ignore_prefixes if prefix):
        return False

    name_lower = name.lower()
    if any(name_lower.endswith(suffix.lower()) for suffix in cfg.ignore_suffixes if suffix):
        return False

    allowed_exts = {
        ext.lower() if ext.startswith(".") else f".{ext.lower()}"
        for ext in cfg.extensions
        if ext
    }
    if path.suffix.lower() not in allowed_exts:
        return False

    try:
        st = path.stat()
    except OSError:
        return False

    if not stat.S_ISREG(st.st_mode):
        return False

    if st.st_size == 0:
        logger.warning("Skipping 0-byte media file: %s", path)
        if store is not None:
            store.log("WARN", f"Skipped 0-byte file: {name}")
        return False

    return True


def _now_ms() -> int:
    return int(time.time() * 1000)


class SyncStore:
    """Thread-safe SQLite WAL queue, metadata, and ring-buffered log store."""

    def __init__(self, db_path: Path | None = None) -> None:
        resolved = Path(db_path).expanduser() if db_path is not None else default_db_path()
        resolved.parent.mkdir(parents=True, exist_ok=True)
        self.db_path = resolved
        self._lock = threading.RLock()
        self._conn = sqlite3.connect(str(resolved), timeout=5.0, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._closed = False
        self._init_schema()

    def _init_schema(self) -> None:
        with self._lock:
            self._conn.execute("PRAGMA journal_mode=WAL;")
            self._conn.execute("PRAGMA busy_timeout=5000;")
            self._conn.execute("PRAGMA synchronous=NORMAL;")
            self._conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS files (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    folder_id TEXT NOT NULL,
                    abs_path TEXT NOT NULL,
                    rel_path TEXT NOT NULL,
                    size_bytes INTEGER NOT NULL,
                    mtime_ns INTEGER NOT NULL,
                    sha256 TEXT NOT NULL,
                    status TEXT NOT NULL,
                    retry_count INTEGER NOT NULL DEFAULT 0,
                    last_error TEXT,
                    discovered_at INTEGER NOT NULL,
                    synced_at INTEGER
                );
                CREATE UNIQUE INDEX IF NOT EXISTS idx_files_dedup
                    ON files(folder_id, rel_path, sha256);
                CREATE INDEX IF NOT EXISTS idx_files_status_discovered
                    ON files(status, discovered_at, id);

                CREATE TABLE IF NOT EXISTS logs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    ts INTEGER NOT NULL,
                    level TEXT NOT NULL,
                    message TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS meta (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );
                """
            )
            self._conn.commit()

    def enqueue_file(
        self,
        folder_id: str,
        abs_path: str,
        rel_path: str,
        size_bytes: int,
        mtime_ns: int,
        sha256: str,
    ) -> str:
        """Enqueue a file if not already synced/queued; skip 0-byte payloads with a WARN log."""
        if size_bytes <= 0:
            self.log("WARN", f"Skipped 0-byte file: {rel_path}")
            return "skipped_empty"

        now_ms = _now_ms()
        with self._lock:
            row = self._conn.execute(
                """
                SELECT id, status
                FROM files
                WHERE folder_id = ? AND rel_path = ? AND sha256 = ?
                """,
                (folder_id, rel_path, sha256),
            ).fetchone()

            if row is not None:
                status = str(row["status"])
                if status == "SYNCED":
                    return "already_synced"
                if status == "FAILED":
                    return "already_failed"
                return "already_queued"

            self._conn.execute(
                """
                INSERT INTO files (
                    folder_id, abs_path, rel_path, size_bytes, mtime_ns,
                    sha256, status, retry_count, last_error, discovered_at, synced_at
                )
                VALUES (?, ?, ?, ?, ?, ?, 'PENDING', 0, NULL, ?, NULL)
                """,
                (folder_id, abs_path, rel_path, size_bytes, mtime_ns, sha256, now_ms),
            )
            self._conn.commit()
            return "enqueued"

    def next_pending(self) -> dict[str, Any] | None:
        """Return the oldest PENDING file row as a dict, or None if the queue is empty."""
        with self._lock:
            row = self._conn.execute(
                """
                SELECT id, folder_id, abs_path, rel_path, size_bytes, mtime_ns,
                       sha256, status, retry_count, last_error, discovered_at, synced_at
                FROM files
                WHERE status = 'PENDING'
                ORDER BY discovered_at ASC, id ASC
                LIMIT 1
                """
            ).fetchone()
            return dict(row) if row is not None else None

    def mark_uploading(self, file_id: int) -> None:
        """Transition a queued file to UPLOADING."""
        with self._lock:
            self._conn.execute(
                "UPDATE files SET status = 'UPLOADING' WHERE id = ?",
                (file_id,),
            )
            self._conn.commit()

    def mark_synced(self, file_id: int) -> None:
        """Mark a file as SYNCED and clear any prior error."""
        now_ms = _now_ms()
        with self._lock:
            self._conn.execute(
                """
                UPDATE files
                SET status = 'SYNCED', synced_at = ?, last_error = NULL
                WHERE id = ?
                """,
                (now_ms, file_id),
            )
            self._conn.commit()

    def mark_retry(self, file_id: int, err: str) -> None:
        """Return an item to PENDING while recording the retry count and error."""
        with self._lock:
            self._conn.execute(
                """
                UPDATE files
                SET status = 'PENDING', retry_count = retry_count + 1, last_error = ?
                WHERE id = ?
                """,
                (err, file_id),
            )
            self._conn.commit()

    def mark_failed(self, file_id: int, err: str) -> None:
        """Mark a file as FAILED after a permanent per-file rejection."""
        with self._lock:
            self._conn.execute(
                """
                UPDATE files
                SET status = 'FAILED', retry_count = retry_count + 1, last_error = ?
                WHERE id = ?
                """,
                (err, file_id),
            )
            self._conn.commit()

    def delete_file(self, file_id: int) -> None:
        """Delete a file record (e.g., when the source file vanished before upload)."""
        with self._lock:
            self._conn.execute("DELETE FROM files WHERE id = ?", (file_id,))
            self._conn.commit()

    def purge_pending_for_folder(self, folder_id: str) -> int:
        """Delete un-synced PENDING/UPLOADING queue entries for a removed folder."""
        with self._lock:
            cur = self._conn.execute(
                """
                DELETE FROM files
                WHERE folder_id = ? AND status IN ('PENDING', 'UPLOADING')
                """,
                (folder_id,),
            )
            self._conn.commit()
            return cur.rowcount

    def retry_all_failed(self) -> int:
        """Flip all FAILED rows back to PENDING and return the count updated."""
        with self._lock:
            cur = self._conn.execute(
                """
                UPDATE files
                SET status = 'PENDING', last_error = NULL
                WHERE status = 'FAILED'
                """
            )
            self._conn.commit()
            return cur.rowcount

    def log(self, level: str, message: str) -> None:
        """Append a log row and prune the logs table to the newest 500 rows."""
        now_ms = _now_ms()
        normalized_level = level.strip().upper() or "INFO"
        with self._lock:
            self._conn.execute(
                "INSERT INTO logs (ts, level, message) VALUES (?, ?, ?)",
                (now_ms, normalized_level, message),
            )
            self._conn.execute(
                """
                DELETE FROM logs
                WHERE id NOT IN (
                    SELECT id FROM logs ORDER BY id DESC LIMIT ?
                )
                """,
                (MAX_LOG_ROWS,),
            )
            self._conn.commit()

    def log_count(self) -> int:
        """Return the number of rows currently stored in the logs table."""
        with self._lock:
            row = self._conn.execute("SELECT COUNT(*) FROM logs").fetchone()
            return int(row[0]) if row is not None else 0

    def list_logs(self, limit: int = 100) -> list[dict[str, Any]]:
        """Return up to `limit` newest log rows ordered newest-first."""
        safe_limit = max(0, int(limit))
        with self._lock:
            rows = self._conn.execute(
                """
                SELECT id, ts, level, message
                FROM logs
                ORDER BY id DESC
                LIMIT ?
                """,
                (safe_limit,),
            ).fetchall()
            return [dict(row) for row in rows]

    def set_meta(self, key: str, value: str) -> None:
        """Upsert a metadata key-value pair."""
        with self._lock:
            self._conn.execute(
                """
                INSERT INTO meta (key, value) VALUES (?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value
                """,
                (key, str(value)),
            )
            self._conn.commit()

    def get_meta(self, key: str) -> str | None:
        """Fetch a metadata value by key, or None if unset."""
        with self._lock:
            row = self._conn.execute(
                "SELECT value FROM meta WHERE key = ?",
                (key,),
            ).fetchone()
            return str(row["value"]) if row is not None else None

    def get_status_snapshot(self, watched_folders: list[WatchedFolder]) -> dict[str, Any]:
        """Return queue totals, per-folder counts, metadata, and recent logs for status IPC/UI."""
        with self._lock:
            folder_stats: dict[str, dict[str, int]] = {}
            rows = self._conn.execute(
                """
                SELECT folder_id, status, COUNT(*) AS cnt, COALESCE(SUM(size_bytes), 0) AS total_bytes
                FROM files
                GROUP BY folder_id, status
                """
            ).fetchall()

            totals = {
                "pending": 0,
                "uploading": 0,
                "synced": 0,
                "failed": 0,
                "pending_bytes": 0,
            }

            for row in rows:
                fid = str(row["folder_id"])
                status = str(row["status"])
                cnt = int(row["cnt"])
                total_bytes = int(row["total_bytes"])
                bucket = folder_stats.setdefault(
                    fid,
                    {
                        "pending": 0,
                        "uploading": 0,
                        "synced": 0,
                        "failed": 0,
                        "pending_bytes": 0,
                    },
                )
                if status == "PENDING":
                    bucket["pending"] += cnt
                    bucket["pending_bytes"] += total_bytes
                    totals["pending"] += cnt
                    totals["pending_bytes"] += total_bytes
                elif status == "UPLOADING":
                    bucket["pending"] += cnt
                    bucket["uploading"] += cnt
                    bucket["pending_bytes"] += total_bytes
                    totals["pending"] += cnt
                    totals["uploading"] += cnt
                    totals["pending_bytes"] += total_bytes
                elif status == "SYNCED":
                    bucket["synced"] += cnt
                    totals["synced"] += cnt
                elif status == "FAILED":
                    bucket["failed"] += cnt
                    totals["failed"] += cnt

            meta_rows = self._conn.execute("SELECT key, value FROM meta").fetchall()
            meta = {str(r["key"]): str(r["value"]) for r in meta_rows}
            recent_logs = self.list_logs(limit=50)

        folders_out = []
        for wf in watched_folders:
            stats = folder_stats.get(
                wf.id,
                {"pending": 0, "uploading": 0, "synced": 0, "failed": 0, "pending_bytes": 0},
            )
            folders_out.append(
                {
                    "id": wf.id,
                    "path": wf.path,
                    "label": wf.label,
                    "enabled": wf.enabled,
                    "exists": os.path.isdir(os.path.expanduser(wf.path)),
                    "pending": stats["pending"],
                    "uploading": stats["uploading"],
                    "synced": stats["synced"],
                    "failed": stats["failed"],
                    "pending_bytes": stats["pending_bytes"],
                }
            )

        return {
            "counts": totals,
            "folders": folders_out,
            "meta": meta,
            "logs": recent_logs,
        }

    def close(self) -> None:
        """Close the SQLite connection."""
        with self._lock:
            if not self._closed:
                self._conn.close()
                self._closed = True
