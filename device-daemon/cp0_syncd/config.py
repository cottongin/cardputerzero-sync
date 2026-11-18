"""Configuration persistence and watched-folder management for cp0-syncd."""

from __future__ import annotations

import json
import logging
import os
import re
import uuid
from dataclasses import asdict, dataclass, field, replace
from pathlib import Path

logger = logging.getLogger(__name__)

ENV_CONFIG_PATH = "CP0_SYNC_CONFIG"
DEFAULT_TARGET_PORT = 8765
DEFAULT_THEME = "dark"
VALID_THEMES = ("dark", "light")
DEFAULT_EXTENSIONS = [
    ".jpg",
    ".jpeg",
    ".png",
    ".webp",
    ".gif",
    ".bmp",
    ".dng",
    ".raw",
    ".mp4",
    ".mov",
]
DEFAULT_IGNORE_PREFIXES = [".", "~"]
DEFAULT_IGNORE_SUFFIXES = [".part", ".tmp", ".crdownload"]


@dataclass
class WatchedFolder:
    """A single local directory watched by cp0-syncd."""

    id: str
    path: str
    label: str
    enabled: bool = True


@dataclass
class DaemonConfig:
    """Runtime configuration for the on-device sync daemon and companion UI."""

    target_host: str = ""
    target_port: int = DEFAULT_TARGET_PORT
    theme: str = DEFAULT_THEME
    watched_folders: list[WatchedFolder] = field(default_factory=list)
    extensions: list[str] = field(default_factory=lambda: list(DEFAULT_EXTENSIONS))
    ignore_prefixes: list[str] = field(default_factory=lambda: list(DEFAULT_IGNORE_PREFIXES))
    ignore_suffixes: list[str] = field(default_factory=lambda: list(DEFAULT_IGNORE_SUFFIXES))


def default_config_path() -> Path:
    """Return the default daemon config path, honoring CP0_SYNC_CONFIG if set."""
    override = os.environ.get(ENV_CONFIG_PATH, "").strip()
    if override:
        return Path(override).expanduser()
    return Path.home() / ".config" / "cp0-sync" / "config.json"


def default_watched_folders(home_dir: Path | None = None) -> list[WatchedFolder]:
    """Return the first-run default watched folder list (<home>/Pictures/DCIM/Camera)."""
    base_home = Path(home_dir).expanduser() if home_dir is not None else Path.home()
    camera_path = base_home / "Pictures" / "DCIM" / "Camera"
    return [
        WatchedFolder(
            id="camera",
            path=str(camera_path),
            label="Camera",
            enabled=True,
        )
    ]


def _resolve_config_path(path: Path | None) -> Path:
    return Path(path).expanduser() if path is not None else default_config_path()


