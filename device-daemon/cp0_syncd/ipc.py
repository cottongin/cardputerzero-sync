"""Newline-delimited JSON Unix domain socket IPC server for cp0-syncd."""

from __future__ import annotations

import json
import logging
import os
import socket
import threading
from pathlib import Path
from typing import Any, Callable

logger = logging.getLogger(__name__)

ENV_SOCK_PATH = "CP0_SYNC_SOCK"


def default_sock_path() -> Path:
    """Return the default Unix socket path, honoring CP0_SYNC_SOCK then XDG_RUNTIME_DIR."""
    override = os.environ.get(ENV_SOCK_PATH, "").strip()
    if override:
        return Path(override).expanduser()
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "").strip() or "/tmp"
    return Path(runtime_dir) / "cp0-sync.sock"


class IpcServer:
    """AF_UNIX stream server speaking newline-delimited JSON commands."""

    def __init__(
        self,
        sock_path: Path,
        handler: Callable[[dict[str, Any]], dict[str, Any]],
    ) -> None:
        self.sock_path = Path(sock_path).expanduser()
        self._handler = handler
        self._stop_event = threading.Event()
        self._sock: socket.socket | None = None
        self._accept_thread: threading.Thread | None = None

    def start(self) -> None:
        """Unlink any stale socket, bind AF_UNIX socket, and start accept loop."""
        if self._accept_thread is not None and self._accept_thread.is_alive():
            return

        self.sock_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass

        srv_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv_sock.bind(str(self.sock_path))
        os.chmod(self.sock_path, 0o600)
        srv_sock.listen(16)
        srv_sock.settimeout(0.2)

        self._sock = srv_sock
        self._stop_event.clear()
        self._accept_thread = threading.Thread(
            target=self._accept_loop,
            name="cp0-ipc-server",
            daemon=True,
        )
        self._accept_thread.start()

    def stop(self) -> None:
        """Stop the IPC accept loop, close socket, and unlink socket path."""
        self._stop_event.set()
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

        if self._accept_thread is not None and self._accept_thread is not threading.current_thread():
            self._accept_thread.join(timeout=2.0)
        self._accept_thread = None

        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass

    def _accept_loop(self) -> None:
        while not self._stop_event.is_set() and self._sock is not None:
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                if self._stop_event.is_set():
                    break
                continue

            client_thread = threading.Thread(
                target=self._serve_client,
                args=(conn,),
                name="cp0-ipc-client",
                daemon=True,
            )
            client_thread.start()

    def _serve_client(self, conn: socket.socket) -> None:
        conn.settimeout(2.0)
        try:
            with conn:
                with conn.makefile("r", encoding="utf-8", errors="replace") as reader:
                    for raw_line in reader:
                        line = raw_line.strip()
                        if not line:
                            continue
                        response = self._dispatch_line(line)
                        payload = (json.dumps(response, separators=(",", ":")) + "\n").encode("utf-8")
                        conn.sendall(payload)
        except (OSError, TimeoutError):
            pass

    def _dispatch_line(self, line: str) -> dict[str, Any]:
        try:
            req = json.loads(line)
        except json.JSONDecodeError as exc:
            return {"ok": False, "error": f"Invalid JSON: {exc}"}

        if not isinstance(req, dict):
            return {"ok": False, "error": "Request must be a JSON object"}

        try:
            resp = self._handler(req)
        except Exception as exc:
            logger.exception("IPC handler raised exception")
            return {"ok": False, "error": str(exc)}

        if not isinstance(resp, dict):
            return {"ok": False, "error": "Internal handler returned invalid response"}
        return resp
