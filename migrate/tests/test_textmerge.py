import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from omarchy_import import textmerge
from tests import fixtures


class StripTests(unittest.TestCase):
    def test_pinch_and_qemu_blocks_are_removed(self):
        self.assertEqual(textmerge.strip_try_blocks(fixtures.INPUT + b"\n" + fixtures.PINCH_BLOCK),
                         fixtures.INPUT)
        self.assertEqual(textmerge.strip_try_blocks(fixtures.MONITORS + b"\n" + fixtures.QEMU_BLOCK),
                         fixtures.MONITORS)

    def test_legacy_pinch_loader_is_removed(self):
        legacy = (fixtures.INPUT + b"\n" + textmerge.LEGACY_PINCH_LINES[0] + b"\n"
                  + textmerge.LEGACY_PINCH_LINES[1] + b"\n")
        self.assertEqual(textmerge.strip_try_blocks(legacy), fixtures.INPUT)

    def test_unterminated_block_is_left_alone(self):
        data = fixtures.INPUT + b"-- BEGIN TRY OMARCHY PINCH DEVICE\nlocal x = 1\n"
        self.assertEqual(textmerge.strip_try_blocks(data), data)

    def test_text_without_blocks_is_unchanged(self):
        data = b"no trailing newline"
        self.assertEqual(textmerge.strip_try_blocks(data), data)

    def test_marker_in_the_middle_of_a_line_is_ignored(self):
        data = b"x = 1 -- BEGIN TRY OMARCHY PINCH DEVICE\n-- END TRY OMARCHY PINCH DEVICE\n"
        self.assertEqual(textmerge.strip_try_blocks(data), data)


class RewriteTests(unittest.TestCase):
    def test_paths_are_rewritten_only_at_name_boundaries(self):
        rewriter = textmerge.Rewriter("/home/omarchy", "/home/ada")
        self.assertEqual(rewriter.text(b"cd /home/omarchy/Work; ls /home/omarchy\n"),
                         b"cd /home/ada/Work; ls /home/ada\n")
        self.assertEqual(rewriter.text(b"/home/omarchy2/x /home/omarchy.old"),
                         b"/home/omarchy2/x /home/omarchy.old")
        self.assertEqual(rewriter.path("/home/omarchy/.local/x"), "/home/ada/.local/x")
        self.assertEqual(rewriter.path("/home/omarchyx"), "/home/omarchyx")

    def test_same_home_is_left_alone(self):
        rewriter = textmerge.Rewriter("/home/ada", "/home/ada")
        self.assertFalse(rewriter.active)
        self.assertEqual(rewriter.text(b"/home/ada/x"), b"/home/ada/x")

    def test_bookmarks_lose_the_windows_share(self):
        data = (b"file:///home/omarchy/Downloads Downloads\nfile:///mnt/host Windows\n"
                b"file:///home/omarchy/Work Work\nfile:///home/omarchy/Workbench\n")
        self.assertEqual(textmerge.clean_bookmarks(data, "/home/omarchy", {"Work"}),
                         b"file:///home/omarchy/Downloads Downloads\n"
                         b"file:///home/omarchy/Workbench\n")


class ListMergeTests(unittest.TestCase):
    def test_append_lists_keep_this_computer_first_without_duplicates(self):
        merged = textmerge.merge_lists(".ssh/known_hosts", b"a\nb\n", b"b\nc\n")
        self.assertEqual(merged, b"b\nc\na\n")

    def test_history_puts_the_trial_first(self):
        merged = textmerge.merge_lists(".bash_history", b"ls\nls\n", b"pwd\n")
        self.assertEqual(merged, b"ls\nls\npwd\n")

    def test_history_is_not_repeated_on_a_second_import(self):
        first = textmerge.merge_lists(".bash_history", b"ls\ncd x\n", b"pwd\n")
        second = textmerge.merge_lists(".bash_history", b"ls\ncd x\necho new\n", first)
        self.assertEqual(second, b"ls\ncd x\necho new\npwd\n")


