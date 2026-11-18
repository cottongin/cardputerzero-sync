import os
import stat
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGING_DIR = ROOT / "packaging"


def _parse_png_chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid PNG signature")
    pos = 8
    chunks: list[tuple[bytes, bytes]] = []
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        chunk_type = data[pos + 4 : pos + 8]
        chunk_data = data[pos + 8 : pos + 8 + length]
        crc_expected = struct.unpack(">I", data[pos + 8 + length : pos + 12 + length])[0]
        crc_actual = zlib.crc32(chunk_type + chunk_data) & 0xFFFFFFFF
        if crc_expected != crc_actual:
            raise ValueError(f"CRC mismatch in chunk {chunk_type!r}")
        chunks.append((chunk_type, chunk_data))
        pos += 12 + length
        if chunk_type == b"IEND":
            break
    return chunks


class TestPackagingAssets(unittest.TestCase):
    def test_systemd_user_unit(self) -> None:
        unit = (PACKAGING_DIR / "cp0-syncd.service").read_text(encoding="utf-8")
        self.assertIn("ExecStart=/usr/bin/cp0-syncd", unit)
        self.assertIn("WantedBy=default.target", unit)
        self.assertIn("Restart=on-failure", unit)
        self.assertIn("RestartSec=5", unit)
        self.assertIn("After=network-online.target", unit)

    def test_desktop_entry(self) -> None:
        desktop = (PACKAGING_DIR / "cp0-sync.desktop").read_text(encoding="utf-8")
        self.assertIn("Type=Application", desktop)
        self.assertIn("Name=Photo Sync", desktop)
        self.assertIn("Exec=/usr/bin/cp0-sync-ui", desktop)
        self.assertIn("Icon=/usr/share/APPLaunch/share/images/cp0-sync.png", desktop)
        self.assertIn("Terminal=false", desktop)

    def test_icon_png_valid_80x80(self) -> None:
        png = (PACKAGING_DIR / "cp0-sync.png").read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        width, height = struct.unpack(">II", png[16:24])
        self.assertEqual(width, 80)
        self.assertEqual(height, 80)

        chunks = _parse_png_chunks(png)
        chunk_types = [t for t, _ in chunks]
        self.assertIn(b"IHDR", chunk_types)
        self.assertIn(b"IDAT", chunk_types)
        self.assertEqual(chunk_types[-1], b"IEND")

        idat_payload = b"".join(d for t, d in chunks if t == b"IDAT")
        raw_pixels = zlib.decompress(idat_payload)
        # 80 rows * (1 filter byte + 80 * 4 RGBA bytes) = 25680 bytes (or RGB = 19280)
        self.assertGreaterEqual(len(raw_pixels), 80 * (1 + 80 * 3))

    def test_debian_control(self) -> None:
        ctrl = (PACKAGING_DIR / "debian" / "control").read_text(encoding="utf-8")
        self.assertIn("Package: cp0-sync", ctrl)
        self.assertIn("Version: 1.0.0-m5stack1", ctrl)
        self.assertIn("Architecture: arm64", ctrl)
        self.assertIn("Depends:", ctrl)
        self.assertIn("python3 (>= 3.9)", ctrl)
        self.assertIn("libsqlite3-0", ctrl)
        self.assertIn("libfreetype6", ctrl)
        self.assertNotIn("${", ctrl)

    def test_maintainer_scripts(self) -> None:
        postinst_path = PACKAGING_DIR / "debian" / "postinst"
        prerm_path = PACKAGING_DIR / "debian" / "prerm"

        for script_path in (postinst_path, prerm_path):
            mode = script_path.stat().st_mode
            self.assertEqual(stat.S_IMODE(mode) & 0o755, 0o755, f"{script_path} must be executable")
            text = script_path.read_text(encoding="utf-8")
            self.assertIn("set -e", text)

        postinst = postinst_path.read_text(encoding="utf-8")
        self.assertIn("systemctl --global enable cp0-syncd.service", postinst)
        self.assertIn("|| true", postinst)

        prerm = prerm_path.read_text(encoding="utf-8")
        self.assertIn("systemctl --global disable cp0-syncd.service", prerm)
        self.assertIn("|| true", prerm)

    def test_build_deb_script_and_stage_only(self) -> None:
        build_script = PACKAGING_DIR / "build-deb.sh"
        mode = build_script.stat().st_mode
        self.assertEqual(stat.S_IMODE(mode) & 0o755, 0o755, "build-deb.sh must be executable")

        syntax = subprocess.run(
            ["bash", "-n", str(build_script)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, f"bash -n failed: {syntax.stderr}")

        script_text = build_script.read_text(encoding="utf-8")
        self.assertIn("cp0-sync_1.0.0_m5stack1_arm64.deb", script_text)

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            dummy_ui = tmp_path / "cp0-sync-ui"
            dummy_ui.write_bytes(b"#!/bin/sh\nexit 0\n")
            dummy_ui.chmod(0o755)

            stage_dir = tmp_path / "stage"
            res = subprocess.run(
                [str(build_script), "--stage-only", str(stage_dir), "--ui-bin", str(dummy_ui)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(res.returncode, 0, f"--stage-only failed:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")

            expected_files = [
                stage_dir / "DEBIAN" / "control",
                stage_dir / "DEBIAN" / "postinst",
                stage_dir / "DEBIAN" / "prerm",
                stage_dir / "usr" / "bin" / "cp0-sync-ui",
                stage_dir / "usr" / "bin" / "cp0-syncd",
                stage_dir / "usr" / "lib" / "cp0-sync" / "cp0_syncd" / "__init__.py",
                stage_dir / "usr" / "lib" / "cp0-sync" / "cp0_syncd" / "daemon.py",
                stage_dir / "usr" / "lib" / "systemd" / "user" / "cp0-syncd.service",
                stage_dir / "usr" / "share" / "APPLaunch" / "applications" / "cp0-sync.desktop",
                stage_dir / "usr" / "share" / "APPLaunch" / "share" / "images" / "cp0-sync.png",
            ]
            for fpath in expected_files:
                self.assertTrue(fpath.is_file(), f"Missing staged file: {fpath}")

            for exec_path in (
                stage_dir / "DEBIAN" / "postinst",
                stage_dir / "DEBIAN" / "prerm",
                stage_dir / "usr" / "bin" / "cp0-sync-ui",
                stage_dir / "usr" / "bin" / "cp0-syncd",
            ):
                self.assertEqual(
                    stat.S_IMODE(exec_path.stat().st_mode),
                    0o755,
                    f"Expected 0755 on {exec_path}",
                )

            pycache_dirs = list((stage_dir / "usr" / "lib" / "cp0-sync").rglob("__pycache__"))
            self.assertEqual(pycache_dirs, [], "Staged Python tree must not contain __pycache__")

    def test_dockerfile_arm64(self) -> None:
        dockerfile = (PACKAGING_DIR / "Dockerfile.arm64").read_text(encoding="utf-8")
        self.assertIn("debian:bookworm-slim", dockerfile)
        for pkg in (
            "build-essential",
            "cmake",
            "pkg-config",
            "libsqlite3-dev",
            "libfreetype-dev",
            "dpkg-dev",
            "python3",
        ):
            self.assertIn(pkg, dockerfile)
        self.assertIn("-DCP0_UI_BUILD_APP=ON", dockerfile)
        self.assertIn("-DCP0_UI_TARGET_DEVICE=ON", dockerfile)
        self.assertIn("-DCP0_UI_BUILD_TESTS=ON", dockerfile)
        self.assertIn("ctest", dockerfile)
        self.assertIn("build-deb.sh --local", dockerfile)


if __name__ == "__main__":
    unittest.main()
