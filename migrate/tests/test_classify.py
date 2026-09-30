import unittest

from omarchy_import import classify
from omarchy_import.classify import APPS, BROWSER, FILES, KEYS, SETTINGS, SKIP, Classifier, Place


class PlaceTests(unittest.TestCase):
    def setUp(self):
        self.classifier = Classifier({"obsidian"})

    def place(self, relative, kind="directory", target=None):
        return self.classifier.place(relative, kind, target)

    def test_visible_home_entries_are_files(self):
        self.assertEqual(self.place("Documents"), Place(FILES, "files/Documents", "Documents"))
        self.assertEqual(self.place("notes.txt", "file").group, classify.LOOSE_FILES_GROUP)

    def test_share_link_is_left_behind(self):
        self.assertEqual(self.place("Work", "symlink", "/mnt/host").kind, SKIP)
        self.assertEqual(self.place("Work", "symlink", "/mnt/host/sub").kind, SKIP)

    def test_dotfiles_and_tool_dirs(self):
        self.assertEqual(self.place(".bashrc", "file").kind, SETTINGS)
        self.assertEqual(self.place(".vim").kind, SETTINGS)
        self.assertEqual(self.place(".cache").kind, SKIP)
        self.assertEqual(self.place(".ssh").kind, KEYS)
        self.assertEqual(self.place(".netrc", "file").kind, KEYS)
        self.assertEqual(self.place(".mozilla"), Place(BROWSER, "browser/firefox", "Firefox"))
        self.assertEqual(self.place(".cargo"), Place(APPS, "apps/.cargo", "Rust (cargo)"))
        self.assertEqual(self.place(".unknown-tool").label, "unknown-tool")

    def test_config_children(self):
        self.assertEqual(self.place(".config/hypr").kind, SETTINGS)
        self.assertEqual(self.place(".config/obsidian").kind, SETTINGS)  # in skel
        self.assertEqual(self.place(".config/gh").kind, KEYS)
        self.assertEqual(self.place(".config/chromium").group, "browser/chromium")
        self.assertEqual(self.place(".config/mozilla"), Place(BROWSER, "browser/firefox-xdg", "Firefox"))
        self.assertEqual(self.place(".config/pulse").kind, SKIP)
        self.assertEqual(self.place(".config/Slack"), Place(APPS, "apps/.config/Slack", "Slack"))

    def test_local_children(self):
        self.assertEqual(self.place(".local/bin").kind, SETTINGS)
        self.assertEqual(self.classifier.place(".local/share", "directory").kind,
                         classify.CONTAINER)
        self.assertEqual(self.place(".local/share/applications").kind, SETTINGS)
        self.assertEqual(self.place(".local/share/keyrings").kind, KEYS)
        self.assertEqual(self.place(".local/share/nvim").kind, SKIP)
        self.assertEqual(self.place(".local/share/Steam").group, "apps/.local/share/Steam")
        self.assertEqual(self.place(".local/state/wireplumber").kind, SKIP)
        self.assertEqual(self.place(".local/state/omarchy/toggles").kind, SETTINGS)
        self.assertEqual(self.place(".local/state/omarchy/current").kind, SKIP)
        self.assertEqual(self.place(".var/app/com.spotify.Client").kind, APPS)


class OverrideTests(unittest.TestCase):
    def setUp(self):
        self.classifier = Classifier()
        self.settings = Place(SETTINGS, "settings")
        self.app = Place(APPS, "apps/.config/Code", "Code")

    def test_try_only_and_hardware_files(self):
        for relative in (".local/state/omarchy/toggles/suspend-off", ".config/hypr/monitors.lua",
                         ".ssh/authorized_keys",
                         ".local/share/applications/try-omarchy-windows-"
                         "0123456789abcdef0123456789abcdef.desktop"):
            self.assertEqual(self.classifier.override(relative, self.settings, "file").kind, SKIP,
                             relative)

    def test_caches_and_locks_inside_apps(self):
        self.assertEqual(self.classifier.override(".config/Code/Cache", self.app, "directory").kind,
                         SKIP)
        self.assertEqual(self.classifier.override(".config/Code/Service Worker/CacheStorage",
                                                  self.app, "directory").kind, SKIP)
        self.assertEqual(self.classifier.override(".config/Code/SingletonLock", self.app,
                                                  "symlink", "host-1").kind, SKIP)
        # Settings folders named like caches are kept.
        self.assertEqual(self.classifier.override(".config/hypr/Cache", self.settings,
                                                  "directory").kind, SETTINGS)

    def test_credentials_inside_app_folders_are_keys(self):
        self.assertEqual(self.classifier.override(".claude/.credentials.json",
                                                  Place(APPS, "apps/.claude"), "file").kind, KEYS)

    def test_special_files_and_try_links(self):
        self.assertEqual(self.classifier.override(".gnupg/S.gpg-agent", Place(KEYS, "keys"),
                                                  "special").kind, SKIP)
        self.assertEqual(self.classifier.override(".config/x", self.settings, "symlink",
                                                  "/usr/share/try-omarchy/pinch-input.lua").kind,
                         SKIP)


if __name__ == "__main__":
    unittest.main()
