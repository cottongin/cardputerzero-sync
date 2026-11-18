"""CLI and macOS LaunchAgent management for CardputerZero photo sync."""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path
from subprocess import CompletedProcess
from typing import Any, Callable
from urllib.error import URLError
from urllib.request import urlopen

from .config import ENV_CONFIG_PATH, default_config_path, load_mac_config, save_mac_config

LAUNCH_AGENT_LABEL = "com.cardputerzero.photosync"
LOG_MAX_BYTES = 5 * 1024 * 1024
LOG_BACKUP_COUNT = 2


def default_log_path() -> Path:
    """Return the default rotating log file path for the receiver service."""
    return Path.home() / "Library" / "Logs" / "cp0-mac-sync.log"


def default_plist_path() -> Path:
    """Return the default macOS LaunchAgent plist path."""
    return Path.home() / "Library" / "LaunchAgents" / f"{LAUNCH_AGENT_LABEL}.plist"


def default_receiver_bin() -> Path:
    """Locate the cp0-mac-receiver executable script."""
    repo_bin = Path(os.path.realpath(__file__)).resolve().parents[1] / "bin" / "cp0-mac-receiver"
    if repo_bin.exists():
        return repo_bin
    in_path = shutil.which("cp0-mac-receiver")
    return Path(in_path) if in_path else repo_bin


def _launchd_domain() -> str:
    return f"gui/{os.getuid()}"


def _service_target() -> str:
    return f"{_launchd_domain()}/{LAUNCH_AGENT_LABEL}"


def _default_runner(cmd: list[str], **kwargs: Any) -> CompletedProcess[str]:
    kwargs.setdefault("text", True)
    return subprocess.run(cmd, **kwargs)


def render_launch_agent_plist(receiver_bin: Path, log_path: Path, config_path: Path) -> str:
    """Render the macOS LaunchAgent plist XML for cp0-mac-receiver."""
    payload = {
        "Label": LAUNCH_AGENT_LABEL,
        "ProgramArguments": [sys.executable, str(receiver_bin)],
        "RunAtLoad": True,
        "KeepAlive": True,
        "StandardOutPath": str(log_path),
        "StandardErrorPath": str(log_path),
        "EnvironmentVariables": {
            ENV_CONFIG_PATH: str(config_path),
        },
    }
    return plistlib.dumps(payload, sort_keys=False).decode("utf-8")


def _resolve_path(path: Path | None, fallback: Callable[[], Path]) -> Path:
    return Path(path).expanduser() if path is not None else fallback()


def _cmd_set_dir(args: argparse.Namespace, cfg_path: Path) -> int:
    dest = Path(args.path).expanduser().resolve()
    if dest.exists() and not dest.is_dir():
        print(f"Error: {dest} exists and is not a directory", file=sys.stderr)
        return 1
    try:
        dest.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        print(f"Error creating directory {dest}: {exc}", file=sys.stderr)
        return 1

    cfg = load_mac_config(cfg_path)
    cfg.dest_root = str(dest)
    save_mac_config(cfg, cfg_path)
    print(f"Destination directory set to {dest}")
    return 0


def _cmd_set_port(
    args: argparse.Namespace,
    cfg_path: Path,
    plist_path: Path,
    runner: Callable[..., CompletedProcess[str]],
) -> int:
    try:
        port = int(args.port)
    except ValueError:
        print(f"Error: invalid port {args.port!r}", file=sys.stderr)
        return 2

    if not (1 <= port <= 65535):
        print(f"Error: port must be between 1 and 65535 (got {port})", file=sys.stderr)
        return 2

    cfg = load_mac_config(cfg_path)
    cfg.listen_port = port
    save_mac_config(cfg, cfg_path)

    if plist_path.exists():
        res = runner(["launchctl", "kickstart", "-k", _service_target()], capture_output=True, check=False)
        if res.returncode != 0:
            res = runner(
                ["launchctl", "bootstrap", _launchd_domain(), str(plist_path)],
                capture_output=True,
                check=False,
            )
        if res.returncode != 0:
            err = (res.stderr or "").strip() or f"launchctl exited with {res.returncode}"
            print(f"Warning: updated port to {port}, but failed to restart service: {err}", file=sys.stderr)
            return res.returncode

    print(f"Listen port set to {port}")
    return 0


def _cmd_install_service(
    cfg_path: Path,
    plist_path: Path,
    log_path: Path,
    runner: Callable[..., CompletedProcess[str]],
) -> int:
    if not cfg_path.exists():
        save_mac_config(load_mac_config(cfg_path), cfg_path)

    plist_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass

    plist_xml = render_launch_agent_plist(default_receiver_bin(), log_path, cfg_path)
    plist_path.write_text(plist_xml, encoding="utf-8")

    runner(["launchctl", "bootout", _service_target()], capture_output=True, check=False)
    res = runner(
        ["launchctl", "bootstrap", _launchd_domain(), str(plist_path)],
        capture_output=True,
        check=False,
    )
    if res.returncode != 0:
        err = (res.stderr or "").strip() or f"launchctl bootstrap exited with {res.returncode}"
        print(f"Error installing LaunchAgent: {err}", file=sys.stderr)
        return res.returncode

    print(f"Installed LaunchAgent at {plist_path}")
    return 0


def _cmd_uninstall_service(
    plist_path: Path,
    runner: Callable[..., CompletedProcess[str]],
) -> int:
    runner(["launchctl", "bootout", _service_target()], capture_output=True, check=False)
    plist_path.unlink(missing_ok=True)
    print(f"Uninstalled LaunchAgent ({LAUNCH_AGENT_LABEL})")
    return 0


