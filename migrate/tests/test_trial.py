import contextlib
import io
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest
from unittest import mock

from omarchy_import import safefs
from omarchy_import import trial as trial_module
from omarchy_import.trial import Trial, TrialError
from tests import fixtures


class TrialTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.base = Path(scratch.name)
        self.root, self.home = fixtures.make_trial(self.base, user="ada", uid=1000, gid=1000)

    def test_account_version_and_theme(self):
        trial = Trial(self.root)
        self.assertEqual(trial.account.name, "ada")
        self.assertEqual(trial.account.home, "/home/ada")
        self.assertIn("docker", trial.account.groups)
        self.assertEqual(trial.omarchy_version(), "4.0.3")
        self.assertEqual(trial.theme(), "gruvbox")
        marker = (self.home / ".local/state/omarchy/done/finalize-user").stat()
        self.assertEqual(trial.baseline_ns(), max(marker.st_ctime_ns, marker.st_mtime_ns))
        self.assertTrue(trial.is_try)
        self.assertEqual(trial.share_names(), {"Windows"})

    def test_a_chosen_account_keeps_its_groups(self):
        trial = Trial(self.root)
        trial.choose("ada")
        self.assertIn("docker", trial.account.groups)

    def test_packages_added_on_top_of_the_image(self):
        packages = Trial(self.root).packages()
        self.assertEqual(sorted(packages.added), ["cowsay", "figlet"])
        self.assertNotIn("zlib", packages.explicit)

    def test_only_services_the_user_enabled_are_reported(self):
        # The fixture enabled docker before the account was set up, like the image.
        self.assertEqual(Trial(self.root).enabled_services(), [])
        fixtures.link(self.root / "etc/systemd/system/multi-user.target.wants/libvirtd.service",
                      "/usr/lib/systemd/system/libvirtd.service")
        fixtures.link(self.root / "etc/systemd/system/multi-user.target.wants/try-omarchy-x.service",
                      "/etc/systemd/system/try-omarchy-x.service")
        self.assertEqual(Trial(self.root).enabled_services(), ["libvirtd.service"])

    def test_disk_without_an_account(self):
        (self.root / "etc/passwd").write_text("root:x:0:0::/root:/bin/bash\n")
        with self.assertRaisesRegex(TrialError, "no user account"):
            Trial(self.root)

    def test_unknown_account(self):
        with self.assertRaisesRegex(TrialError, "no account named bob"):
            Trial(self.root, "bob")

    def test_not_an_omarchy_disk(self):
        with self.assertRaises(TrialError):
            Trial(self.base / "missing")

    def test_passwd_parsing_ignores_system_accounts(self):
        people = trial_module.people(
            "root:x:0:0::/root:/bin/bash\n"
            "svc:x:1001:1001::/var/lib/svc:/bin/bash\n"
            "ghost:x:1002:1002::/home/ghost:/usr/bin/nologin\n"
            "ada:x:1000:1000::/home/ada:/bin/zsh\n"
            "broken line\n")
        self.assertEqual([account.name for account in people], ["ada"])

    def test_units_with_shell_syntax_are_not_suggested(self):
        fixtures.link(self.root / "etc/systemd/system/multi-user.target.wants/"
                      "x$(touch pwned).service", "/usr/lib/systemd/system/x.service")
        fixtures.link(self.root / "etc/systemd/system/multi-user.target.wants/libvirtd.service",
                      "/usr/lib/systemd/system/libvirtd.service")
        self.assertEqual(Trial(self.root).enabled_services(), ["libvirtd.service"])

    def test_a_theme_name_that_is_not_a_theme_is_ignored(self):
        fixtures.write(self.home / ".local/state/omarchy/current/theme.name", b"--help\n",
                       fixtures.TRIAL_EDIT)
        self.assertIsNone(Trial(self.root).theme())

    def test_pacman_desc(self):
        self.assertEqual(trial_module.parse_pacman_desc("%NAME%\nvim\n\n%REASON%\n1\n"),
                         ("vim", False))
        self.assertEqual(trial_module.parse_pacman_desc("%NAME%\nvim\n"), ("vim", True))


