import re
import unittest

from omarchy_import import report, selection
from omarchy_import.apply import Report
from omarchy_import.plan import Action, Group, Inventory, Plan

# What a count of one must never read as.
WRONG_SINGULAR = re.compile(r"\b1 (files|items|are|of Omarchy's defaults will be replaced with your "
                            r"versions)\b|\b1 file[^.]*\bwere\b")


def plan_with(count, resolution="trial"):
    actions = []
    for kind in ("create", "replace", "merge", "conflict", "same", "default", "skip"):
        actions += [Action(f"{kind}/{i}", "file", "settings", kind) for i in range(count)]
    return Plan(actions, [], ["settings"], resolution)


class SummaryWordingTests(unittest.TestCase):
    def test_one_of_each_reads_in_the_singular(self):
        for resolution in ("trial", "keep"):
            lines = report.plan_summary(plan_with(1, resolution))
            for line in lines:
                self.assertNotRegex(line, WRONG_SINGULAR)
            self.assertIn("1 new file or link will be copied", lines)
            self.assertIn("1 file will combine your changes with this computer's", lines)
            self.assertIn("1 file is still Omarchy's default, so this computer's newer one stays",
                          lines)
            self.assertIn("1 Try-only, cache or unsafe item stays behind", lines)

    def test_two_of_each_reads_in_the_plural(self):
        lines = report.plan_summary(plan_with(2))
        self.assertIn("2 new files and links will be copied", lines)
        self.assertIn("2 of Omarchy's defaults will be replaced with your versions", lines)
        self.assertIn("2 are already the same here", lines)
        self.assertIn("2 files you already changed on this computer will be replaced by the "
                      "trial's (this computer's copies go to the backup)", lines)

    def test_result_lines(self):
        result = Report("run", "/backups")
        result.add("a", "create", "done")
        result.add("b", "conflict", "done", "kept this computer's version; the trial's is b.x")
        lines = report.result_summary(result, [])
        self.assertTrue(lines[0].startswith("Imported 2 files"))
        self.assertTrue(lines[1].startswith("1 file from the trial was saved next to your newer "
                                            "version ("))
        single = Report("run", "/backups")
        single.add("a", "create", "done")
        self.assertTrue(report.result_summary(single, [])[0].startswith("Imported 1 file ("))

    def test_picker_rows(self):
        settings = Group("settings", "settings", "Settings and customizations", changed=1, bytes=10)
        keys = Group("keys", "keys", "Keys and sign-ins", changed=1)
        rows = selection.option_rows(Inventory({"settings": settings, "keys": keys}, []), None, None,
                                     10 ** 12)
        self.assertEqual(rows[0][1], "Settings and customizations (1 file 10 bytes)")
        self.assertTrue(rows[1][1].endswith("command line logins (1 file)"))


if __name__ == "__main__":
    unittest.main()
