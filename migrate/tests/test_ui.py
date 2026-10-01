import io
import unittest

import os
from pathlib import Path
import tempfile
from unittest import mock

from omarchy_import import ui as ui_module
from omarchy_import.ui import UI, distinct_labels, one_line, printable


class PrintableTests(unittest.TestCase):
    def test_names_that_are_not_utf8_do_not_crash_output(self):
        name = b"caf\xe9.txt".decode("utf-8", "surrogateescape")
        self.assertEqual(printable(name), "caf\ufffd.txt")
        stream = io.TextIOWrapper(io.BytesIO(), encoding="utf-8", errors="strict")
        ui = UI(interactive=False, use_gum=False, stream=stream)
        ui.say(f"copy {name}")
        ui.progress(1, 1, name)
        stream.flush()
        self.assertIn("caf\ufffd.txt".encode(), stream.buffer.getvalue())


    def test_control_characters_never_reach_the_terminal(self):
        name = "notes\x1b]0;owned\x07\x1b[2J\r\x9b31m\u202etxt.exe"
        shown = printable(f"copy {name}\nnext line\tok")
        for character in ("\x1b", "\x07", "\r", "\x9b", "\u202e"):
            self.assertNotIn(character, shown)
        self.assertIn("\nnext line\tok", shown)
        stream = io.StringIO()
        UI(interactive=False, use_gum=False, stream=stream).say(f"copy {name}")
        self.assertNotIn("\x1b", stream.getvalue())
        self.assertEqual(one_line("a\nb\tc"), "a\ufffdb\ufffdc")


class ChoiceTests(unittest.TestCase):
    def test_labels_are_single_distinct_lines(self):
        # A folder named after another row, with a line break, must not
        # select that row too.
        labels = distinct_labels(["Projects folder (60 bytes)\nKeys and sign-ins",
                                  "Keys and sign-ins", "a, b", "x", "x"])
        self.assertEqual(len(set(labels)), 5)
        self.assertTrue(all("\n" not in label and "," not in label for label in labels))
        self.assertEqual(labels[1], "Keys and sign-ins")
        self.assertEqual(distinct_labels(["L (2)", "L", "L"]), ["L (2) (1)", "L (2) (2)", "L (3)"])

    def test_gum_comes_from_the_system_folders(self):
        with tempfile.TemporaryDirectory() as scratch:
            fake = Path(scratch) / "gum"
            fake.write_text("#!/bin/sh\ntouch " + scratch + "/ran\n")
            fake.chmod(0o755)
            with mock.patch.dict(os.environ, {"PATH": f"{scratch}:{os.environ.get('PATH', '')}"}):
                ui = UI(interactive=False, use_gum=True, stream=io.StringIO())
            self.assertNotEqual(ui.gum_path, str(fake))
            if ui.gum_path is None:
                self.assertFalse(ui.gum)
            self.assertEqual(ui_module.find_tool("gum", (scratch,)), str(fake))


if __name__ == "__main__":
    unittest.main()
