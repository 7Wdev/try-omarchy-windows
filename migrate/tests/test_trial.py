from pathlib import Path
import tempfile
import unittest

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

    def test_pacman_desc(self):
        self.assertEqual(trial_module.parse_pacman_desc("%NAME%\nvim\n\n%REASON%\n1\n"),
                         ("vim", False))
        self.assertEqual(trial_module.parse_pacman_desc("%NAME%\nvim\n"), ("vim", True))


if __name__ == "__main__":
    unittest.main()
