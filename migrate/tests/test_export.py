import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

from omarchy_import import export
from tests import fixtures


class ExportCase(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.base = Path(scratch.name)
        self.root, self.trial_home = fixtures.make_trial(self.base)
        self.dest = self.base / "dest"
        self.dest.mkdir()

    def export(self, select="defaults"):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            status = export.main(["--root", str(self.root), "--yes", "--select", select,
                                  str(self.dest)])
        self.assertEqual(status, 0, output.getvalue())
        archives = list(self.dest.glob("omarchy-export-*.tar.gz"))
        self.assertEqual(len(archives), 1)
        self.assertEqual(list(self.dest.glob(".*partial")), [])
        return archives[0]

    @staticmethod
    def names(archive):
        with tarfile.open(archive) as tar:
            return {member.name.split("/", 1)[1] for member in tar.getmembers()
                    if "/" in member.name}

    def extract(self, archive):
        target = self.base / "extracted"
        with tarfile.open(archive) as tar:
            # Like GNU tar, which people use: the archive has absolute links.
            tar.extractall(target, filter="fully_trusted")
        return next(target.iterdir())


class ExportTests(ExportCase):
    def test_only_changes_and_their_defaults_are_packed(self):
        names = self.names(self.export())
        home = "trial-root/home/omarchy"
        for expected in ("import.sh", "try-omarchy-import.pyz", "README.txt", "export.json",
                         "trial-root/etc/passwd", f"{home}/.config/hypr/bindings.lua",
                         f"{home}/.bashrc", f"{home}/.config/git/config",
                         f"{home}/Documents/resume.md",
                         "trial-root/etc/skel/.config/hypr/bindings.lua",
                         "trial-root/usr/share/try-omarchy/packages.lock.txt",
                         f"{home}/.local/state/omarchy/current/theme.name",
                         "trial-root/var/lib/pacman/local/cowsay-0-0/desc"):
            self.assertIn(expected, names)
        for left_out in (f"{home}/.XCompose", f"{home}/.config/omarchy/branding/about.txt",
                         f"{home}/.config/hypr/monitors.lua",
                         f"{home}/.local/state/omarchy/toggles/suspend-off",
                         f"{home}/.local/state/omarchy/done/finalize-user",
                         f"{home}/Work/.mise.toml", f"{home}/.cache/thumbnails/a.png",
                         f"{home}/.ssh/id_ed25519", f"{home}/.config/chromium/Default/Bookmarks"):
            self.assertNotIn(left_out, names)

    def test_keys_and_browsers_only_when_picked_with_a_warning(self):
        archive = self.export("all")
        names = self.names(archive)
        self.assertIn("trial-root/home/omarchy/.ssh/id_ed25519", names)
        self.assertIn("trial-root/home/omarchy/.config/chromium/Default/Bookmarks", names)
        root = self.extract(archive)
        self.assertTrue(json.loads((root / "export.json").read_text())["secrets"])
        self.assertIn("keep it private", (root / "README.txt").read_text())
        self.assertEqual((root / "trial-root/home/omarchy/.ssh/id_ed25519").stat().st_mode & 0o777,
                         0o600)

    def test_nothing_is_left_when_the_export_fails(self):
        with mock.patch.object(export, "importer_archive", side_effect=OSError("disk full")):
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(OSError):
                export.main(["--root", str(self.root), "--yes", "--select", "settings",
                             str(self.dest)])
        self.assertEqual(list(self.dest.iterdir()), [])

    def test_round_trip_through_the_bundled_importer(self):
        root = self.extract(self.export("all"))
        home, skel = fixtures.make_home(self.base)
        environment = dict(os.environ, HOME=str(home), TRY_OMARCHY_IMPORT_SKEL=str(skel),
                           PATH="/usr/bin:/bin")
        result = subprocess.run(["bash", str(root / "import.sh"), "--yes", "--select",
                                 "settings,files,keys"], env=environment, capture_output=True,
                                text=True, timeout=120, stdin=subprocess.DEVNULL)
        self.assertIn("Imported", result.stdout, result.stderr)
        self.assertEqual((home / ".config/hypr/bindings.lua").read_bytes(),
                         fixtures.NEW_BINDINGS + b'bind("SUPER", "N", "notes")\n')
        self.assertIn(b"name = Ada", (home / ".config/git/config").read_bytes())
        self.assertEqual((home / "Documents/resume.md").read_bytes(), b"# Ada\n")
        self.assertTrue((home / ".ssh/id_ed25519").exists())
        self.assertFalse((home / ".XCompose").exists())
        bookmarks = (home / ".config/gtk-3.0/bookmarks").read_text()
        self.assertIn(f"file://{home}/Projects/site Site", bookmarks)
        self.assertNotIn("/mnt/host", bookmarks)


if __name__ == "__main__":
    unittest.main()
