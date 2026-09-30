import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from omarchy_import import cli
from omarchy_import.plan import Group, Inventory
from tests import fixtures


class CliCase(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.base = Path(scratch.name)
        self.root, self.trial_home = fixtures.make_trial(self.base)
        self.home, self.skel = fixtures.make_home(self.base)
        patcher = mock.patch.dict(os.environ, {"HOME": str(self.home),
                                               "TRY_OMARCHY_IMPORT_SKEL": str(self.skel)})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.runner = fixtures.FakeRunner({
            "pacman -Qq": "base\nomarchy\n",
            "pacman -Sl": "extra cowsay 3.8-1\n",
            "id -Gn": "ada wheel\n",
        }, programs={"pacman", "yay", "omarchy-theme-set", "id", "getent", "systemctl"})

    def main(self, *arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            status = cli.main(["--root", str(self.root), *arguments], runner=self.runner)
        return status, output.getvalue()


class CliTests(CliCase):
    def test_dry_run_json_lists_groups_and_changes_nothing(self):
        before = sorted(path.relative_to(self.home) for path in self.home.rglob("*"))
        status, output = self.main("--dry-run", "--json", "--select", "defaults")
        self.assertEqual(status, 0)
        document = json.loads(output[output.index("{"):])
        ids = [group["id"] for group in document["available"]]
        self.assertIn("settings", ids)
        self.assertIn("browser/chromium", ids)
        self.assertNotIn("browser/chromium", document["groups"])
        self.assertNotIn("keys", document["groups"])
        self.assertEqual(document["packages"]["repo"], ["cowsay"])
        self.assertEqual(document["theme"], "gruvbox")
        after = sorted(path.relative_to(self.home) for path in self.home.rglob("*"))
        self.assertEqual(before, after)

    def test_import_installs_packages_and_sets_the_theme(self):
        status, output = self.main("--yes", "--select", "settings,packages,theme")
        self.assertEqual(status, 0, output)
        self.assertIn(b"name = Ada", (self.home / ".config/git/config").read_bytes())
        commands = self.runner.commands()
        self.assertIn("pacman -S --needed --noconfirm cowsay", commands)
        self.assertIn("yay -S --needed --noconfirm figlet", commands)
        self.assertIn("omarchy-theme-set gruvbox", commands)
        self.assertIn("Log out and back in", output)

    def test_mise_runs_only_when_its_config_came_over(self):
        self.runner.programs.add("mise")
        self.main("--yes", "--select", "files")
        self.assertFalse(any(command.startswith("mise install") for command in
                             self.runner.commands()))
        self.main("--yes", "--select", "settings")
        self.assertIn("mise install --yes", self.runner.commands())

    def test_theme_is_left_alone_when_it_is_already_current(self):
        fixtures.write(self.home / ".local/state/omarchy/current/theme.name", b"gruvbox\n",
                       fixtures.HOME_EDIT)
        self.main("--yes", "--select", "theme")
        self.assertNotIn("omarchy-theme-set gruvbox", self.runner.commands())

    def test_incomplete_steps_make_the_exit_status_nonzero(self):
        self.runner.failing.add("yay -S")
        status, output = self.main("--yes", "--select", "packages")
        self.assertEqual(status, 1)
        self.assertIn("Not finished", output)

    def test_unknown_group_is_rejected(self):
        status, _ = self.main("--yes", "--select", "settings,nope")
        self.assertEqual(status, 2)

    def test_refuses_to_run_as_root(self):
        with mock.patch("os.geteuid", return_value=0):
            status, _ = self.main("--yes")
        self.assertEqual(status, 2)


class SelectionTests(unittest.TestCase):
    def inventory(self):
        groups = {}
        for group_id, kind, size in (("settings", "settings", 10), ("files/Big", "files", 900),
                                     ("files/Small", "files", 50), ("apps/Huge", "apps", 10**9),
                                     ("apps/Tiny", "apps", 5), ("browser/chromium", "browser", 5),
                                     ("keys", "keys", 1)):
            groups[group_id] = Group(group_id, kind, group_id.split("/")[-1], bytes=size, files=1)
        return Inventory(groups, [])

    def test_defaults_fit_the_free_space(self):
        free = cli.RESERVE_BYTES + 100
        rows = {row[0]: row[2] for row in cli.option_rows(self.inventory(), None, "tokyo", free)}
        self.assertTrue(rows["settings"])
        self.assertTrue(rows["files/Small"])
        self.assertFalse(rows["files/Big"])
        self.assertFalse(rows["apps/Huge"])
        self.assertTrue(rows["apps/Tiny"])
        self.assertFalse(rows["browser/chromium"])
        self.assertFalse(rows["keys"])
        self.assertTrue(rows["theme"])

    def test_keywords(self):
        rows = cli.option_rows(self.inventory(), None, None, 10**12)
        self.assertEqual(cli.resolve_selection("files,keys", rows),
                         {"files/Big", "files/Small", "keys"})
        self.assertEqual(cli.resolve_selection("all", rows), {row[0] for row in rows})
        self.assertNotIn("keys", cli.resolve_selection("defaults", rows))
        with self.assertRaises(cli.Stop):
            cli.resolve_selection("bogus", rows)


if __name__ == "__main__":
    unittest.main()