class ContainmentTests(unittest.TestCase):
    """Nothing in the trial's metadata can point a read outside the trial."""

    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.base = Path(scratch.name)
        self.root, self.home = fixtures.make_trial(self.base, user="ada", uid=1000, gid=1000)
        # Private files next to the trial that it must never reach.
        self.outside = self.base / "outside"
        fixtures.write(self.outside / ".config/secret", b"private\n", fixtures.TRIAL_EDIT)
        fixtures.write(self.outside / "ada/.config/secret", b"private\n", fixtures.TRIAL_EDIT)
        fixtures.write(self.outside / "passwd", b"ada:x:1000:1000::/home/ada:/bin/bash\n",
                       fixtures.SKEL_TIME)

    def passwd(self, home):
        (self.root / "etc/passwd").write_text(f"ada:x:1000:1000::{home}:/bin/bash\n")

    def test_the_traversal_from_the_review_is_refused(self):
        self.passwd("/home/../../outside")
        self.assertTrue((self.root / "home/../../outside").is_dir())
        with self.assertRaisesRegex(TrialError, "no user account"):
            Trial(self.root)

    def test_homes_with_dot_parts_or_outside_home_are_refused(self):
        for home in ("/home/ada/..", "/home/./ada", "/home//ada", "/outside", "/home",
                     "home/ada", "/home/ada\x1b"):
            self.passwd(home)
            with self.assertRaises(TrialError, msg=home):
                Trial(self.root)

    def test_a_home_link_cannot_leave_the_trial(self):
        shutil.rmtree(self.root / "home")
        for target in (str(self.outside), "../outside", "../../outside", "/../outside"):
            with self.subTest(target=target):
                if (self.root / "home").is_symlink():
                    (self.root / "home").unlink()
                os.symlink(target, self.root / "home")
                with self.assertRaisesRegex(TrialError, "no user account"):
                    Trial(self.root)

    def test_a_symlinked_home_folder_is_refused(self):
        moved = self.root / "home/ada-real"
        (self.root / "home/ada").rename(moved)
        os.symlink("ada-real", self.root / "home/ada")
        with self.assertRaisesRegex(TrialError, "no user account"):
            Trial(self.root)

    def test_links_inside_the_trial_still_work(self):
        (self.root / "var").mkdir(exist_ok=True)
        (self.root / "home").rename(self.root / "var/home")
        os.symlink("/var/home", self.root / "home")
        trial = Trial(self.root)
        self.assertEqual(trial.home, self.root / "var/home/ada")
        self.assertEqual(trial.theme(), "gruvbox")
        self.assertIsNotNone(trial.baseline_ns())

    def test_metadata_folders_that_link_outside_are_not_read(self):
        # etc links to a folder holding a passwd file of its own.
        (self.root / "etc").rename(self.root / "etc-real")
        os.symlink(str(self.outside), self.root / "etc")
        with self.assertRaises(TrialError):
            Trial(self.root)
        (self.root / "etc").unlink()
        (self.root / "etc-real").rename(self.root / "etc")

        local = self.root / "var/lib/pacman/local"
        shutil.rmtree(local)
        fixtures.write(self.outside / "local/htop-1-1/desc", b"%NAME%\nhtop\n", fixtures.SKEL_TIME)
        os.symlink(str(self.outside / "local"), local)
        self.assertEqual(Trial(self.root).packages().added, [])
        local.unlink()
        fixtures.write(local / "evil-1/desc", b"", fixtures.SKEL_TIME)
        (local / "evil-1/desc").unlink()
        os.symlink(str(self.outside / "local/htop-1-1/desc"), local / "evil-1/desc")
        self.assertEqual(Trial(self.root).packages().added, [])

        state = self.home / ".local/state/omarchy/current"
        shutil.rmtree(state)
        fixtures.write(self.outside / "current/theme.name", b"outside\n", fixtures.TRIAL_EDIT)
        os.symlink(str(self.outside / "current"), state)
        self.assertIsNone(Trial(self.root).theme())

    def test_a_skeleton_link_cannot_be_read_through(self):
        skel = self.root / "etc/skel"
        shutil.rmtree(skel / ".config")
        os.symlink(str(self.outside / ".config"), skel / ".config")
        source = safefs.Source(Trial(self.root).skel)
        self.assertIsNone(source.lstat(".config/secret"))
        self.assertIsNone(source.read(".config/secret"))
        self.assertIsNotNone(source.lstat(".bashrc"))
        shutil.rmtree(self.root / "etc/skel")
        os.symlink(str(self.outside), self.root / "etc/skel")
        trial = Trial(self.root)
        self.assertIsNone(trial.skel)
        self.assertIsNone(safefs.Source(trial.skel).read(".config/secret"))

    def test_an_extracted_export_is_read_like_a_disk(self):
        from omarchy_import import export
        destination = self.base / "dest"
        destination.mkdir()
        with mock.patch.object(export, "running_programs", return_value=set()), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(export.main(["--root", str(self.root), "--yes", "--select",
                                          "defaults", str(destination)]), 0)
        archive = next(destination.glob("omarchy-export-*.tar.gz"))
        with tarfile.open(archive) as tar:
            tar.extractall(self.base / "extracted", filter="fully_trusted")
        root = next((self.base / "extracted").iterdir()) / "trial-root"
        trial = Trial(root)
        self.assertEqual(trial.account.name, "ada")
        self.assertEqual(trial.home, root / "home/ada")
        self.assertEqual(sorted(trial.packages().added), ["cowsay", "figlet"])


if __name__ == "__main__":
    unittest.main()
