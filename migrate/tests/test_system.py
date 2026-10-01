"""Commands only ever come from the system's folders.

These tests use the real Runner against a folder of harmless stand-ins for
the system's tools. Nothing here runs the real sudo, a package manager or a
desktop command.
"""

import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from omarchy_import import attach, cli
from omarchy_import.system import Runner, find_tool
from omarchy_import.ui import UI
from tests import fixtures
from tests.test_cli import CliCase

# The stand-in sudo records its arguments and runs the command after --.
SUDO = """#!/bin/sh
echo "system sudo $*" >> "{log}"
if [ "$1" = "--" ]; then shift; exec "$@"; fi
"""
TOOL = """#!/bin/sh
echo "system {name} $*" >> "{log}"
"""
OUTPUTS = {
    "pacman": 'case "$1" in -Sl) echo "extra cowsay 3.8-1";; esac\n',
    "losetup": 'case "$*" in *--show*) echo /dev/loop99;; esac\n',
    # Like the real one, it calls other commands through PATH.
    "omarchy-theme-set": "hyprctl reload\n",
}
SYSTEM_TOOLS = ("pacman", "yay", "mise", "omarchy-theme-set", "omarchy-theme-bg-set", "hyprctl",
                "mount", "umount", "losetup", "dmsetup", "id", "getent", "systemctl")
IMPORTED = ("sudo", "pacman", "yay", "mise", "omarchy-theme-set", "hyprctl", "mount", "umount",
            "losetup", "dmsetup", "git", "gum", "less")


def executable(path, text):
    path.write_text(text)
    path.chmod(0o755)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.base = Path(scratch.name)
        self.log = self.base / "log"
        self.system = self.base / "system"
        self.elsewhere = self.base / "elsewhere"
        self.system.mkdir()
        self.elsewhere.mkdir()
        executable(self.system / "sudo", SUDO.format(log=self.log))
        executable(self.system / "tool", TOOL.format(name="tool", log=self.log))
        executable(self.elsewhere / "tool", "#!/bin/sh\necho elsewhere >> " + str(self.log) + "\n")
        executable(self.elsewhere / "only-elsewhere", "#!/bin/sh\necho elsewhere >> "
                   + str(self.log) + "\n")
        path = mock.patch.dict(os.environ, {"PATH": f"{self.elsewhere}:{self.system}"})
        path.start()
        self.addCleanup(path.stop)
        self.runner = Runner(directories=[str(self.system)])

    def lines(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_tools_are_found_in_the_system_folders_only(self):
        self.assertEqual(self.runner.which("tool"), str(self.system / "tool"))
        self.assertIsNone(self.runner.which("only-elsewhere"))
        self.assertIsNone(self.runner.which(str(self.elsewhere / "tool")))
        self.assertIsNone(find_tool("../elsewhere/tool", [str(self.system)]))
        self.runner.run(["tool", "a"])
        self.assertEqual(self.lines(), ["system tool a"])
        with self.assertRaises(FileNotFoundError):
            self.runner.run(["only-elsewhere"])
        self.assertEqual(self.lines(), ["system tool a"])

    def test_sudo_and_the_command_under_it_are_full_paths(self):
        with mock.patch("os.geteuid", return_value=1000):
            self.runner.run(["tool", "b"], sudo=True)
        self.assertEqual(self.lines(), [f"system sudo -- {self.system}/tool b", "system tool b"])

    def test_commands_get_a_path_of_the_system_folders(self):
        executable(self.system / "show-path", '#!/bin/sh\necho "$PATH"\n')
        self.assertEqual(self.runner.run(["show-path"]).stdout.strip(), str(self.system))
        result = self.runner.run(["show-path"], env={"PATH": str(self.elsewhere)})
        self.assertEqual(result.stdout.strip(), str(self.system))


@unittest.skipIf(os.geteuid() == 0, "the importer refuses to run as root")
class ImportedHelpersTests(CliCase):
    """The review's probe: a trial brings helpers named like the tools the
    import runs afterwards, and PATH puts ~/.local/bin first."""

    def setUp(self):
        super().setUp()
        self.log = self.base / "log"
        self.system = self.base / "system"
        self.system.mkdir()
        executable(self.system / "sudo", SUDO.format(log=self.log))
        for name in SYSTEM_TOOLS:
            executable(self.system / name, TOOL.format(name=name, log=self.log)
                       + OUTPUTS.get(name, ""))
        for name in IMPORTED:
            fixtures.write(self.trial_home / ".local/bin" / name,
                           f'#!/bin/sh\necho "imported {name} $*" >> "{self.log}"\n'.encode(),
                           fixtures.TRIAL_EDIT, 0o755)
        path = mock.patch.dict(os.environ, {
            "PATH": f"{self.home}/.local/bin:{self.system}:/usr/bin:/bin"})
        path.start()
        self.addCleanup(path.stop)
        self.runner = Runner(directories=[str(self.system)])

    def lines(self):
        return self.log.read_text().splitlines()

    def test_follow_up_steps_never_run_an_imported_helper(self):
        status, output = self.main("--yes", "--select", "settings,packages,theme")
        self.assertEqual(status, 0, output)
        # The import really brought them.
        for name in IMPORTED:
            self.assertTrue(os.access(self.home / ".local/bin" / name, os.X_OK), name)
        # Mounting and cleaning up afterwards, as a run from a disk does.
        workdir = self.base / "mounts"
        workdir.mkdir()
        session = attach.Session(self.runner, workdir)
        session.attach_disk(self.base / "disk.raw")
        session.finish()
        with mock.patch.object(attach, "remove_scratch_files", return_value=0):
            cli.cleanup(UI(interactive=False, stream=io.StringIO()), self.runner)
        lines = self.lines()
        self.assertEqual([line for line in lines if line.startswith("imported")], [])
        system = self.system
        for expected in (
                f"system sudo -- {system}/pacman -S --needed --noconfirm -- cowsay",
                "system pacman -S --needed --noconfirm -- cowsay",
                "system yay -S --needed --noconfirm -- figlet",
                "system sudo -k",
                "system mise install --yes",
                "system omarchy-theme-set gruvbox",
                "system hyprctl reload",
                f"system sudo -- {system}/umount {workdir}/trial-1",
                f"system sudo -- {system}/losetup --detach /dev/loop99",
                f"system sudo -- {system}/dmsetup ls"):
            self.assertIn(expected, lines)
        forget = lines.index("system sudo -k")
        self.assertLess(lines.index("system yay -S --needed --noconfirm -- figlet"), forget)
        self.assertLess(forget, lines.index("system mise install --yes"))
        self.assertLess(forget, lines.index("system omarchy-theme-set gruvbox"))


if __name__ == "__main__":
    unittest.main()