@unittest.skipUnless(textmerge.git_available(), "git is needed for three-way merges")
class Merge3Tests(unittest.TestCase):
    def test_clean_merge_combines_both_sides(self):
        merged, clean = textmerge.merge3(
            fixtures.TRIAL_BINDINGS, fixtures.TRIAL_BINDINGS + b'bind("SUPER", "N", "notes")\n',
            fixtures.NEW_BINDINGS)
        self.assertTrue(clean)
        self.assertEqual(merged, fixtures.NEW_BINDINGS + b'bind("SUPER", "N", "notes")\n')

    def test_addition_next_to_a_changed_default_keeps_both(self):
        # A user appends right after a line the newer default rewrote; git
        # calls touching changes a conflict.
        base = b"-- header\nkeep\nold example\n"
        ours = base + b"\nbind(\"SUPER + N\")\n"
        theirs = b"-- new header\nkeep\nnew example\n"
        merged, clean = textmerge.merge3(base, ours, theirs)
        self.assertFalse(clean)
        self.assertEqual(merged, b"-- new header\nkeep\nold example\n\nbind(\"SUPER + N\")\n")

    def test_same_line_changed_on_both_sides_keeps_the_trial_version(self):
        self.assertEqual(textmerge.merge3(b"a\n", b"b\n", b"c\n"), (b"b\n", False))

    def test_binary_is_not_merged(self):
        self.assertEqual(textmerge.merge3(b"a\0", b"b\0", b"c\0"), (None, False))

    def test_an_imported_git_earlier_in_path_is_not_used(self):
        with tempfile.TemporaryDirectory() as scratch:
            fake = Path(scratch) / "git"
            fake.write_text(f"#!/bin/sh\ntouch {scratch}/ran\necho fake\n")
            fake.chmod(0o755)
            with mock.patch.dict(os.environ, {"PATH": f"{scratch}:{os.environ.get('PATH', '')}"}):
                self.assertTrue(textmerge.git_available())
                self.assertEqual(textmerge.merge3(b"a\nb\nc\nd\n", b"a\nb\nc\nD\n",
                                                  b"A\nb\nc\nd\n"),
                                 (b"A\nb\nc\nD\n", True))
            self.assertFalse((Path(scratch) / "ran").exists())


class KeyringTests(unittest.TestCase):
    NATIVE_ITEM = b"""
[1]
item-type=0
display-name=Chromium Safe Storage
secret=native-secret
mtime=1
ctime=1

[1:attribute0]
name=application
type=string
value=chromium

[2]
item-type=0
display-name=Wi-Fi
secret=hunter2
mtime=1
ctime=1

[2:attribute0]
name=ssid
type=string
value=home
"""

    def test_this_computers_item_stays_unless_its_browser_comes_over(self):
        kept = textmerge.merge_keyrings(fixtures.KEYRING_CHROMIUM,
                                        fixtures.KEYRING_EMPTY + self.NATIVE_ITEM).decode()
        self.assertIn("secret=native-secret", kept)
        self.assertNotIn("trial-secret", kept)
        self.assertIn("secret=hunter2", kept)

    def test_trial_items_win_for_imported_browsers_and_native_only_items_stay(self):
        merged = textmerge.merge_keyrings(fixtures.KEYRING_CHROMIUM,
                                          fixtures.KEYRING_EMPTY + self.NATIVE_ITEM, {"chromium"})
        text = merged.decode()
        self.assertIn("secret=trial-secret", text)
        self.assertNotIn("native-secret", text)
        self.assertIn("secret=hunter2", text)
        self.assertIn("[2:attribute0]", text)
        _, items = textmerge.parse_keyring(merged)
        self.assertEqual([number for number, _ in items], ["1", "2"])

    def test_merge_into_an_empty_keyring(self):
        merged = textmerge.merge_keyrings(fixtures.KEYRING_CHROMIUM, fixtures.KEYRING_EMPTY)
        self.assertEqual(len(textmerge.keyring_items(merged)), 1)
        self.assertTrue(merged.startswith(b"[keyring]\n"))

    def test_encrypted_keyrings_are_recognised(self):
        self.assertTrue(textmerge.keyring_is_encrypted(b"GnomeKeyring\n\r\0\n"))
        self.assertFalse(textmerge.keyring_is_encrypted(fixtures.KEYRING_EMPTY))


if __name__ == "__main__":
    unittest.main()
