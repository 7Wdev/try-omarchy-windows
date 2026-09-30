import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import unittest

from omarchy_import import textmerge
from omarchy_import.apply import Applier, load_journal
from omarchy_import.classify import Classifier
from omarchy_import.cli import skel_config_names
from omarchy_import.plan import Context, Planner, scan
from omarchy_import.safefs import Destination, TEMP_PREFIX
from omarchy_import.trial import Trial, marker_time
from tests import fixtures

MIGRATE = Path(__file__).resolve().parents[1]


class ImportCase(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="try-omarchy-import-test-")
        self.addCleanup(self.scratch.cleanup)
        self.base = Path(self.scratch.name)
        self.root, self.trial_home = fixtures.make_trial(self.base)
        self.home, self.skel = fixtures.make_home(self.base)
        self.destination = None

    def plan(self, groups=("settings",), resolution="trial", running=()):
        trial = Trial(self.root)
        classifier = Classifier(skel_config_names(trial.skel, self.skel))
        self.inventory = scan(trial.home, classifier)
        context = Context(
            trial.home, trial.skel, trial.baseline_ns(), self.home, self.skel,
            marker_time(self.home),
            textmerge.Rewriter(trial.account.home, self.home, trial.share_names()),
            trial.share_names(), load_journal(self.home), set(running))
        if self.destination is None:
            self.destination = Destination(self.home)
            self.addCleanup(self.destination.close)
        return Planner(context, self.destination, resolution).plan(self.inventory, list(groups))

    def apply(self, plan):
        return Applier(self.destination).apply(plan)

    def run_import(self, groups=("settings",), resolution="trial"):
        return self.apply(self.plan(groups, resolution))

    @staticmethod
    def actions(plan):
        return {action.relative: action for action in plan.actions}

    def read(self, relative):
        return (self.home / relative).read_bytes()

    def backups(self):
        root = self.home / ".local/share/try-omarchy-import/backups"
        return sorted(root.iterdir()) if root.exists() else []


class PlanTests(ImportCase):
    def test_defaults_and_try_only_files_stay_behind(self):
        plan = self.plan()
        actions = self.actions(plan)
        self.assertEqual(actions[".config/omarchy/branding/about.txt"].action, "default")
        self.assertEqual(actions[".XCompose"].action, "default")
        skipped = dict(plan.skipped)
        for relative in (".local/state/omarchy/toggles/suspend-off", ".config/hypr/monitors.lua",
                         ".local/state/wireplumber", ".cache", "Windows",
                         ".local/share/applications/try-omarchy-windows-"
                         "0123456789abcdef0123456789abcdef.desktop"):
            self.assertIn(relative, skipped)

    def test_groups_and_defaults(self):
        self.plan()
        groups = self.inventory.groups
        self.assertEqual(groups["settings"].kind, "settings")
        self.assertIn("files/Documents", groups)
        self.assertIn("apps/.config/Slack", groups)
        self.assertIn("browser/chromium", groups)
        keys = [entry.relative for entry in groups["keys"].entries]
        self.assertIn(".claude/.credentials.json", keys)
        self.assertIn(".config/gh/hosts.yml", keys)
        self.assertNotIn(".claude/.credentials.json",
                         [entry.relative for entry in groups["apps/.claude"].entries])

    def test_folder_the_user_never_filled_is_not_created(self):
        self.run_import()
        self.assertFalse((self.home / ".config/omarchy/branding").exists())


