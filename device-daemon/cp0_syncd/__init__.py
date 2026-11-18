"""CardputerZero on-device background photo sync daemon package."""

from .config import (
    DaemonConfig,
    WatchedFolder,
    add_watched_folder,
    default_config_path,
    load_daemon_config,
    remove_watched_folder,
    save_daemon_config,
)
from .store import (
    SyncStore,
    compute_sha256,
    default_db_path,
    is_media_file,
)

__all__ = [
    "DaemonConfig",
    "SyncStore",
    "WatchedFolder",
    "add_watched_folder",
    "compute_sha256",
    "default_config_path",
    "default_db_path",
    "is_media_file",
    "load_daemon_config",
    "remove_watched_folder",
    "save_daemon_config",
]
