#!/usr/bin/env python3
"""Offline unit tests for the WUP Stream-style GitHub release allocator."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

PUBLISH_PATH = Path(__file__).resolve().parents[1] / "tools" / "publish_release.py"
spec = importlib.util.spec_from_file_location("publish_release", PUBLISH_PATH)
assert spec is not None and spec.loader is not None
publish = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish)

A = "a" * 40
B = "b" * 40
C = "c" * 40


class ReleaseVersionTests(unittest.TestCase):
    def test_first_version(self):
        self.assertEqual(publish.choose_tag({}, A), (0, False))

    def test_version_increments(self):
        self.assertEqual(publish.choose_tag({0: A, 2: B, 9: C}, "d" * 40),
                         (10, False))

    def test_rerun_same_commit(self):
        self.assertEqual(publish.choose_tag({0: A, 1: B}, A), (0, True))

    def test_parse_lightweight_and_annotated_tags(self):
        raw = (
            f"{A}\trefs/tags/v1.0.0.0\n"
            f"{C}\trefs/tags/v1.0.0.1^{{}}\n"
            f"{B}\trefs/tags/v1.0.0.1\n"  # Tag object, not commit.
            f"{A}\trefs/tags/v2.0.0\n"
            f"{A}\trefs/tags/v1.0.0.foo\n"
        )
        self.assertEqual(publish.read_version_tags(raw), {0: A, 1: C})

    def test_collision_retries_next_version(self):
        class Result:
            returncode = 1
            stderr = "already exists"
        class Ok:
            returncode = 0
        with (patch.object(publish, "remote_tags", side_effect=[
                  {0: A}, {0: A, 1: B}, {0: A, 1: B}]),
              patch.object(publish, "command", side_effect=[Result(), Ok()]) as cmd,
              patch.object(publish.time, "sleep")):
            self.assertEqual(publish.reserve_tag("wozt/pcble2gamepad", C),
                             "v1.0.0.2")
            self.assertEqual(cmd.call_count, 2)

    def test_existing_annotated_tag_reuses_commit(self):
        with (patch.object(publish, "remote_tags", return_value={0: A}),
              patch.object(publish, "command") as cmd):
            self.assertEqual(publish.reserve_tag("wozt/pcble2gamepad", A),
                             "v1.0.0.0")
            cmd.assert_not_called()

    def test_versioned_assets_and_sha256(self):
        with tempfile.TemporaryDirectory() as tmp:
            original = Path(tmp) / "pcble2gamepad-x86_64.AppImage"
            original.write_bytes(b"\x7fELF" + b"abc" * 512)
            output, checksums = publish.build_assets(original, "v1.0.0.13")
            self.assertEqual(output.name, "pcble2gamepad-1.0.0.13-x86_64.AppImage")
            self.assertEqual(checksums.name, "SHA256SUMS")
            self.assertTrue(checksums.read_text().endswith(f"  {output.name}\n"))
            self.assertEqual(output.read_bytes(), original.read_bytes())


if __name__ == "__main__":
    unittest.main()