class ApplyTests(ImportCase):
    def test_changes_are_combined_with_newer_defaults(self):
        self.run_import()
        self.assertEqual(self.read(".config/hypr/bindings.lua"),
                         fixtures.NEW_BINDINGS + b'bind("SUPER", "N", "notes")\n')
        self.assertEqual(self.read(".config/hypr/input.lua"), b'input({ kb_layout = "us,de" })\n')
        self.assertEqual(self.read(".config/hypr/monitors.lua"), fixtures.MONITORS)

    def test_git_identity_replaces_the_setup_default_with_a_backup(self):
        report = self.run_import()
        self.assertIn(b"name = Ada", self.read(".config/git/config"))
        backup = Path(report.backup_directory) / ".config/git/config"
        self.assertIn(b"New Name", backup.read_bytes())
        self.assertEqual(backup.stat().st_mode & 0o777, 0o600)

    def test_paths_and_bookmarks_follow_the_new_home(self):
        fixtures.link(self.trial_home / ".local/bin/notes", "/home/omarchy/Documents/notes.sh")
        self.run_import()
        bookmarks = self.read(".config/gtk-3.0/bookmarks").decode()
        self.assertIn(f"file://{self.home}/Projects/site Site", bookmarks)
        self.assertNotIn("/mnt/host", bookmarks)
        self.assertNotIn("/home/omarchy", bookmarks)
        self.assertEqual(os.readlink(self.home / ".local/bin/notes"),
                         f"{self.home}/Documents/notes.sh")

    def test_setting_changed_here_uses_the_trial_version_with_a_backup(self):
        fixtures.write(self.home / ".bashrc", b"# mine\n", fixtures.HOME_EDIT)
        plan = self.plan()
        self.assertEqual(self.actions(plan)[".bashrc"].action, "conflict")
        report = self.apply(plan)
        self.assertIn(b"alias gs", self.read(".bashrc"))
        self.assertEqual((Path(report.backup_directory) / ".bashrc").read_bytes(), b"# mine\n")

    def test_setting_changed_here_can_be_kept(self):
        fixtures.write(self.home / ".bashrc", b"# mine\n", fixtures.HOME_EDIT)
        self.run_import(resolution="keep")
        self.assertEqual(self.read(".bashrc"), b"# mine\n")
        self.assertIn(b"alias gs", self.read(".bashrc.from-try-omarchy"))
        # A second run recognises its own copy instead of adding another.
        plan = self.plan(resolution="keep")
        self.assertEqual(self.actions(plan)[".bashrc"].action, "imported")
        self.assertFalse((self.home / ".bashrc.from-try-omarchy-2").exists())

    def test_files_are_never_overwritten(self):
        fixtures.write(self.home / "Documents/resume.md", b"# Newer\n", fixtures.HOME_EDIT)
        self.run_import(("files/Documents",))
        self.assertEqual(self.read("Documents/resume.md"), b"# Newer\n")
        self.assertEqual(self.read("Documents/resume (from Try Omarchy).md"), b"# Ada\n")

    def test_modes_and_times_are_kept(self):
        self.run_import(("keys",))
        self.assertEqual((self.home / ".ssh").stat().st_mode & 0o777, 0o700)
        key = self.home / ".ssh/id_ed25519"
        self.assertEqual(key.stat().st_mode & 0o777, 0o600)
        self.assertEqual(key.stat().st_mtime, fixtures.TRIAL_EDIT)
        self.assertFalse((self.home / ".ssh/authorized_keys").exists())
        self.assertIn(b"trial-secret",
                      self.read(".local/share/keyrings/Default_keyring.keyring"))

    def test_rerun_changes_nothing(self):
        self.run_import(("settings", "files/Documents", "keys"))
        plan = self.plan(("settings", "files/Documents", "keys"))
        busy = [action for action in plan.actions
                if action.action in ("create", "replace", "merge", "conflict", "mkdir")]
        self.assertEqual(busy, [])

    def test_deleted_after_import_is_not_brought_back(self):
        self.run_import()
        (self.home / ".config/mise/config.toml").unlink()
        plan = self.plan()
        self.assertEqual(self.actions(plan)[".config/mise/config.toml"].action,
                         "removed-after-import")
        self.apply(plan)
        self.assertFalse((self.home / ".config/mise/config.toml").exists())

    def test_edited_after_import_is_kept(self):
        self.run_import()
        (self.home / ".config/mise/config.toml").write_bytes(b"# edited here\n")
        plan = self.plan()
        self.assertEqual(self.actions(plan)[".config/mise/config.toml"].action,
                         "changed-after-import")

    def test_later_trial_changes_are_picked_up(self):
        self.run_import()
        fixtures.write(self.trial_home / ".config/mise/config.toml", b'[tools]\nnode = "24"\n',
                       fixtures.TRIAL_EDIT + 500)
        plan = self.plan()
        self.assertEqual(self.actions(plan)[".config/mise/config.toml"].action, "replace")
        self.apply(plan)
        self.assertEqual(self.read(".config/mise/config.toml"), b'[tools]\nnode = "24"\n')

    def test_linked_folder_on_this_computer_is_left_alone(self):
        dotfiles = self.base / "dotfiles/nvim"
        dotfiles.mkdir(parents=True)
        os.symlink(dotfiles, self.home / ".config/nvim")
        plan = self.plan()
        action = self.actions(plan)[".config/nvim/lua/plugins/mine.lua"]
        self.assertEqual(action.action, "skip")
        self.assertIn("link", action.reason)
        self.apply(plan)
        self.assertEqual(list(dotfiles.iterdir()), [])

    def test_change_between_planning_and_applying_wins(self):
        plan = self.plan()
        fixtures.write(self.home / ".bashrc", b"# typed while importing\n", fixtures.HOME_EDIT)
        report = self.apply(plan)
        self.assertEqual(self.read(".bashrc"), b"# typed while importing\n")
        skipped = [result for result in report.results if result.relative == ".bashrc"]
        self.assertEqual(skipped[0].status, "skipped")

    def test_browser_profile_replaces_the_new_one_and_brings_the_keyring(self):
        plan = self.plan(("browser/chromium",))
        kinds = [action.action for action in plan.actions]
        self.assertEqual(kinds[0], "replace-profile")
        self.assertIn(".local/share/keyrings/Default_keyring.keyring", self.actions(plan))
        report = self.apply(plan)
        self.assertTrue((self.home / ".config/chromium/Default/Bookmarks").exists())
        self.assertFalse((self.home / ".config/chromium/Default/Preferences").exists())
        self.assertFalse((self.home / ".config/chromium/Default/Cache").exists())
        self.assertFalse(os.path.lexists(self.home / ".config/chromium/SingletonLock"))
        self.assertTrue((Path(report.backup_directory) / ".config/chromium/Default/Preferences")
                        .exists())
        self.assertIn(b"trial-secret",
                      self.read(".local/share/keyrings/Default_keyring.keyring"))
        # Running the import again leaves the imported profile alone.
        again = self.plan(("browser/chromium",))
        self.assertEqual(again.actions[0].action, "imported")

    def test_profile_that_cannot_be_moved_is_not_mixed(self):
        plan = self.plan(("browser/chromium",))
        # The profile changes after planning, so moving it aside is refused.
        fixtures.write(self.home / ".config/chromium/Default/Preferences", b'{"changed": 1}\n',
                       fixtures.HOME_EDIT)
        os.utime(self.home / ".config/chromium", None)
        (self.home / ".config/chromium/new-file").write_bytes(b"x")
        report = self.apply(plan)
        self.assertFalse((self.home / ".config/chromium/Default/Bookmarks").exists())
        statuses = {result.relative: result.status for result in report.results}
        self.assertEqual(statuses[".config/chromium/Default/Bookmarks"], "skipped")

    def test_running_browser_is_not_touched(self):
        plan = self.plan(("browser/chromium",), running=("chromium",))
        self.assertEqual(plan.actions[0].action, "skip")
        self.assertIn("close Chromium", plan.actions[0].reason)

    def test_no_temporary_files_are_left(self):
        self.run_import(("settings", "files/Documents", "files/Pictures", "keys"))
        leftovers = [path for path in self.home.rglob(f"{TEMP_PREFIX}*")]
        self.assertEqual(leftovers, [])


