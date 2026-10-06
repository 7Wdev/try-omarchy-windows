from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("release-notes.py")


class ReleaseNotesTests(unittest.TestCase):
    def run_notes(
        self, root: Path, tag: str, *extra: str, console_encoding: str = "utf-8"
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                tag,
                "--notes-dir",
                str(root / "release-notes"),
                *extra,
            ],
            env=dict(os.environ, PYTHONIOENCODING=console_encoding),
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )

    def test_prefers_concise_release_notes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "release-notes").mkdir()
            (root / "release-notes/v1.2.3-preview.md").write_text(
                "Short release notes.\n", encoding="utf-8"
            )
            (root / "CHANGELOG.md").write_text(
                "## v1.2.3-preview\n\nLong changelog.\n", encoding="utf-8"
            )

            result = self.run_notes(root, "v1.2.3-preview")

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "Short release notes.\n")

    def test_non_ascii_notes_with_cp1252_console(self) -> None:
        body = "Try Omarchy v1.2.3.\n\n# Fixes\n\n- Clipboard images work. 🎉\n\nThanks, 世界!\n"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "release-notes").mkdir()
            (root / "release-notes/v1.2.3.md").write_text(body, encoding="utf-8")
            for extra in ((), ("--output", str(root / "published.md"))):
                with self.subTest(extra=extra):
                    result = self.run_notes(root, "v1.2.3", *extra, console_encoding="cp1252")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if extra:
                        self.assertEqual((root / "published.md").read_bytes(), body.encode("utf-8"))
                        self.assertEqual(result.stdout, "")
                    else:
                        self.assertEqual(result.stdout, body)

    def test_empty_announcement_does_not_create_output(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "release-notes").mkdir()
            (root / "release-notes/v1.2.3.md").write_text(" \n", encoding="utf-8")
            output = root / "published.md"
            result = self.run_notes(root, "v1.2.3", "--output", str(output))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("is empty", result.stderr)
            self.assertFalse(output.exists())

    def test_missing_announcement_rejects_changelog_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "release-notes").mkdir()
            (root / "CHANGELOG.md").write_text(
                "# Changelog\n\n"
                "## v1.2.3-preview - today\n\n"
                "Release changes.\n\n"
                "## v1.2.2-preview\n\nOld changes.\n",
                encoding="utf-8",
            )

            result = self.run_notes(root, "v1.2.3-preview")

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Missing reviewed release notes", result.stderr)
            self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
