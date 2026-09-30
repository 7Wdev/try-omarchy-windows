"""Carry out a plan, keeping a journal so an interrupted import can resume.

Before anything that replaces a file on this computer, the original is copied
to a private backup folder. Every file the importer writes is recorded in an
append-only journal (size, mtime and hash of what was written), and a
replacement is recorded as pending before it happens. A later run uses the
journal to recognise its own earlier work, so it never recreates something
the user deleted after importing, and never overwrites a later edit.
"""

import contextlib
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import time

from . import safefs

JOURNAL_NAME = "journal.jsonl"
STATE_DIRECTORY = ".local/state/try-omarchy-import"
BACKUP_DIRECTORY = ".local/share/try-omarchy-import/backups"


def load_journal(home):
    """Return {relative: last completed record} from earlier runs."""
    path = Path(home) / STATE_DIRECTORY / JOURNAL_NAME
    records = {}
    try:
        with open(path, "rb") as source:
            for line in source:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue  # a torn last line from an interrupted run
                if not isinstance(record, dict) or not isinstance(record.get("path"), str):
                    continue
                if record.get("state") in ("pending", "done"):
                    # The last record wins. A pending one means the run
                    # stopped around a replacement; the planner checks the
                    # recorded hash to see whether it happened.
                    records[record["path"]] = record
    except FileNotFoundError:
        return {}
    return records


class Journal:
    def __init__(self, home, run_id):
        self.directory = Path(home) / STATE_DIRECTORY
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.chmod(self.directory, 0o700)
        path = self.directory / JOURNAL_NAME
        fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC,
                     0o600)
        self.stream = os.fdopen(fd, "a", encoding="utf-8")
        self.run_id = run_id
        self.unsynced = 0

    def write(self, record, durable=False):
        record = {**record, "run": self.run_id}
        self.stream.write(json.dumps(record, sort_keys=True) + "\n")
        self.stream.flush()
        self.unsynced += 1
        if durable or self.unsynced >= 200:
            os.fsync(self.stream.fileno())
            self.unsynced = 0

    def close(self):
        self.stream.flush()
        os.fsync(self.stream.fileno())
        self.stream.close()


@dataclass
class Result:
    relative: str
    action: str
    status: str
    detail: str = ""


@dataclass
class Report:
    run_id: str
    backup_directory: str
    results: list = field(default_factory=list)
    written_bytes: int = 0

    def failures(self):
        return [result for result in self.results if result.status == "failed"]

    def add(self, relative, action, status, detail=""):
        self.results.append(Result(relative, action, status, detail))


def new_run_id():
    return time.strftime("%Y%m%d-%H%M%S")