class ReviewRegressionTests(ImportCase):
    def test_interrupted_browser_import_finishes_on_the_next_run(self):
        plan = self.plan(("browser/chromium",))
        partial = [action for action in plan.actions][:2]  # moved aside, root folder made
        self.assertEqual(partial[0].action, "replace-profile")
        plan.actions = partial
        self.apply(plan)
        self.assertFalse((self.home / ".config/chromium/Default/Bookmarks").exists())
        again = self.plan(("browser/chromium",))
        self.assertNotIn("replace-profile", [action.action for action in again.actions])
        self.apply(again)
        self.assertTrue((self.home / ".config/chromium/Default/Bookmarks").exists())
        done = self.plan(("browser/chromium",))
        self.assertEqual(done.actions[0].action, "imported")

    def test_names_that_are_not_utf8_come_over(self):
        name = b"caf\xe9.txt".decode("utf-8", "surrogateescape")
        fixtures.write(self.trial_home / "Documents" / name, b"menu\n", fixtures.TRIAL_EDIT)
        report = self.run_import(("files/Documents",))
        self.assertEqual(report.failures(), [])
        self.assertEqual((self.home / "Documents" / name).read_bytes(), b"menu\n")

    def test_later_trial_changes_keep_what_was_merged_in(self):
        self.run_import()
        self.assertIn(b"new release", self.read(".config/hypr/bindings.lua"))
        fixtures.write(self.trial_home / ".config/hypr/bindings.lua",
                       fixtures.TRIAL_BINDINGS + b'bind("SUPER", "N", "notes")\n'
                       b'bind("SUPER", "M", "music")\n', fixtures.TRIAL_EDIT + 900)
        plan = self.plan()
        action = self.actions(plan)[".config/hypr/bindings.lua"]
        self.assertIn(action.action, ("merge", "replace"))
        self.assertTrue(action.backup)
        self.apply(plan)
        merged = self.read(".config/hypr/bindings.lua")
        self.assertIn(b"new release", merged)
        self.assertIn(b'"music"', merged)

    def test_keys_alone_keep_this_computers_browser_key(self):
        native = fixtures.KEYRING_EMPTY + b"""
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
"""
        fixtures.write(self.home / ".local/share/keyrings/Default_keyring.keyring", native,
                       fixtures.HOME_EDIT, 0o600)
        self.run_import(("keys",))
        keyring = self.read(".local/share/keyrings/Default_keyring.keyring")
        self.assertIn(b"native-secret", keyring)
        self.assertNotIn(b"trial-secret", keyring)

    def test_an_unreadable_file_is_skipped_not_fatal(self):
        locked = self.trial_home / ".config/hypr/private.lua"
        fixtures.write(locked, b"secret\n", fixtures.TRIAL_EDIT)
        os.chmod(locked, 0)
        self.addCleanup(os.chmod, locked, 0o600)
        if os.access(locked, os.R_OK):
            self.skipTest("running as root")
        plan = self.plan()
        action = self.actions(plan)[".config/hypr/private.lua"]
        self.assertEqual(action.action, "skip")
        self.assertIn("could not be read", action.reason)
        self.apply(plan)
        self.assertIn(b'"notes"', self.read(".config/hypr/bindings.lua"))

    def test_a_large_history_is_still_merged(self):
        history = b"".join(b"echo %d\n" % number for number in range(200_000))
        self.assertGreater(len(history), textmerge.TEXT_LIMIT)
        fixtures.write(self.trial_home / ".bash_history", history, fixtures.TRIAL_EDIT)
        fixtures.write(self.home / ".bash_history", b"typed here\n", fixtures.HOME_EDIT)
        self.run_import()
        merged = self.read(".bash_history")
        self.assertTrue(merged.startswith(b"echo 0\n"))
        self.assertTrue(merged.endswith(b"typed here\n"))

    def test_a_copy_beside_is_refreshed_after_trial_changes(self):
        fixtures.write(self.home / ".bashrc", b"# mine\n", fixtures.HOME_EDIT)
        self.run_import(resolution="keep")
        fixtures.write(self.trial_home / ".bashrc", b"alias gs='git status -s'\n",
                       fixtures.TRIAL_EDIT + 900)
        plan = self.plan(resolution="keep")
        action = self.actions(plan)[".bashrc"]
        self.assertEqual(action.action, "conflict")
        self.apply(plan)
        self.assertEqual(self.read(".bashrc"), b"# mine\n")
        self.assertIn(b"git status -s", self.read(".bashrc.from-try-omarchy-2"))


