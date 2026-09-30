import io
import unittest

from omarchy_import.ui import UI, printable


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


if __name__ == "__main__":
    unittest.main()
