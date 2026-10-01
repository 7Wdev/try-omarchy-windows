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
        self.assertEqual(runner.calls[0], (("pacman", "-S", "--needed", "--noconfirm", "--",
                                            "cowsay"), True))
        self.assertEqual(runner.calls[1], (("yay", "-S", "--needed", "--noconfirm", "--",
                                            "figlet"), False))
        self.assertEqual(runner.calls[2][0], ("flatpak", "install", "--noninteractive", "-y",
                                              "--user", "--", "flathub", "com.spotify.Client"))
        # yay runs without the yay, makepkg and git settings in the home
        # folder, which the import may just have brought.
        self.assertEqual(runner.yay_config, ["pacman", "pacman/makepkg.conf"])
        self.assertEqual(runner.environments[1]["GIT_CONFIG_GLOBAL"], "/dev/null")
        self.assertFalse(Path(runner.environments[1]["XDG_CONFIG_HOME"]).exists())

    def test_failures_are_reported_not_raised(self):
        runner = self.runner()
        runner.failing.update({"pacman -S", "yay -S", "flatpak install"})
        self.assertFalse(packages.install_repo(runner, ["cowsay"]).ok)
        self.assertFalse(packages.install_aur(runner, ["figlet"], True).ok)
        self.assertIn("com.example.App", packages.install_flatpaks(
            runner, [("com.example.App", "system")], True).detail)
        self.assertFalse(packages.install_aur(runner, ["figlet"], False).ok)

    def test_malformed_package_metadata_is_left_out(self):
        fixtures.write(self.root / "var/lib/pacman/local/controlled-1/desc",
                       b"%NAME%\n--makepkg=/tmp/controlled-demo-helper\n", fixtures.TRIAL_EDIT)
        fixtures.write(self.root / "var/lib/pacman/local/spaced-1/desc",
                       b"%NAME%\nfoo bar\n", fixtures.TRIAL_EDIT)
        fixtures.write(self.home / ".local/share/flatpak/app/--user/current/x", b"",
                       fixtures.TRIAL_EDIT)
        plan = packages.plan_packages(Trial(self.root), self.runner())
        self.assertEqual(plan.aur, ["figlet"])
        self.assertEqual(plan.invalid, 2)
        self.assertEqual(plan.flatpaks, [("com.spotify.Client", "user")])

    def test_installers_refuse_names_that_are_not_package_names(self):
        bad = ["--makepkg=/tmp/controlled-demo-helper", "-Syu", "/tmp/helper", "foo bar",
               "foo\nbar", "foo\x1b]0;title\x07"]
        for name in bad:
            runner = self.runner()
            for step in (packages.install_repo(runner, ["cowsay", name]),
                         packages.install_aur(runner, [name, "figlet"], True),
                         packages.install_flatpaks(runner, [("com.spotify.Client", "user"),
                                                            (name, "user")], True)):
                self.assertFalse(step.ok)
                self.assertNotIn(name, step.detail)
            self.assertEqual(runner.calls, [], name)
        runner = self.runner()
        self.assertFalse(packages.install_flatpaks(runner, [("--user", "system")], True).ok)
        self.assertEqual(runner.calls, [])

    def test_theme_names_that_are_not_theme_names_are_not_set(self):
        runner = self.runner()
        runner.programs.add("omarchy-theme-set")
        for theme in ("--help", "../../etc", "a b"):
            self.assertFalse(packages.set_theme(runner, theme).ok)
        self.assertEqual(runner.calls, [])
        self.assertTrue(packages.set_theme(runner, "tokyo-night").ok)
        self.assertEqual(runner.commands(), ["omarchy-theme-set tokyo-night"])

    def test_background_outside_the_home_and_omarchy_is_ignored(self):
        home = self.base / "home"
        fixtures.write(home / "Pictures/wall.png", b"png", fixtures.HOME_SETUP)
        for target in ("/etc/passwd", "/home/omarchy/../../etc/passwd",
                       "/home/omarchy/Pictures/../Pictures/wall.png", "Pictures/wall.png",
                       f"{home}/Pictures/wall.png"):
            self.assertIsNone(packages.background_path(target, "/home/omarchy", home), target)

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


class HyprlandCheckTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.home = Path(scratch.name)
        fixtures.write(self.home / ".config/hypr/hyprland.lua", b"-- config\n", fixtures.HOME_EDIT)

    def test_clean_config(self):
        runner = fixtures.FakeRunner({"Hyprland --verify-config":
                                      "======== Config parsing result:\n\nconfig ok\n"},
                                     programs={"Hyprland"})
        self.assertTrue(packages.verify_hyprland(runner, self.home).ok)

    def test_problem_is_reported(self):
        runner = fixtures.FakeRunner({"Hyprland --verify-config":
                                      "\x1b[1;31mERR\x1b[0m /home/ada/.config/hypr/bindings.lua:30: "
                                      "hl.bind: dispatcher must be a dispatcher\n"},
                                     programs={"Hyprland"})
        step = packages.verify_hyprland(runner, self.home)
        self.assertFalse(step.ok)
        self.assertIn("settings: /home/ada/.config/hypr/bindings.lua:30: hl.bind", step.detail)
        self.assertNotIn("\x1b", step.detail)

    def test_a_check_that_cannot_run_says_nothing(self):
        runner = fixtures.FakeRunner({"Hyprland --verify-config":
                                      "CRIT: Critical error thrown: XDG_RUNTIME_DIR is not set!\n"},
                                     programs={"Hyprland"})
        self.assertIsNone(packages.verify_hyprland(runner, self.home))

    def test_skipped_without_hyprland(self):
        self.assertIsNone(packages.verify_hyprland(fixtures.FakeRunner(), self.home))


if __name__ == "__main__":
    unittest.main()