class InterruptedImportTests(ImportCase):
    def test_import_killed_midway_finishes_on_the_next_run(self):
        # Add enough files that the kill lands in the middle.
        for number in range(60):
            fixtures.write(self.trial_home / f"Documents/note-{number:02}.md",
                           f"note {number}\n".encode(), fixtures.TRIAL_EDIT)
        script = textwrap.dedent(f"""
            import os, signal, sys
            sys.path.insert(0, {str(MIGRATE)!r})
            from tests.test_import import ImportCase
            from omarchy_import.apply import Applier
            case = ImportCase()
            case.base = None
            from pathlib import Path
            case.root = Path({str(self.root)!r})
            case.trial_home = Path({str(self.trial_home)!r})
            case.home = Path({str(self.home)!r})
            case.skel = Path({str(self.skel)!r})
            case.destination = None
            case.addCleanup = lambda *a: None
            plan = case.plan(("settings", "files/Documents"))
            def progress(done, total, action):
                if done == 40:
                    os.kill(os.getpid(), signal.SIGKILL)
            Applier(case.destination, progress=progress).apply(plan)
        """)
        result = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True)
        self.assertEqual(result.returncode, -signal.SIGKILL, result.stderr)
        partial = len(list((self.home / "Documents").glob("note-*.md")))
        self.assertLess(partial, 60)
        plan = self.plan(("settings", "files/Documents"))
        self.apply(plan)
        self.assertEqual(len(list((self.home / "Documents").glob("note-*.md"))), 60)
        self.assertEqual(self.read(".config/hypr/bindings.lua"),
                         fixtures.NEW_BINDINGS + b'bind("SUPER", "N", "notes")\n')
        self.assertEqual(list(self.home.rglob(f"{TEMP_PREFIX}*")), [])
        final = self.plan(("settings", "files/Documents"))
        self.assertEqual([action for action in final.actions
                          if action.action in ("create", "replace", "merge", "conflict")], [])


