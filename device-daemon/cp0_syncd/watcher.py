"""Recursive startup scanner, Linux ctypes inotify watcher, and polling fallback."""

from __future__ import annotations

import ctypes
import ctypes.util
import errno
import logging
import os
import select
import struct
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .config import DaemonConfig, WatchedFolder
from .store import SyncStore, compute_sha256, is_media_file

logger = logging.getLogger(__name__)

# Linux <sys/inotify.h> event masks
IN_CLOSE_WRITE = 0x00000008
IN_MOVED_TO = 0x00000080
IN_CREATE = 0x00000100
IN_DELETE = 0x00000200
IN_DELETE_SELF = 0x00000400
IN_MOVE_SELF = 0x00000800
IN_IGNORED = 0x00008000
IN_ISDIR = 0x40000000

_DIR_WATCH_MASK = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF
_ANCESTOR_WATCH_MASK = IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF
_EVENT_HEADER = struct.Struct("=iIII")
_READ_BUFFER_SIZE = 65536


@dataclass(frozen=True)
class InotifyEvent:
    """Decoded Linux struct inotify_event record."""

    wd: int
    mask: int
    cookie: int
    name: str


def parse_inotify_events(data: bytes) -> list[InotifyEvent]:
    """Decode zero or more packed Linux struct inotify_event entries from raw bytes."""
    events: list[InotifyEvent] = []
    offset = 0
    total = len(data)
    header_size = _EVENT_HEADER.size

    while offset + header_size <= total:
        wd, mask, cookie, name_len = _EVENT_HEADER.unpack_from(data, offset)
        offset += header_size
        if name_len > 0:
            end = min(offset + name_len, total)
            raw_name = data[offset:end].split(b"\x00", 1)[0]
            name = os.fsdecode(raw_name)
            offset += name_len
        else:
            name = ""
        events.append(InotifyEvent(wd=wd, mask=mask, cookie=cookie, name=name))
    return events


def scan_folder(folder: WatchedFolder, cfg: DaemonConfig, store: SyncStore) -> int:
    """Recursively scan an enabled watched folder and enqueue new media files."""
    if not folder.enabled:
        return 0

    root = Path(os.path.expanduser(folder.path))
    if not root.is_dir():
        return 0

    enqueued = 0
    ignore_prefixes = [prefix for prefix in cfg.ignore_prefixes if prefix]

    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = sorted(
            name
            for name in dirnames
            if not any(name.startswith(prefix) for prefix in ignore_prefixes)
        )
        current_dir = Path(dirpath)
        for filename in sorted(filenames):
            full_path = current_dir / filename
            if not is_media_file(full_path, cfg, store=store):
                continue
            try:
                st = full_path.stat()
                if st.st_size <= 0:
                    continue
                sha256 = compute_sha256(full_path)
                rel_path = full_path.relative_to(root).as_posix()
            except (OSError, ValueError):
                continue

            outcome = store.enqueue_file(
                folder.id,
                str(full_path),
                rel_path,
                st.st_size,
                st.st_mtime_ns,
                sha256,
            )
            if outcome == "enqueued":
                enqueued += 1

    return enqueued


def _load_linux_libc() -> Any | None:
    """Load Linux libc.so.6 with inotify symbols, or return None on unsupported OS."""
    if not sys.platform.startswith("linux"):
        return None

    candidates = ["libc.so.6"]
    found = ctypes.util.find_library("c")
    if found and found not in candidates:
        candidates.append(found)

    for candidate in candidates:
        try:
            libc = ctypes.CDLL(candidate, use_errno=True)
            if (
                hasattr(libc, "inotify_init1")
                and hasattr(libc, "inotify_add_watch")
                and hasattr(libc, "inotify_rm_watch")
            ):
                libc.inotify_init1.argtypes = [ctypes.c_int]
                libc.inotify_init1.restype = ctypes.c_int
                libc.inotify_add_watch.argtypes = [
                    ctypes.c_int,
                    ctypes.c_char_p,
                    ctypes.c_uint32,
                ]
                libc.inotify_add_watch.restype = ctypes.c_int
                libc.inotify_rm_watch.argtypes = [ctypes.c_int, ctypes.c_int]
                libc.inotify_rm_watch.restype = ctypes.c_int
                return libc
        except OSError:
            continue
    return None


