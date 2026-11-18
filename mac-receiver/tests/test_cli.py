import io
import json
import os
import plistlib
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from subprocess import CompletedProcess
from unittest.mock import patch

from cp0_mac_receiver.cli import LAUNCH_AGENT_LABEL, render_launch_agent_plist, run_cli
from cp0_mac_receiver.config import MacConfig, load_mac_config, save_mac_config

BIN_DIR = Path(__file__).resolve().parents[1] / "bin"


class TestMacCli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg_path = Path(self.tmp.name) / "config.json"
        self.plist_path = Path(self.tmp.name) / "com.cardputerzero.photosync.plist"
        self.log_path = Path(self.tmp.name) / "cp0-mac-sync.log"
        self.calls = []

    def tearDown(self):
        self.tmp.cleanup()

    def _fake_runner(self, cmd, **kwargs):
        self.calls.append(cmd)
        return CompletedProcess(cmd, 0, stdout="", stderr="")

    def _run_quiet(self, argv, **kwargs):
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = run_cli(argv, **kwargs)
        return rc, out.getvalue(), err.getvalue()

    def test_set_dir_creates_directory_and_updates_config(self):
        new_dir = Path(self.tmp.name) / "Photos" / "CP0"
        rc, out, _ = self._run_quiet(
            ["set-dir", str(new_dir)],
            config_path=self.cfg_path,
            plist_path=self.plist_path,
            runner=self._fake_runner,
        )
        self.assertEqual(rc, 0)
        self.assertIn(str(new_dir.resolve()), out)
        self.assertTrue(new_dir.is_dir())
        self.assertEqual(load_mac_config(self.cfg_path).dest_root, str(new_dir.resolve()))

    def test_install_and_uninstall_service_writes_and_removes_plist(self):
        rc, _, _ = self._run_quiet(
            ["install-service"],
            config_path=self.cfg_path,
            plist_path=self.plist_path,
            runner=self._fake_runner,
            log_path=self.log_path,
        )
        self.assertEqual(rc, 0)
        plist_text = self.plist_path.read_text(encoding="utf-8")
        self.assertIn("com.cardputerzero.photosync", plist_text)
        self.assertIn("KeepAlive", plist_text)
        parsed = plistlib.loads(plist_text.encode("utf-8"))
        self.assertEqual(parsed["Label"], LAUNCH_AGENT_LABEL)
        self.assertTrue(parsed["RunAtLoad"])
        self.assertTrue(parsed["KeepAlive"])
        self.assertTrue(any("launchctl" in c[0] for c in self.calls))

        rc2, _, _ = self._run_quiet(
            ["uninstall-service"],
            config_path=self.cfg_path,
            plist_path=self.plist_path,
            runner=self._fake_runner,
        )
        self.assertEqual(rc2, 0)
        self.assertFalse(self.plist_path.exists())

    def test_set_port_validates_range_and_restarts_service(self):
        self._run_quiet(
            ["install-service"],
            config_path=self.cfg_path,
            plist_path=self.plist_path,
            runner=self._fake_runner,
            log_path=self.log_path,
        )
        self.calls.clear()
        for bad_port in ("99999", "0", "not-a-port"):
            rc_bad, _, _ = self._run_quiet(
                ["set-port", bad_port],
                config_path=self.cfg_path,
                plist_path=self.plist_path,
                runner=self._fake_runner,
            )
            self.assertNotEqual(rc_bad, 0)

        rc, _, _ = self._run_quiet(
            ["set-port", "9100"],
            config_path=self.cfg_path,
            plist_path=self.plist_path,
            runner=self._fake_runner,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(load_mac_config(self.cfg_path).listen_port, 9100)
        self.assertTrue(any("kickstart" in " ".join(c) or "bootstrap" in " ".join(c) for c in self.calls))

    def test_render_launch_agent_plist_includes_expected_keys(self):
        receiver_bin = Path("/opt/cp0/bin/cp0-mac-receiver")
        xml = render_launch_agent_plist(receiver_bin, self.log_path, self.cfg_path)
        data = plistlib.loads(xml.encode("utf-8"))
        self.assertEqual(data["Label"], LAUNCH_AGENT_LABEL)
        self.assertTrue(data["RunAtLoad"])
        self.assertTrue(data["KeepAlive"])
        self.assertIn(str(receiver_bin), data["ProgramArguments"])
        self.assertEqual(data["StandardOutPath"], str(self.log_path))
        self.assertEqual(data["StandardErrorPath"], str(self.log_path))
        self.assertEqual(data["EnvironmentVariables"]["CP0_MAC_CONFIG"], str(self.cfg_path))

    def test_status_reports_config_tailscale_and_health(self):
        save_mac_config(
            MacConfig(dest_root=str(Path(self.tmp.name) / "Pics"), listen_port=8765),
            self.cfg_path,
        )

        def _status_runner(cmd, **kwargs):
            self.calls.append(cmd)
            if cmd[0] == "tailscale":
                payload = {
                    "Self": {
                        "DNSName": "my-mac.tail123.ts.net.",
                        "TailscaleIPs": ["100.64.0.10"],
                    }
                }
                return CompletedProcess(cmd, 0, stdout=json.dumps(payload), stderr="")
            if cmd[0] == "launchctl":
                return CompletedProcess(cmd, 0, stdout="state = running\npid = 4242\n", stderr="")
            return CompletedProcess(cmd, 0, stdout="", stderr="")

        fake_health = {
            "status": "ok",
            "version": "1.0.0",
            "hostname": "my-mac",
            "dest_root": str(Path(self.tmp.name) / "Pics"),
        }

        class _FakeResp:
            status = 200

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self):
                return json.dumps(fake_health).encode("utf-8")

        with patch("cp0_mac_receiver.cli.urlopen", return_value=_FakeResp()):
            rc, out, _ = self._run_quiet(
                ["status"],
                config_path=self.cfg_path,
                plist_path=self.plist_path,
                runner=_status_runner,
            )
        self.assertEqual(rc, 0)
        self.assertIn("8765", out)
        self.assertIn("my-mac.tail123.ts.net", out)
        self.assertIn("ok", out)

    def test_logs_prints_recent_lines_missing_and_follow(self):
        rc_missing, out_missing, _ = self._run_quiet(
            ["logs"],
            config_path=self.cfg_path,
            plist_path=self.plist_path,
            runner=self._fake_runner,
            log_path=self.log_path,
        )
        self.assertEqual(rc_missing, 0)
        self.assertIn("No log file", out_missing)

        self.log_path.write_text("line1\nline2\nline3\n", encoding="utf-8")
        rc, out, _ = self._run_quiet(
            ["logs"],
            config_path=self.cfg_path,
            plist_path=self.plist_path,
            runner=self._fake_runner,
            log_path=self.log_path,
        )
        self.assertEqual(rc, 0)
        self.assertIn("line1", out)
        self.assertIn("line3", out)

        rc_follow, _, _ = self._run_quiet(
            ["logs", "--follow"],
            config_path=self.cfg_path,
            plist_path=self.plist_path,
            runner=self._fake_runner,
            log_path=self.log_path,
        )
        self.assertEqual(rc_follow, 0)
        self.assertTrue(any(c[0] == "tail" and "-f" in c and str(self.log_path) in c for c in self.calls))

    def test_bin_entrypoints_work_via_symlink_without_pythonpath(self):
        clean_env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}
        for script_name in ("cp0-mac-sync", "cp0-mac-receiver"):
            target = BIN_DIR / script_name
            self.assertTrue(os.access(target, os.X_OK), f"{target} must be executable")
            link = Path(self.tmp.name) / script_name
            link.symlink_to(target)
            res = subprocess.run(
                [sys.executable, str(link), "--help"],
                env=clean_env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(res.returncode, 0, res.stderr)


if __name__ == "__main__":
    unittest.main()
