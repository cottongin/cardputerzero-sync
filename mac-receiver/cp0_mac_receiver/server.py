"""Tailscale-filtered, atomic HTTP receiver server for CardputerZero photos."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import logging
import math
import os
import socket
import subprocess
import threading
import time
import uuid
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable
from urllib.parse import unquote

from .config import MacConfig, load_mac_config

logger = logging.getLogger(__name__)

VERSION = "1.0.0"
CHUNK_SIZE = 64 * 1024

_ALLOWED_NETWORKS = (
    ipaddress.ip_network("100.64.0.0/10"),
    ipaddress.ip_network("fd7a:115c:a1e0::/48"),
    ipaddress.ip_network("127.0.0.0/8"),
    ipaddress.ip_network("::1/128"),
)


def is_allowed_client_ip(ip_str: str, tailscale_only: bool) -> bool:
    """Return True if client IP is allowed under the Tailscale/loopback policy."""
    if not tailscale_only:
        return True

    raw = ip_str.split("%", 1)[0].strip()
    try:
        addr = ipaddress.ip_address(raw)
    except ValueError:
        return False

    if isinstance(addr, ipaddress.IPv6Address) and addr.ipv4_mapped is not None:
        addr = addr.ipv4_mapped

    return any(addr in network for network in _ALLOWED_NETWORKS)


def _default_osascript_notifier(count: int, dest_root: Path) -> None:
    """Display a native macOS Notification Center banner via osascript."""
    noun = "photo" if count == 1 else "photos"
    folder = dest_root.name.replace('"', '\\"')
    message = f"Synced {count} {noun} to {folder}"
    script = f'display notification "{message}" with title "CardputerZero Sync"'
    try:
        subprocess.run(
            ["osascript", "-e", script],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        logger.debug("osascript notification failed: %s", exc)


def _is_valid_sha256_hex(value: str) -> bool:
    if len(value) != 64:
        return False
    try:
        int(value, 16)
        return True
    except ValueError:
        return False


def _hash_file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _sanitize_destination(dest_root: Path, raw_label: str, raw_rel_path: str) -> Path:
    """Validate and resolve the target file path strictly inside dest_root."""
    try:
        label = unquote(raw_label, encoding="utf-8", errors="strict")
        rel_path = unquote(raw_rel_path, encoding="utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise ValueError(f"invalid UTF-8 percent-encoding: {exc}") from exc

    if not label or "\x00" in label or "/" in label or "\\" in label or label in (".", ".."):
        raise ValueError("invalid folder label")

    if not rel_path or "\x00" in rel_path or "\\" in rel_path or rel_path.startswith("/"):
        raise ValueError("invalid relative path")

    segments = rel_path.split("/")
    if any(seg in ("", ".", "..") for seg in segments):
        raise ValueError("path traversal or empty segment rejected")

    resolved_root = dest_root.expanduser().resolve()
    resolved_target = (resolved_root / label / Path(*segments)).resolve()
    if resolved_target == resolved_root or not resolved_target.is_relative_to(resolved_root):
        raise ValueError("destination escapes dest_root")

    return resolved_target


def _collision_candidate(target_path: Path, mtime: float, sha256_hex: str, index: int = 0) -> Path:
    stamp = datetime.fromtimestamp(mtime, tz=timezone.utc).strftime("%Y%m%d_%H%M%S")
    suffix = "" if index == 0 else f"_{index}"
    name = f"{target_path.stem}_{stamp}_{sha256_hex[:8]}{suffix}{target_path.suffix}"
    return target_path.with_name(name)


class _MacReceiverHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: object) -> None:
        logger.debug("%s - %s", self.client_address[0], format % args)

    def _send_json(self, status: int, payload: dict[str, object]) -> None:
        body = json.dumps(payload).encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _load_and_authorize(self) -> MacConfig | None:
        cfg = load_mac_config(self.server.config_path)  # type: ignore[attr-defined]
        client_ip = self.client_address[0]
        if not is_allowed_client_ip(client_ip, cfg.tailscale_only):
            logger.warning("Rejected request from non-Tailscale IP %s", client_ip)
            self._send_json(HTTPStatus.FORBIDDEN, {"error": "forbidden"})
            return None
        return cfg

    def do_GET(self) -> None:
        cfg = self._load_and_authorize()
        if cfg is None:
            return

        path = self.path.split("?", 1)[0]
        if path != "/health":
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
            return

        self._send_json(
            HTTPStatus.OK,
            {
                "status": "ok",
                "version": VERSION,
                "hostname": socket.gethostname(),
                "dest_root": cfg.dest_root,
            },
        )

    def do_POST(self) -> None:
        cfg = self._load_and_authorize()
        if cfg is None:
            return

        path = self.path.split("?", 1)[0]
        if path != "/upload":
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
            return

        raw_len = self.headers.get("Content-Length")
        if raw_len is None:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "missing Content-Length"})
            return
        try:
            content_length = int(raw_len)
        except ValueError:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid Content-Length"})
            return
        if content_length < 0:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "negative Content-Length"})
            return

        expected_sha = (self.headers.get("X-CP0-SHA256") or "").strip().lower()
        if not _is_valid_sha256_hex(expected_sha):
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid X-CP0-SHA256"})
            return

        raw_mtime = self.headers.get("X-CP0-Mtime")
        if raw_mtime is None:
            mtime = time.time()
        else:
            try:
                mtime = float(raw_mtime)
            except ValueError:
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid X-CP0-Mtime"})
                return
            if not math.isfinite(mtime):
                self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid X-CP0-Mtime"})
                return

        raw_label = self.headers.get("X-CP0-Folder-Label", "")
        raw_rel_path = self.headers.get("X-CP0-Rel-Path", "")
        try:
            target_path = _sanitize_destination(Path(cfg.dest_root), raw_label, raw_rel_path)
        except ValueError as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return

        target_path.parent.mkdir(parents=True, exist_ok=True)
        part_path = target_path.parent / f".cp0-{uuid.uuid4().hex}.part"

        try:
            computed_sha = self._stream_to_part(part_path, content_length)
        except _PrematureDisconnectError:
            part_path.unlink(missing_ok=True)
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "incomplete request body"})
            return
        except OSError as exc:
            part_path.unlink(missing_ok=True)
            logger.error("Failed writing upload part file %s: %s", part_path, exc)
            self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": "write_failed"})
            return

        if computed_sha != expected_sha:
            part_path.unlink(missing_ok=True)
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "sha256 mismatch"})
            return

        server: MacReceiverServer = self.server  # type: ignore[assignment]
        status, saved_path = server.commit_part(part_path, target_path, expected_sha, mtime, cfg)
        if status == "already_exists":
            self._send_json(HTTPStatus.OK, {"status": "already_exists"})
            return

        self._send_json(HTTPStatus.OK, {"status": "saved", "saved_path": str(saved_path)})

    def _stream_to_part(self, part_path: Path, content_length: int) -> str:
        digest = hashlib.sha256()
        remaining = content_length
        with open(part_path, "wb") as handle:
            while remaining > 0:
                chunk = self.rfile.read(min(CHUNK_SIZE, remaining))
                if not chunk:
                    raise _PrematureDisconnectError()
                handle.write(chunk)
                digest.update(chunk)
                remaining -= len(chunk)
            handle.flush()
            os.fsync(handle.fileno())
        return digest.hexdigest()


class _PrematureDisconnectError(Exception):
    """Raised when the client disconnects before Content-Length bytes arrive."""


class MacReceiverServer(ThreadingHTTPServer):
    """Multi-threaded HTTP server receiving atomic photo uploads from CardputerZero."""

    allow_reuse_address = True
    daemon_threads = True

    def __init__(
        self,
        config_path: Path | None = None,
        notifier: Callable[[int, Path], None] | None = None,
        debounce_sec: float = 5.0,
    ) -> None:
        self.config_path = config_path
        cfg = load_mac_config(self.config_path)
        if ":" in cfg.listen_host:
            self.address_family = socket.AF_INET6

        self._notifier = notifier or _default_osascript_notifier
        self._debounce_sec = debounce_sec
        self._commit_lock = threading.Lock()
        self._notify_lock = threading.Lock()
        self._pending_count = 0
        self._pending_dest = Path(cfg.dest_root)
        self._notify_timer: threading.Timer | None = None
        self._server_thread: threading.Thread | None = None

        super().__init__((cfg.listen_host, cfg.listen_port), _MacReceiverHandler)

    def start(self) -> None:
        """Start serving requests in a background daemon thread if not already running."""
        if self._server_thread is not None and self._server_thread.is_alive():
            return
        self._server_thread = threading.Thread(target=self.serve_forever, daemon=True)
        self._server_thread.start()

    def shutdown(self) -> None:
        """Stop the HTTP server loop and cancel any pending notification timer."""
        self._cancel_notify_timer()
        super().shutdown()
        if self._server_thread is not None and self._server_thread is not threading.current_thread():
            self._server_thread.join(timeout=2.0)

    def server_close(self) -> None:
        """Close the listening socket and clean up any pending notification timer."""
        self._cancel_notify_timer()
        super().server_close()

    def _cancel_notify_timer(self) -> None:
        with self._notify_lock:
            if self._notify_timer is not None:
                self._notify_timer.cancel()
                self._notify_timer = None

    def _schedule_notification(self, dest_root: Path) -> None:
        with self._notify_lock:
            self._pending_count += 1
            self._pending_dest = dest_root
            if self._notify_timer is not None:
                self._notify_timer.cancel()
            timer = threading.Timer(self._debounce_sec, self._flush_notification)
            timer.daemon = True
            self._notify_timer = timer
            timer.start()

    def _flush_notification(self) -> None:
        with self._notify_lock:
            count = self._pending_count
            dest = self._pending_dest
            self._pending_count = 0
            self._notify_timer = None
        if count > 0:
            try:
                self._notifier(count, dest)
            except Exception:
                logger.exception("Batch notification callback failed")

    def commit_part(
        self,
        part_path: Path,
        target_path: Path,
        sha256_hex: str,
        mtime: float,
        cfg: MacConfig,
    ) -> tuple[str, Path | None]:
        """Deduplicate, handle name collisions, and atomically move part_path to final destination."""
        try:
            with self._commit_lock:
                final_path = target_path
                if final_path.exists():
                    if _hash_file_sha256(final_path) == sha256_hex:
                        part_path.unlink(missing_ok=True)
                        return ("already_exists", None)

                    index = 0
                    while True:
                        candidate = _collision_candidate(target_path, mtime, sha256_hex, index)
                        if not candidate.exists():
                            final_path = candidate
                            break
                        if _hash_file_sha256(candidate) == sha256_hex:
                            part_path.unlink(missing_ok=True)
                            return ("already_exists", None)
                        index += 1

                os.replace(part_path, final_path)
        except BaseException:
            part_path.unlink(missing_ok=True)
            raise

        os.utime(final_path, (mtime, mtime))
        logger.info("Saved photo %s", final_path)
        if cfg.notify_on_batch:
            self._schedule_notification(Path(cfg.dest_root).expanduser())
        return ("saved", final_path)