class Applier:
    def __init__(self, destination, run_id=None, progress=None):
        self.destination = destination
        self.run_id = run_id or new_run_id()
        self.backup_root = f"{BACKUP_DIRECTORY}/{self.run_id}"
        self.progress = progress
        self.report = Report(self.run_id, str(Path(destination.home) / self.backup_root))

    def apply(self, plan):
        journal = Journal(self.destination.home, self.run_id)
        try:
            self._cleanup(plan)
            total = sum(1 for action in plan.actions if action.action in
                        ("create", "replace", "merge", "conflict", "mkdir", "replace-profile"))
            done = 0
            # A browser profile that could not be moved aside must not be
            # mixed with the trial's: everything under it is skipped.
            blocked = []
            for action in plan.actions:
                if action.action not in ("create", "replace", "merge", "conflict", "mkdir",
                                         "replace-profile"):
                    continue
                done += 1
                if self.progress:
                    self.progress(done, total, action)
                if any(action.relative == root or action.relative.startswith(root + "/")
                       for root in blocked):
                    self.report.add(action.relative, action.action, "skipped",
                                    "this computer's profile could not be moved aside")
                    continue
                try:
                    self._one(action, journal)
                except safefs.Changed:
                    if action.action == "replace-profile":
                        blocked.append(action.relative)
                    self.report.add(action.relative, action.action, "skipped",
                                    "changed on this computer while importing")
                except FileExistsError:
                    self.report.add(action.relative, action.action, "skipped",
                                    "appeared on this computer while importing")
                except safefs.UnsafePath as error:
                    if action.action == "replace-profile":
                        blocked.append(action.relative)
                    self.report.add(action.relative, action.action, "skipped",
                                    f"{error.path} {error.reason}")
                except OSError as error:
                    if action.action == "replace-profile":
                        blocked.append(action.relative)
                    self.report.add(action.relative, action.action, "failed",
                                    error.strerror or str(error))
        finally:
            journal.close()
            os.sync()
        return self.report

    def _cleanup(self, plan):
        """Remove temporary files an interrupted run left in folders we touch."""
        folders = {action.relative.rpartition("/")[0] for action in plan.actions
                   if action.action in ("create", "replace", "merge", "conflict")}
        for folder in sorted(folders):
            with contextlib.suppress(OSError, safefs.UnsafePath):
                self.destination.remove_temporaries(folder)

    def _backup(self, action):
        """Copy this computer's file into the run's backup folder."""
        target = f"{self.backup_root}/{action.relative}"
        with self.destination.open_file(action.relative) as fd:
            metadata = os.fstat(fd)
            if safefs.Fingerprint.of(metadata) != action.expected:
                raise safefs.Changed(action.relative)
            self.destination.create_file(target, (fd, None), mode=0o600,
                                         mtime_ns=metadata.st_mtime_ns, durable=True,
                                         directory_mode=0o700)
        return target

    def _one(self, action, journal):
        entry = action.entry
        if action.action == "replace-profile":
            target = f"{self.backup_root}/{action.relative}"
            self.destination.rename_away(action.relative, target, action.expected)
            journal.write({"path": action.relative, "state": "done", "kind": "profile",
                           "backup": target}, durable=True)
            self.report.add(action.relative, action.action, "done", f"moved to {target}")
            return
        if action.action == "mkdir":
            created = self.destination.make_directory(action.relative, entry.mode)
            if created:
                self._set_directory_mode(action.relative, entry.mode)
            self.report.add(action.relative, action.action, "done")
            return

        if action.action == "conflict" and action.sibling:
            relative = action.sibling
            mode = "create"
        elif action.action == "create":
            relative = action.relative
            mode = "create"
        else:
            relative = action.relative
            mode = "replace"

        if action.kind == "symlink":
            target = action.content.decode()
            if mode == "create":
                self.destination.create_symlink(relative, target)
            else:
                self.destination.replace_symlink(relative, target, action.expected)
            journal.write({"path": relative, "state": "done", "kind": "symlink", "target": target,
                           "source_size": entry.size, "source_mtime_ns": entry.mtime_ns})
            self.report.add(relative, action.action, "done")
            return

        file_mode = entry.mode if entry is not None else 0o644
        mtime_ns = entry.mtime_ns if entry is not None and action.action != "merge" else None
        backup = None
        if mode == "replace" and action.backup:
            backup = self._backup(action)
        if action.content is not None:
            content = action.content
            expected_sha = safefs.hash_bytes(content)
            size = len(content)
        else:
            content = None
            expected_sha, size = (safefs.source_hash(action.source) if mode == "replace"
                                  else (None, entry.size))
        if mode == "replace":
            journal.write({"path": relative, "state": "pending", "kind": "file", "size": size,
                           "sha256": expected_sha, "backup": backup}, durable=True)
        source_fd = None
        try:
            if content is None:
                source_fd = safefs.source_open(action.source)
                content = (source_fd, None)
            if mode == "create":
                sha, written = self.destination.create_file(
                    relative, content, mode=file_mode, mtime_ns=mtime_ns,
                    durable=action.group == "settings" or action.group == "keys")
            else:
                sha, written = self.destination.replace_file(
                    relative, content, action.expected, mode=file_mode, mtime_ns=mtime_ns)
        finally:
            if source_fd is not None:
                os.close(source_fd)
        metadata = self.destination.lstat(relative)
        record = {"path": relative, "state": "done", "kind": "file", "size": written,
                  "sha256": sha, "mtime_ns": metadata.st_mtime_ns if metadata else None,
                  "backup": backup}
        if entry is not None:
            record["source_size"] = entry.size
            record["source_mtime_ns"] = entry.mtime_ns
        if relative != action.relative:
            record["conflict_of"] = action.relative
        journal.write(record)
        self.report.written_bytes += written
        detail = f"backup in {backup}" if backup else ""
        if action.action == "conflict" and action.sibling:
            detail = f"kept this computer's version; the trial's is {action.sibling}"
        self.report.add(relative, action.action, "done", detail)

    def _set_directory_mode(self, relative, mode):
        with self.destination.directory(relative) as fd:
            if fd is not None:
                os.fchmod(fd, (mode & 0o777) | 0o700)