def _nearest_existing_ancestor(target: Path) -> Path:
    """Return target or its nearest existing ancestor directory."""
    candidate = target
    while True:
        if candidate.is_dir():
            return candidate
        parent = candidate.parent
        if parent == candidate:
            return Path("/")
        candidate = parent


def _iter_subdirs(root: Path) -> list[Path]:
    """Yield root and all non-symlink subdirectories beneath root."""
    dirs: list[Path] = []
    if not root.is_dir():
        return dirs
    for dirpath, dirnames, _ in os.walk(root, followlinks=False):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        dirs.append(Path(dirpath))
    return dirs


class PollingWatcher:
    """Portable mtime/size polling watcher fallback for non-Linux hosts."""

    def __init__(
        self,
        on_file_ready: Callable[[WatchedFolder, Path], None],
        on_dir_created: Callable[[WatchedFolder, Path], None] | None = None,
        poll_interval: float = 0.5,
    ) -> None:
        self._on_file_ready = on_file_ready
        self._on_dir_created = on_dir_created
        self._poll_interval = max(0.02, float(poll_interval))
        self._lock = threading.RLock()
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._folders: dict[str, WatchedFolder] = {}
        self._snapshots: dict[str, dict[str, Any]] = {}

    def sync_folders(self, folders: list[WatchedFolder]) -> None:
        with self._lock:
            target_ids = {wf.id for wf in folders if wf.enabled}
            for existing_id in list(self._folders.keys()):
                if existing_id not in target_ids:
                    self._folders.pop(existing_id, None)
                    self._snapshots.pop(existing_id, None)
            for wf in folders:
                if wf.enabled:
                    if wf.id not in self._folders or self._folders[wf.id].path != wf.path:
                        self._folders[wf.id] = wf
                        self._snapshots[wf.id] = self._build_baseline(wf)
                    else:
                        self._folders[wf.id] = wf

    def add_folder(self, folder: WatchedFolder) -> None:
        with self._lock:
            if not folder.enabled:
                return
            self._folders[folder.id] = folder
            self._snapshots[folder.id] = self._build_baseline(folder)

    def remove_folder(self, folder_id: str) -> None:
        with self._lock:
            self._folders.pop(folder_id, None)
            self._snapshots.pop(folder_id, None)

    def _build_baseline(self, folder: WatchedFolder) -> dict[str, Any]:
        root = Path(os.path.expanduser(folder.path))
        if not root.is_dir():
            return {"present": False, "dirs": set(), "files": {}}
        dirs, files = self._scan_tree(root)
        return {"present": True, "dirs": dirs, "files": files}

    @staticmethod
    def _scan_tree(root: Path) -> tuple[set[Path], dict[Path, tuple[int, int]]]:
        dirs: set[Path] = set()
        files: dict[Path, tuple[int, int]] = {}
        for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            cur = Path(dirpath)
            dirs.add(cur)
            for name in filenames:
                fpath = cur / name
                try:
                    st = fpath.stat()
                except OSError:
                    continue
                files[fpath] = (st.st_size, st.st_mtime_ns)
        return dirs, files

    def poll_once(self) -> None:
        events_dirs: list[tuple[WatchedFolder, Path]] = []
        events_files: list[tuple[WatchedFolder, Path]] = []

        with self._lock:
            items = list(self._folders.items())

        for folder_id, folder in items:
            root = Path(os.path.expanduser(folder.path))
            is_present = root.is_dir()
            with self._lock:
                snap = self._snapshots.get(folder_id)
                if snap is None:
                    continue
                was_present = bool(snap["present"])
                if not is_present:
                    snap["present"] = False
                    snap["dirs"] = set()
                    snap["files"] = {}
                    continue

                dirs, files = self._scan_tree(root)
                prev_dirs: set[Path] = snap["dirs"]
                prev_files: dict[Path, tuple[int, int]] = snap["files"]

                if not was_present:
                    events_dirs.append((folder, root))
                for new_dir in sorted(dirs - prev_dirs):
                    if new_dir != root:
                        events_dirs.append((folder, new_dir))

                for fpath, sig in sorted(files.items()):
                    if prev_files.get(fpath) != sig:
                        events_files.append((folder, fpath))

                snap["present"] = True
                snap["dirs"] = dirs
                snap["files"] = files

        if self._on_dir_created is not None:
            for folder, dir_path in events_dirs:
                self._on_dir_created(folder, dir_path)
        for folder, file_path in events_files:
            self._on_file_ready(folder, file_path)

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run_loop, name="cp0-polling-watcher", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout=2.0)
        self._thread = None

    def _run_loop(self) -> None:
        while not self._stop_event.is_set():
            try:
                self.poll_once()
            except Exception:
                logger.exception("PollingWatcher tick failed")
            self._stop_event.wait(timeout=self._poll_interval)


