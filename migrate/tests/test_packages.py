from pathlib import Path
import tempfile
import unittest

from omarchy_import import packages
from omarchy_import.trial import Trial
from tests import fixtures


class PackagePlanTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.base = Path(scratch.name)
        self.root, self.home = fixtures.make_trial(self.base)
        fixtures.write(self.root / "var/lib/pacman/local/htop-1-1/desc",
                       b"%NAME%\nhtop\n", fixtures.SKEL_TIME)
        fixtures.write(self.home / ".local/share/flatpak/app/com.spotify.Client/current/x", b"",
                       fixtures.TRIAL_EDIT)
        fixtures.link(self.root / "etc/systemd/system/multi-user.target.wants/libvirtd.service",
                      "/usr/lib/systemd/system/libvirtd.service")

    def runner(self, **extra):
        outputs = {
            "pacman -Qq": "base\nomarchy\nhtop\n",
            "pacman -Sl": "extra cowsay 3.8-1\ncore base 3-1\nextra htop 3-1\n",
            "flatpak list": "",
            "systemctl list-unit-files": "sshd.service enabled enabled\n",
            "id -Gn": "ada wheel\n",
            "getent group": "wheel:x:998:ada\ndocker:x:970:\n",
        }
        outputs.update(extra)
        return fixtures.FakeRunner(outputs, programs={"pacman", "flatpak", "systemctl", "id",
                                                      "getent", "yay"})

    def test_plan_splits_repo_aur_and_installed(self):
        plan = packages.plan_packages(Trial(self.root), self.runner())
        self.assertEqual(plan.repo, ["cowsay"])
        self.assertEqual(plan.aur, ["figlet"])
        self.assertEqual(plan.installed, ["htop"])
        self.assertEqual(plan.flatpaks, [("com.spotify.Client", "user")])
        self.assertEqual(plan.services, ["libvirtd.service"])
        self.assertEqual(plan.groups, ["docker"])
        self.assertTrue(plan.has_yay)

    def test_install_commands(self):
        runner = self.runner()
        self.assertTrue(packages.install_repo(runner, ["cowsay"]).ok)
        self.assertTrue(packages.install_aur(runner, ["figlet"], True).ok)
        self.assertTrue(packages.install_flatpaks(runner, [("com.spotify.Client", "user")],
                                                  True).ok)
        self.assertEqual(runner.calls[0], (("pacman", "-S", "--needed", "--noconfirm", "cowsay"),
                                           True))
        self.assertEqual(runner.calls[1], (("yay", "-S", "--needed", "--noconfirm", "figlet"),
                                           False))
        self.assertIn("--user", runner.calls[2][0])

    def test_failures_are_reported_not_raised(self):
        runner = self.runner()
        runner.failing.update({"pacman -S", "yay -S", "flatpak install"})
        self.assertFalse(packages.install_repo(runner, ["cowsay"]).ok)
        self.assertFalse(packages.install_aur(runner, ["figlet"], True).ok)
        self.assertIn("com.x", packages.install_flatpaks(runner, [("com.x", "system")], True).detail)
        self.assertFalse(packages.install_aur(runner, ["figlet"], False).ok)

    def test_background_follows_the_theme_or_the_imported_file(self):
        home = self.base / "home"
        theme_file = home / ".local/state/omarchy/current/theme/backgrounds/1-gruvbox.jpg"
        fixtures.write(theme_file, b"jpg", fixtures.HOME_SETUP)
        self.assertEqual(packages.background_path(
            "/home/omarchy/.local/state/omarchy/current/theme/backgrounds/1-gruvbox.jpg",
            "/home/omarchy", home), theme_file)
        fixtures.write(home / "Pictures/wall.png", b"png", fixtures.HOME_SETUP)
        self.assertEqual(packages.background_path("/home/omarchy/Pictures/wall.png",
                                                  "/home/omarchy", home),
                         home / "Pictures/wall.png")
        self.assertIsNone(packages.background_path("/home/omarchy/missing.png", "/home/omarchy",
                                                   home))
        self.assertIsNone(packages.background_path(None, "/home/omarchy", home))


if __name__ == "__main__":
    unittest.main()
