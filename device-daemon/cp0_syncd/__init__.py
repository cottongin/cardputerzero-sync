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
from .daemon import SyncDaemon
from .ipc import IpcServer, default_sock_path
from .store import (
    SyncStore,
    compute_sha256,
    default_db_path,
    is_media_file,
)
from .uploader import Uploader
from .watcher import InotifyWatcher, PollingWatcher, scan_folder

__all__ = [
    "DaemonConfig",
    "InotifyWatcher",
    "IpcServer",
    "PollingWatcher",
    "SyncDaemon",
    "SyncStore",
    "Uploader",
    "WatchedFolder",
    "add_watched_folder",
    "compute_sha256",
    "default_config_path",
    "default_db_path",
    "default_sock_path",
    "is_media_file",
    "load_daemon_config",
    "remove_watched_folder",
    "save_daemon_config",
    "scan_folder",
]