class InotifyWatcher:
    """Linux ctypes inotify watcher with ancestor tracking and PollingWatcher fallback."""

    def __init__(
        self,
        on_file_ready: Callable[[WatchedFolder, Path], None],
        on_dir_created: Callable[[WatchedFolder, Path], None] | None = None,
        poll_interval: float = 0.5,
        libc: Any | None = None,
    ) -> None:
        self._on_file_ready = on_file_ready
        self._on_dir_created = on_dir_created
        self._poll_interval = max(0.05, float(poll_interval))
        self._lock = threading.RLock()
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None

        self._owns_real_fd = libc is None
        self._libc = libc if libc is not None else _load_linux_libc()
        self._polling: PollingWatcher | None = None
        self._fd: int = -1

        self._folders: dict[str, WatchedFolder] = {}
        self._wd_paths: dict[int, Path] = {}
        self._path_wds: dict[Path, int] = {}
        self._active_by_wd: dict[int, tuple[WatchedFolder, Path]] = {}
        self._ancestors_by_wd: dict[int, dict[str, tuple[WatchedFolder, Path]]] = {}
        self._folder_wds: dict[str, set[int]] = {}
        self._unmounted: dict[str, tuple[WatchedFolder, Path]] = {}

        if self._libc is not None:
            fd = int(self._libc.inotify_init1(0))
            if fd < 0:
                err = ctypes.get_errno()
                logger.warning("inotify_init1 failed (errno=%s); falling back to polling", err)
                self._libc = None
            else:
                self._fd = fd
                if self._owns_real_fd:
                    os.set_inheritable(self._fd, False)
                    os.set_blocking(self._fd, False)

        if self._libc is None:
            self._polling = PollingWatcher(
                on_file_ready=on_file_ready,
                on_dir_created=on_dir_created,
                poll_interval=self._poll_interval,
            )

    @property
    def is_using_polling(self) -> bool:
        """Return True if running with the PollingWatcher fallback."""
        return self._polling is not None

    def sync_folders(self, folders: list[WatchedFolder]) -> None:
        """Synchronize active watches with the given WatchedFolder list."""
        if self._polling is not None:
            self._polling.sync_folders(folders)
            return

        with self._lock:
            enabled_map = {wf.id: wf for wf in folders if wf.enabled}
            for existing_id in list(self._folders.keys()):
                if existing_id not in enabled_map:
                    self._detach_folder_locked(existing_id)

            for wf in folders:
                if not wf.enabled:
                    continue
                prev = self._folders.get(wf.id)
                if prev is None or prev.path != wf.path:
                    self._detach_folder_locked(wf.id)
                    self._attach_folder_locked(wf)
                else:
                    self._folders[wf.id] = wf

    def add_folder(self, folder: WatchedFolder) -> None:
        """Attach watches for a single folder (or its nearest existing ancestor)."""
        if self._polling is not None:
            self._polling.add_folder(folder)
            return
        with self._lock:
            if not folder.enabled:
                return
            self._detach_folder_locked(folder.id)
            self._attach_folder_locked(folder)

    def remove_folder(self, folder_id: str) -> None:
        """Detach all watches associated with folder_id."""
        if self._polling is not None:
            self._polling.remove_folder(folder_id)
            return
        with self._lock:
            self._detach_folder_locked(folder_id)

    def _add_watch_locked(self, dir_path: Path, mask: int) -> int | None:
        assert self._libc is not None
        existing = self._path_wds.get(dir_path)
        if existing is not None:
            return existing
        wd = int(self._libc.inotify_add_watch(self._fd, os.fsencode(str(dir_path)), mask))
        if wd < 0:
            err = ctypes.get_errno()
            logger.warning("inotify_add_watch failed for %s (errno=%s)", dir_path, err)
            return None
        self._wd_paths[wd] = dir_path
        self._path_wds[dir_path] = wd
        return wd

    def _maybe_rm_watch_locked(self, wd: int) -> None:
        if wd in self._active_by_wd:
            return
        if self._ancestors_by_wd.get(wd):
            return
        self._ancestors_by_wd.pop(wd, None)
        dir_path = self._wd_paths.pop(wd, None)
        if dir_path is not None and self._path_wds.get(dir_path) == wd:
            self._path_wds.pop(dir_path, None)
        if self._libc is not None and self._fd >= 0:
            self._libc.inotify_rm_watch(self._fd, wd)

    def _attach_folder_locked(self, folder: WatchedFolder) -> None:
        self._folders[folder.id] = folder
        root = Path(os.path.expanduser(folder.path))
        if root.is_dir():
            self._unmounted.pop(folder.id, None)
            self._attach_tree_locked(folder, root)
        else:
            self._watch_ancestor_locked(folder, root)

    def _attach_tree_locked(self, folder: WatchedFolder, root: Path) -> None:
        wds = self._folder_wds.setdefault(folder.id, set())
        for subdir in _iter_subdirs(root):
            wd = self._add_watch_locked(subdir, _DIR_WATCH_MASK)
            if wd is not None:
                self._active_by_wd[wd] = (folder, subdir)
                wds.add(wd)

    def _watch_ancestor_locked(self, folder: WatchedFolder, target_root: Path) -> None:
        ancestor = _nearest_existing_ancestor(target_root)
        self._unmounted[folder.id] = (folder, target_root)
        wd = self._add_watch_locked(ancestor, _ANCESTOR_WATCH_MASK)
        if wd is not None:
            self._ancestors_by_wd.setdefault(wd, {})[folder.id] = (folder, target_root)
            self._folder_wds.setdefault(folder.id, set()).add(wd)

    def _detach_folder_locked(self, folder_id: str) -> None:
        self._folders.pop(folder_id, None)
        self._unmounted.pop(folder_id, None)
        wds = self._folder_wds.pop(folder_id, set())
        for wd in list(wds):
            active = self._active_by_wd.get(wd)
            if active is not None and active[0].id == folder_id:
                self._active_by_wd.pop(wd, None)
            anc = self._ancestors_by_wd.get(wd)
            if anc is not None:
                anc.pop(folder_id, None)
            self._maybe_rm_watch_locked(wd)

    def _recheck_unmounted_mounts(self) -> None:
        mounted_callbacks: list[tuple[WatchedFolder, Path]] = []
        with self._lock:
            for folder_id, (folder, target_root) in list(self._unmounted.items()):
                if target_root.is_dir():
                    self._detach_folder_locked(folder_id)
                    self._attach_folder_locked(folder)
                    mounted_callbacks.append((folder, target_root))
                    continue

                # If an intermediate mountpoint appeared, tighten ancestor watch closer to target_root.
                best_ancestor = _nearest_existing_ancestor(target_root)
                watched_ancestors = {
                    self._wd_paths[wd]
                    for wd in self._folder_wds.get(folder_id, set())
                    if wd in self._wd_paths
                }
                if best_ancestor not in watched_ancestors:
                    self._detach_folder_locked(folder_id)
                    self._attach_folder_locked(folder)

        if self._on_dir_created is not None:
            for folder, target_root in mounted_callbacks:
                self._on_dir_created(folder, target_root)

    def process_inotify_bytes(self, data: bytes) -> None:
        """Parse raw inotify buffer bytes and dispatch callbacks."""
        file_callbacks: list[tuple[WatchedFolder, Path]] = []
        dir_callbacks: list[tuple[WatchedFolder, Path]] = []

        events = parse_inotify_events(data)
        with self._lock:
            for ev in events:
                watched_dir = self._wd_paths.get(ev.wd)
                if ev.mask & IN_IGNORED:
                    self._cleanup_ignored_wd_locked(ev.wd)
                    continue

                if watched_dir is None:
                    continue

                if ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF):
                    self._handle_self_removed_locked(ev.wd, watched_dir)
                    continue

                if ev.mask & IN_ISDIR:
                    if ev.name and (ev.mask & (IN_CREATE | IN_MOVED_TO)):
                        created_dir = watched_dir / ev.name
                        self._handle_dir_created_locked(
                            ev.wd, created_dir, dir_callbacks
                        )
                    continue

                # File event: react ONLY to IN_CLOSE_WRITE or IN_MOVED_TO (never IN_CREATE).
                if ev.name and (ev.mask & (IN_CLOSE_WRITE | IN_MOVED_TO)):
                    active = self._active_by_wd.get(ev.wd)
                    if active is not None:
                        folder, base_dir = active
                        file_callbacks.append((folder, base_dir / ev.name))

        if self._on_dir_created is not None:
            for folder, dir_path in dir_callbacks:
                self._on_dir_created(folder, dir_path)
        for folder, file_path in file_callbacks:
            self._on_file_ready(folder, file_path)

    def _cleanup_ignored_wd_locked(self, wd: int) -> None:
        dir_path = self._wd_paths.pop(wd, None)
        if dir_path is not None and self._path_wds.get(dir_path) == wd:
            self._path_wds.pop(dir_path, None)
        active = self._active_by_wd.pop(wd, None)
        if active is not None:
            folder, _ = active
            self._folder_wds.get(folder.id, set()).discard(wd)
            root = Path(os.path.expanduser(folder.path))
            if dir_path == root and folder.id in self._folders:
                self._detach_folder_locked(folder.id)
                self._attach_folder_locked(folder)
        anc = self._ancestors_by_wd.pop(wd, None)
        if anc is not None:
            affected = list(anc.keys())
            for fid in affected:
                self._folder_wds.get(fid, set()).discard(wd)
                folder = self._folders.get(fid)
                if folder is not None:
                    self._detach_folder_locked(fid)
                    self._attach_folder_locked(folder)

    def _handle_self_removed_locked(self, wd: int, watched_dir: Path) -> None:
        active = self._active_by_wd.get(wd)
        if active is not None:
            folder, _ = active
            root = Path(os.path.expanduser(folder.path))
            if watched_dir == root:
                self._detach_folder_locked(folder.id)
                self._attach_folder_locked(folder)
                return
            self._active_by_wd.pop(wd, None)
            self._folder_wds.get(folder.id, set()).discard(wd)

        anc = self._ancestors_by_wd.get(wd)
        if anc is not None:
            affected = list(anc.values())
            for folder, target_root in affected:
                self._detach_folder_locked(folder.id)
                self._attach_folder_locked(folder)
            return

        self._maybe_rm_watch_locked(wd)

    def _handle_dir_created_locked(
        self,
        wd: int,
        created_dir: Path,
        dir_callbacks: list[tuple[WatchedFolder, Path]],
    ) -> None:
        active = self._active_by_wd.get(wd)
        if active is not None:
            folder, _ = active
            if created_dir.is_dir():
                self._attach_tree_locked(folder, created_dir)
            dir_callbacks.append((folder, created_dir))

        anc = self._ancestors_by_wd.get(wd)
        if anc is not None:
            for folder_id, (folder, target_root) in list(anc.items()):
                if created_dir == target_root or created_dir in target_root.parents:
                    self._detach_folder_locked(folder_id)
                    self._attach_folder_locked(folder)
                    if target_root.is_dir():
                        dir_callbacks.append((folder, target_root))

    def start(self) -> None:
        """Start background filesystem watching thread."""
        if self._polling is not None:
            self._polling.start()
            return
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run_loop, name="cp0-inotify-watcher", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """Stop background watcher thread and close watch resources."""
        if self._polling is not None:
            self._polling.stop()
            return
        self._stop_event.set()
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout=2.0)
        self._thread = None

        with self._lock:
            for folder_id in list(self._folders.keys()):
                self._detach_folder_locked(folder_id)
            if self._owns_real_fd and self._fd >= 0:
                try:
                    os.close(self._fd)
                except OSError:
                    pass
                self._fd = -1

    def _run_loop(self) -> None:
        while not self._stop_event.is_set():
            fd = self._fd
            if fd < 0:
                break
            try:
                readable, _, _ = select.select([fd], [], [], self._poll_interval)
            except (OSError, ValueError):
                if self._stop_event.is_set():
                    break
                continue

            if readable:
                while not self._stop_event.is_set():
                    try:
                        chunk = os.read(fd, _READ_BUFFER_SIZE)
                    except BlockingIOError:
                        break
                    except OSError as exc:
                        if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                            break
                        if not self._stop_event.is_set():
                            logger.warning("inotify read failed: %s", exc)
                        break
                    if not chunk:
                        break
                    self.process_inotify_bytes(chunk)

            if not self._stop_event.is_set():
                self._recheck_unmounted_mounts()