@unittest.skipUnless(shutil.which("git"), "git is needed for the workspace round trip")
class GitWorkspaceTests(ImportCase):
    def git(self, *arguments, cwd):
        environment = {"PATH": os.environ["PATH"], "HOME": str(self.base), "LC_ALL": "C",
                       "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "Ada",
                       "GIT_AUTHOR_EMAIL": "ada@example.com", "GIT_COMMITTER_NAME": "Ada",
                       "GIT_COMMITTER_EMAIL": "ada@example.com"}
        return subprocess.run(["git", "-c", "core.hooksPath=/dev/null", "-c",
                               "commit.gpgsign=false", *arguments], cwd=cwd, env=environment,
                              capture_output=True, text=True, check=True).stdout

    def test_uncommitted_and_untracked_work_survives(self):
        project = self.trial_home / "Projects/demo"
        project.mkdir(parents=True)
        self.git("init", "--initial-branch=main", cwd=project)
        (project / "tracked.txt").write_text("one\n")
        (project / "staged.txt").write_text("staged\n")
        self.git("add", "tracked.txt", cwd=project)
        self.git("commit", "-m", "Base", cwd=project)
        (project / "tracked.txt").write_text("one\ntwo\n")
        self.git("add", "staged.txt", cwd=project)
        (project / "untracked.txt").write_text("new\n")
        head = self.git("rev-parse", "HEAD", cwd=project)
        status = self.git("status", "--porcelain", cwd=project)
        diff = self.git("diff", cwd=project)
        self.run_import(("files/Projects",))
        restored = self.home / "Projects/demo"
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=restored), head)
        self.assertEqual(self.git("status", "--porcelain", cwd=restored), status)
        self.assertEqual(self.git("diff", cwd=restored), diff)
        self.assertEqual(self.git("fsck", "--no-progress", cwd=restored), "")


if __name__ == "__main__":
    unittest.main()