def _probe_health(port: int) -> dict[str, Any] | None:
    try:
        with urlopen(f"http://127.0.0.1:{port}/health", timeout=2.0) as resp:
            if resp.status != 200:
                return None
            data = json.loads(resp.read().decode("utf-8"))
            return data if isinstance(data, dict) else None
    except (URLError, OSError, ValueError, json.JSONDecodeError):
        return None


def _probe_tailscale(runner: Callable[..., CompletedProcess[str]]) -> str:
    try:
        res = runner(["tailscale", "status", "--json"], capture_output=True, check=False)
    except OSError:
        return "unavailable (tailscale CLI not found)"

    if res.returncode != 0 or not res.stdout:
        return "not connected"

    try:
        data = json.loads(res.stdout)
    except json.JSONDecodeError:
        return "unknown (invalid tailscale JSON)"

    self_node = data.get("Self")
    if not isinstance(self_node, dict):
        return "unknown"

    dns_name = str(self_node.get("DNSName") or "").rstrip(".")
    ips = self_node.get("TailscaleIPs") or []
    ip_str = ", ".join(str(ip) for ip in ips if ip)
    if dns_name and ip_str:
        return f"{dns_name} ({ip_str})"
    return dns_name or ip_str or "connected"


def _probe_launchctl(runner: Callable[..., CompletedProcess[str]]) -> str:
    try:
        res = runner(["launchctl", "print", _service_target()], capture_output=True, check=False)
    except OSError:
        return "unknown (launchctl unavailable)"
    if res.returncode != 0:
        return "not loaded"

    state = "loaded"
    pid = None
    for line in (res.stdout or "").splitlines():
        stripped = line.strip()
        if stripped.startswith("state ="):
            state = stripped.split("=", 1)[1].strip()
        elif stripped.startswith("pid ="):
            pid = stripped.split("=", 1)[1].strip()
    return f"{state} (pid {pid})" if pid else state


def _cmd_status(
    cfg_path: Path,
    plist_path: Path,
    runner: Callable[..., CompletedProcess[str]],
) -> int:
    cfg = load_mac_config(cfg_path)
    installed = plist_path.exists()
    launchd_state = _probe_launchctl(runner)
    health = _probe_health(cfg.listen_port)
    tailscale_info = _probe_tailscale(runner)

    print(f"Service:     {LAUNCH_AGENT_LABEL} ({'installed' if installed else 'not installed'}, {launchd_state})")
    if health is not None:
        print(f"Receiver:    ok (listening on {cfg.listen_host}:{cfg.listen_port}, v{health.get('version', '?')})")
    else:
        print(f"Receiver:    unreachable (configured {cfg.listen_host}:{cfg.listen_port})")
    print(f"Destination: {cfg.dest_root}")
    print(f"Tailscale:   {tailscale_info}")
    print(f"Config:      {cfg_path}")
    return 0


def _cmd_logs(
    args: argparse.Namespace,
    log_path: Path,
    runner: Callable[..., CompletedProcess[str]],
) -> int:
    if args.follow:
        if not log_path.exists():
            print(f"No log file found at {log_path}", file=sys.stderr)
            return 1
        res = runner(["tail", "-n", str(args.lines), "-f", str(log_path)], check=False)
        return res.returncode

    if not log_path.exists():
        print(f"No log file found at {log_path}")
        return 0

    with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
        recent = deque(handle, maxlen=args.lines)
    for line in recent:
        sys.stdout.write(line)
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="cp0-mac-sync",
        description="Manage the CardputerZero Mac photo receiver service.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("install-service", help="Install and start the LaunchAgent service.")
    sub.add_parser("uninstall-service", help="Stop and remove the LaunchAgent service.")

    p_dir = sub.add_parser("set-dir", help="Set and create the destination photo directory.")
    p_dir.add_argument("path", help="Destination root directory for synced photos.")

    p_port = sub.add_parser("set-port", help="Set the HTTP receiver listen port (1-65535).")
    p_port.add_argument("port", help="TCP port number.")

    sub.add_parser("status", help="Show LaunchAgent, receiver health, and Tailscale status.")

    p_logs = sub.add_parser("logs", help="View receiver logs.")
    p_logs.add_argument("-f", "--follow", action="store_true", help="Follow log output.")
    p_logs.add_argument("-n", "--lines", type=int, default=100, help="Number of recent lines to display.")

    return parser


def run_cli(
    argv: list[str],
    config_path: Path | None = None,
    plist_path: Path | None = None,
    runner: Callable[..., CompletedProcess[str]] | None = None,
    log_path: Path | None = None,
) -> int:
    """Execute a cp0-mac-sync CLI invocation and return an integer exit status."""
    parser = _build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else 1

    cfg_path = _resolve_path(config_path, default_config_path)
    target_plist = _resolve_path(plist_path, default_plist_path)
    target_log = _resolve_path(log_path, default_log_path)
    exec_runner = runner or _default_runner

    if args.command == "set-dir":
        return _cmd_set_dir(args, cfg_path)
    if args.command == "set-port":
        return _cmd_set_port(args, cfg_path, target_plist, exec_runner)
    if args.command == "install-service":
        return _cmd_install_service(cfg_path, target_plist, target_log, exec_runner)
    if args.command == "uninstall-service":
        return _cmd_uninstall_service(target_plist, exec_runner)
    if args.command == "status":
        return _cmd_status(cfg_path, target_plist, exec_runner)
    if args.command == "logs":
        return _cmd_logs(args, target_log, exec_runner)

    parser.print_help()
    return 2