def _valid_port(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 1 <= value <= 65535


def _valid_str_list(value: object) -> list[str] | None:
    if not isinstance(value, list):
        return None
    cleaned = [item for item in value if isinstance(item, str) and item]
    return cleaned if len(cleaned) == len(value) else None


def _parse_watched_folders(raw_folders: object, home_dir: Path | None) -> list[WatchedFolder]:
    if not isinstance(raw_folders, list):
        return default_watched_folders(home_dir)

    parsed: list[WatchedFolder] = []
    for item in raw_folders:
        if not isinstance(item, dict):
            continue
        folder_id = item.get("id")
        folder_path = item.get("path")
        label = item.get("label")
        enabled = item.get("enabled", True)
        if (
            isinstance(folder_id, str)
            and folder_id.strip()
            and isinstance(folder_path, str)
            and folder_path.strip()
            and isinstance(label, str)
            and label.strip()
            and isinstance(enabled, bool)
        ):
            parsed.append(
                WatchedFolder(
                    id=folder_id.strip(),
                    path=folder_path.strip(),
                    label=label.strip(),
                    enabled=enabled,
                )
            )
    return parsed


def _default_daemon_config(home_dir: Path | None = None) -> DaemonConfig:
    return DaemonConfig(watched_folders=default_watched_folders(home_dir))


def load_daemon_config(path: Path | None = None, home_dir: Path | None = None) -> DaemonConfig:
    """Load DaemonConfig from JSON, creating first-run defaults or safely falling back on errors."""
    cfg_path = _resolve_config_path(path)
    defaults = _default_daemon_config(home_dir)

    if not cfg_path.exists():
        try:
            save_daemon_config(defaults, cfg_path)
        except OSError as exc:
            logger.warning("Could not persist first-run config %s (%s); continuing in memory", cfg_path, exc)
        return defaults

    try:
        raw = json.loads(cfg_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        logger.warning("Failed to read config %s (%s); using defaults", cfg_path, exc)
        return defaults

    if not isinstance(raw, dict):
        logger.warning("Config %s is not a JSON object; using defaults", cfg_path)
        return defaults

    target_host = raw.get("target_host")
    target_port = raw.get("target_port")
    theme = raw.get("theme")
    extensions = _valid_str_list(raw.get("extensions"))
    ignore_prefixes = _valid_str_list(raw.get("ignore_prefixes"))
    ignore_suffixes = _valid_str_list(raw.get("ignore_suffixes"))
    watched_folders = (
        _parse_watched_folders(raw.get("watched_folders"), home_dir)
        if "watched_folders" in raw
        else defaults.watched_folders
    )

    return DaemonConfig(
        target_host=target_host.strip() if isinstance(target_host, str) else defaults.target_host,
        target_port=target_port if _valid_port(target_port) else defaults.target_port,
        theme=theme if isinstance(theme, str) and theme in VALID_THEMES else defaults.theme,
        watched_folders=watched_folders,
        extensions=extensions if extensions is not None else defaults.extensions,
        ignore_prefixes=ignore_prefixes if ignore_prefixes is not None else defaults.ignore_prefixes,
        ignore_suffixes=ignore_suffixes if ignore_suffixes is not None else defaults.ignore_suffixes,
    )


def save_daemon_config(cfg: DaemonConfig, path: Path | None = None) -> None:
    """Atomically persist DaemonConfig to disk using a temp file, fsync, and os.replace."""
    cfg_path = _resolve_config_path(path)
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


def _canonical_path(raw_path: str) -> Path:
    expanded = os.path.expanduser(raw_path)
    return Path(os.path.realpath(expanded))


def _paths_overlap(a: Path, b: Path) -> bool:
    return a == b or a in b.parents or b in a.parents


def _disambiguate_label(base_label: str, existing_labels: set[str]) -> str:
    if base_label.lower() not in existing_labels:
        return base_label
    suffix = 2
    while True:
        candidate = f"{base_label}-{suffix}"
        if candidate.lower() not in existing_labels:
            return candidate
        suffix += 1


def _make_folder_id(label: str, existing_ids: set[str]) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", label.lower()).strip("-") or "folder"
    while True:
        candidate = f"{slug}-{uuid.uuid4().hex[:8]}"
        if candidate not in existing_ids:
            return candidate


def add_watched_folder(
    cfg: DaemonConfig,
    folder_path: str,
    label: str | None = None,
) -> tuple[DaemonConfig, WatchedFolder]:
    """Validate and add a watched folder, rejecting missing/overlapping paths and disambiguating labels."""
    if not isinstance(folder_path, str) or not folder_path.strip():
        raise ValueError("Folder path must be a non-empty string")

    candidate = _canonical_path(folder_path.strip())
    if not candidate.exists() or not candidate.is_dir():
        raise ValueError(f"Watched directory does not exist: {folder_path}")

    for existing in cfg.watched_folders:
        existing_real = _canonical_path(existing.path)
        if _paths_overlap(candidate, existing_real):
            raise ValueError(
                f"Folder {candidate} overlaps already-watched path {existing_real}"
            )

    base_label = label.strip() if isinstance(label, str) and label.strip() else (candidate.name or "Folder")
    existing_labels = {wf.label.lower() for wf in cfg.watched_folders}
    unique_label = _disambiguate_label(base_label, existing_labels)

    existing_ids = {wf.id for wf in cfg.watched_folders}
    folder_id = _make_folder_id(unique_label, existing_ids)

    new_folder = WatchedFolder(
        id=folder_id,
        path=str(candidate),
        label=unique_label,
        enabled=True,
    )
    updated_cfg = replace(cfg, watched_folders=[*cfg.watched_folders, new_folder])
    return updated_cfg, new_folder


def remove_watched_folder(cfg: DaemonConfig, folder_id: str) -> DaemonConfig:
    """Return a copy of DaemonConfig without the specified watched folder ID."""
    return replace(
        cfg,
        watched_folders=[wf for wf in cfg.watched_folders if wf.id != folder_id],
    )
