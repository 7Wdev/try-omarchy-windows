import unittest

from omarchy_import import names


class NameTests(unittest.TestCase):
    def test_real_package_names(self):
        for name in ("cowsay", "figlet", "lib32-glibc", "python-pip", "gtk2+", "libc++", "0ad",
                     "1password", "ttf-jetbrains-mono-nerd", "visual-studio-code-bin",
                     "r8168-dkms", "gst-plugins-ugly", "perl-text-csv_xs", "nodejs-lts-jod",
                     "@scope", "qt6-base", "xorg-xwayland"):
            self.assertTrue(names.package(name), name)

    def test_package_names_that_could_change_what_runs(self):
        for name in ("--makepkg=/tmp/controlled-demo-helper", "--pacman=/tmp/x", "-Syu", "-",
                     "--", "/tmp/helper", "./helper", "../helper", ".hidden", "foo bar",
                     "foo\nbar", "foo\tbar", "foo\x1b[31m", "foo;id", "foo$(id)", "foo/bar",
                     "", "x" * 256, None, 7):
            self.assertFalse(names.package(name), repr(name))

    def test_flatpak_ids(self):
        for app in ("com.spotify.Client", "org.mozilla.firefox", "md.obsidian.Obsidian",
                    "com.github._4lex4.ScanTailor-Advanced", "io.github.some_one.App-Name"):
            self.assertTrue(names.flatpak(app), app)
        for app in ("--user", "--installation=/tmp/x", "-org.a.b", "com.x", "org/a/b",
                    "org.a.b c", "org.3d.app", "../org.a.b", "org.a.b\n", "org..a.b", "", None):
            self.assertFalse(names.flatpak(app), repr(app))

    def test_units_and_themes(self):
        self.assertTrue(names.unit("libvirtd.service"))
        self.assertTrue(names.unit("syncthing@ada.service"))
        self.assertTrue(names.unit("fstrim.timer"))
        for unit in ("x$(touch pwned).service", "a b.service", "-x.service", "x.target",
                     "x\\x2d.service", "x;id.service"):
            self.assertFalse(names.unit(unit), unit)
        for theme in ("tokyo-night", "catppuccin-latte", "Rose_Pine", "my.theme"):
            self.assertTrue(names.theme(theme), theme)
        for theme in ("-x", "../../etc", "a/b", "..", "a b", "x\n"):
            self.assertFalse(names.theme(theme), theme)


if __name__ == "__main__":
    unittest.main()
