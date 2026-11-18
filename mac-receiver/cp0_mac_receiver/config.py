"""Configuration persistence for the CardputerZero Mac receiver."""

from __future__ import annotations

import json
import logging
import os
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path

logger = logging.getLogger(__name__)

ENV_CONFIG_PATH = "CP0_MAC_CONFIG"
DEFAULT_LISTEN_HOST = "0.0.0.0"
DEFAULT_LISTEN_PORT = 8765


def default_dest_root() -> str:
    """Return the default root directory for synced photos."""
    return str(Path.home() / "Pictures" / "CardputerZero")


def default_config_path() -> Path:
    """Return the default config file path, honoring CP0_MAC_CONFIG if set."""
    override = os.environ.get(ENV_CONFIG_PATH, "").strip()
    if override:
        return Path(override).expanduser()
    return Path.home() / ".config" / "cp0-mac-sync" / "config.json"


@dataclass
class MacConfig:
    """Runtime configuration for the Mac receiver daemon and CLI."""

    dest_root: str = field(default_factory=default_dest_root)
    listen_host: str = DEFAULT_LISTEN_HOST
    listen_port: int = DEFAULT_LISTEN_PORT
    notify_on_batch: bool = True
    tailscale_only: bool = True


def _resolve_path(path: Path | None) -> Path:
    return Path(path).expanduser() if path is not None else default_config_path()


def _valid_port(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 65535


def load_mac_config(path: Path | None = None) -> MacConfig:
    """Load MacConfig from JSON, falling back to safe defaults if missing or corrupt."""
    cfg_path = _resolve_path(path)
    defaults = MacConfig()
    if not cfg_path.exists():
        return defaults

    try:
        raw = json.loads(cfg_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        logger.warning("Failed to read config %s (%s); using defaults", cfg_path, exc)
        return defaults

    if not isinstance(raw, dict):
        logger.warning("Config %s is not a JSON object; using defaults", cfg_path)
        return defaults

    dest_root = raw.get("dest_root")
    listen_host = raw.get("listen_host")
    listen_port = raw.get("listen_port")
    notify_on_batch = raw.get("notify_on_batch")
    tailscale_only = raw.get("tailscale_only")

    return MacConfig(
        dest_root=dest_root if isinstance(dest_root, str) and dest_root.strip() else defaults.dest_root,
        listen_host=listen_host if isinstance(listen_host, str) and listen_host.strip() else defaults.listen_host,
        listen_port=listen_port if _valid_port(listen_port) else defaults.listen_port,
        notify_on_batch=notify_on_batch if isinstance(notify_on_batch, bool) else defaults.notify_on_batch,
        tailscale_only=tailscale_only if isinstance(tailscale_only, bool) else defaults.tailscale_only,
    )


def save_mac_config(cfg: MacConfig, path: Path | None = None) -> None:
    """Atomically persist MacConfig to disk using a temporary file and os.replace."""
    cfg_path = _resolve_path(path)
    cfg_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = cfg_path.parent / f".{cfg_path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp"
    payload = json.dumps(asdict(cfg), indent=2) + "\n"
    try:
        with open(tmp_path, "w", encoding="utf-8") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_path, cfg_path)
    except BaseException:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise
