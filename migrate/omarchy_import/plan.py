"""Inventory the trial's home directory and plan the import.

scan() walks the trial home once and sorts every path into groups (see
classify). make_plan() then compares the selected groups with the home
directory on this computer and decides one action per path:

create    the path is missing here, so it is copied
same      this computer already has identical content
replace   this computer still has the default, so the trial's version
          replaces it (the default is backed up first)
merge     the trial's changes are combined with this computer's newer
          default, or a list file is merged line by line
conflict  this computer's copy was changed after setup; depending on the
          chosen resolution the trial's copy replaces it (with a backup) or
          is written next to it
default   the trial never changed it from Omarchy's default, so there is
          nothing to bring over
skip      Try-only, a cache, unsafe or otherwise left behind, with a reason
imported  an earlier run already imported it and it has not changed since

Nothing on either side is modified while planning.
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
import stat

from . import classify, safefs, textmerge
from .classify import APPS, BROWSER, FILES, KEYS, SETTINGS, SKIP

# Kinds of groups where only what the user changed is brought over. Browser
# profiles are copied whole.
CHANGED_ONLY_KINDS = (SETTINGS, APPS, KEYS, FILES)
TRANSFORM_KINDS = (SETTINGS, APPS, KEYS)
HYPR_BLOCK_FILES = {".config/hypr/input.lua", ".config/hypr/monitors.lua"}
BOOKMARKS = ".config/gtk-3.0/bookmarks"

APPLYING = ("create", "replace", "merge", "conflict", "mkdir", "replace-profile")


@dataclass
class Entry:
    relative: str
    kind: str
    mode: int
    size: int
    mtime_ns: int
    target: str = None
    # The later of ctime and mtime. Tools can set an old mtime (tar, cp -p),
    # but not ctime, so this says when the file really last changed.
    changed_ns: int = 0


def changed_ns(metadata):
    return max(metadata.st_ctime_ns, metadata.st_mtime_ns)


@dataclass
class Group:
    id: str
    kind: str
    label: str
    entries: list = field(default_factory=list)
    bytes: int = 0
    files: int = 0
    selected: bool = False
    source_base: Path = None
    changed: int = 0

    def add(self, entry, baseline_ns=None):
        self.entries.append(entry)
        if entry.kind == "directory":
            return
        if baseline_ns is None or entry.changed_ns > baseline_ns:
            self.changed += 1
            self.bytes += entry.size
        self.files += 1


@dataclass
class Inventory:
    groups: dict
    skipped: list  # (relative, reason)

    def ordered(self):
        order = {SETTINGS: 0, FILES: 1, APPS: 2, BROWSER: 3, KEYS: 4}
        return sorted(self.groups.values(), key=lambda group: (order[group.kind], group.id))


@dataclass
class Action:
    relative: str
    kind: str
    group: str
    action: str
    reason: str = ""
    entry: Entry = None
    expected: safefs.Fingerprint = None
    content: bytes = None
    sibling: str = None
    backup: bool = False
    size: int = 0
    source: Path = None


@dataclass
class Plan:
    actions: list
    skipped: list
    groups: list
    resolution: str

    def counts(self):
        result = {}
        for action in self.actions:
            result[action.action] = result.get(action.action, 0) + 1
        return result

    def conflicts(self):
        return [action for action in self.actions if action.action == "conflict"]

    def bytes_needed(self):
        total = 0
        for action in self.actions:
            if action.action in ("create", "replace", "merge", "conflict"):
                total += action.size
            if action.backup and action.expected is not None:
                total += action.expected.size
        return total


def _entry_kind(metadata):
    return safefs.kind_of(metadata)


def scan(home, classifier, baseline_ns=None, progress=None):
    """Sort every path in the trial home into groups. Never follows symlinks.

    Groups with nothing changed since Omarchy set the account up (baseline_ns)
    are not offered; sizes count only what changed.
    """
    home = Path(home)
    groups = {}
    skipped = []
    count = 0

    def group_for(place):
        group = groups.get(place.group)
        if group is None:
            label = place.label
            if place.group == classify.SETTINGS_GROUP:
                label = "Settings and customizations"
            elif place.group == classify.KEYS_GROUP:
                label = "Keys and sign-ins"
            group = groups[place.group] = Group(place.group, place.kind, label, source_base=home)
        return group

    def describe(path, relative):
        metadata = os.lstat(path)
        kind = _entry_kind(metadata)
        target = os.readlink(path) if kind == "symlink" else None
        return Entry(relative, kind, stat.S_IMODE(metadata.st_mode),
                     metadata.st_size if kind == "file" else 0, metadata.st_mtime_ns, target,
                     changed_ns(metadata))

    def walk_group(path, relative, place):
        nonlocal count
        entry = describe(path, relative)
        adjusted = classifier.override(relative, place, entry.kind, entry.target)
        if adjusted.kind == SKIP:
            skipped.append((relative, adjusted.reason))
            return
        group_for(adjusted).add(entry, baseline_ns)
        count += 1
        if progress and count % 5000 == 0:
            progress(count)
        if entry.kind == "directory":
            try:
                children = sorted(os.scandir(path), key=lambda item: item.name)
            except PermissionError:
                skipped.append((relative, "could not be read"))
                return
            for child in children:
                walk_group(child.path, f"{relative}/{child.name}", place)

    def walk_container(path, relative):
        try:
            children = sorted(os.scandir(path), key=lambda item: item.name)
        except FileNotFoundError:
            return
        for child in children:
            child_relative = f"{relative}/{child.name}" if relative else child.name
            entry = describe(child.path, child_relative)
            if child_relative in classify.CONTAINERS and entry.kind == "directory":
                walk_container(child.path, child_relative)
                continue
            place = classifier.place(child_relative, entry.kind, entry.target)
            if place.kind == classify.CONTAINER:
                if entry.kind == "directory":
                    walk_container(child.path, child_relative)
                continue
            if place.kind == SKIP:
                skipped.append((child_relative, place.reason))
                continue
            walk_group(child.path, child_relative, place)

    walk_container(home, "")
    # Folders alone, or files Omarchy made while setting up, are not worth
    # offering. Nor is app data where only links changed (Omarchy links its
    # agent skills into several app folders).
    groups = {key: group for key, group in groups.items()
              if group.changed and (group.kind != APPS or group.bytes)}
    return Inventory(groups, skipped)


@dataclass
class Context:
    """Everything make_plan compares against."""
    trial_home: Path
    trial_skel: Path
    trial_baseline_ns: int
    home: Path
    home_skel: Path
    home_baseline_ns: int
    rewriter: textmerge.Rewriter
    share_names: set = field(default_factory=set)
    journal: dict = field(default_factory=dict)
    running_browsers: set = field(default_factory=set)


def _lstat(path):
    try:
        return os.lstat(path)
    except (FileNotFoundError, NotADirectoryError):
        return None


class TrialDefaults:
    """Decides whether a trial path is still Omarchy's default.

    A path counts as untouched when it equals the trial's /etc/skel copy
    (ignoring Try Omarchy's appended blocks), or when it last changed before
    Omarchy finished setting the account up.
    """

    def __init__(self, skel, baseline_ns):
        self.skel = Path(skel)
        self.baseline_ns = baseline_ns

    @staticmethod
    def normalize(relative, data):
        if data is None:
            return None
        if relative in HYPR_BLOCK_FILES:
            data = textmerge.strip_try_blocks(data)
        return data

    def is_default(self, relative, entry, data=None, read=None):
        """data is the trial file's content if already read; read() reads it on demand."""
        metadata = _lstat(self.skel / relative)
        if metadata is not None:
            skel_kind = _entry_kind(metadata)
            if entry.kind == "symlink" and skel_kind == "symlink":
                if os.readlink(self.skel / relative) == entry.target:
                    return True
            elif entry.kind == "file" and skel_kind == "file":
                if metadata.st_size == entry.size or relative in HYPR_BLOCK_FILES:
                    if data is None and read is not None and entry.size <= textmerge.TEXT_LIMIT * 64:
                        data = read()
                    skel_data = safefs.source_read(self.skel / relative, textmerge.TEXT_LIMIT * 64)
                    if skel_data is not None and data is not None and \
                            self.normalize(relative, skel_data) == self.normalize(relative, data):
                        return True
        return self.baseline_ns is not None and entry.changed_ns <= self.baseline_ns


class Planner:
    def __init__(self, context, destination, resolution="trial"):
        if resolution not in ("trial", "keep"):
            raise ValueError("resolution must be trial or keep")
        self.context = context
        self.destination = destination
        self.resolution = resolution
        self.can_merge = textmerge.git_available()
        self.defaults = TrialDefaults(context.trial_skel, context.trial_baseline_ns)

    # Content helpers ------------------------------------------------------

    def _normalize(self, relative, data):
        return TrialDefaults.normalize(relative, data)

    def _prepare(self, group_kind, relative, data):
        """The trial content as it should land on this computer."""
        data = self._normalize(relative, data)
        if group_kind in TRANSFORM_KINDS and textmerge.is_text(data):
            if relative == BOOKMARKS:
                data = textmerge.clean_bookmarks(data, self.context.rewriter.trial_home,
                                                 self.context.share_names)
            data = self.context.rewriter.text(data)
        return data

    def _trial_default(self, relative, entry, data):
        return self.defaults.is_default(relative, entry, data)

    def _home_default(self, relative, metadata):
        """True if this computer's file is still what Omarchy set up."""
        baseline = self.context.home_baseline_ns
        if baseline is not None and changed_ns(metadata) <= baseline:
            return True
        skel = self.context.home_skel / relative
        skel_metadata = _lstat(skel)
        if skel_metadata is None:
            return False
        kind = _entry_kind(metadata)
        if kind != _entry_kind(skel_metadata):
            return False
        if kind == "symlink":
            return os.readlink(skel) == self.destination.readlink(relative)
        if kind == "file" and skel_metadata.st_size == metadata.st_size:
            return safefs.source_hash(skel)[0] == self.destination.hash(relative)[0]
        return False

    def _read_trial(self, group, entry):
        path = group.source_base / entry.relative
        limit = textmerge.TEXT_LIMIT if group.kind in TRANSFORM_KINDS else 0
        if limit and entry.size <= limit:
            return safefs.source_read(path, limit)
        return None

    # Planning -------------------------------------------------------------

    def plan(self, inventory, selected):
        actions = []
        wanted = [inventory.groups[group_id] for group_id in selected if group_id in inventory.groups]
        wanted_ids = {group.id for group in wanted}
        # A browser profile's saved logins are encrypted with a key stored in
        # the login keyring, so a selected browser brings the keyring along.
        if any(group.kind == BROWSER for group in wanted) and classify.KEYS_GROUP not in wanted_ids:
            keys = inventory.groups.get(classify.KEYS_GROUP)
            if keys is not None:
                keyring = Group(classify.KEYS_GROUP, KEYS, keys.label, source_base=keys.source_base)
                for entry in keys.entries:
                    if entry.relative == textmerge.KEYRING_DIRECTORY or \
                            entry.relative.startswith(textmerge.KEYRING_DIRECTORY + "/"):
                        keyring.add(entry)
                wanted.append(keyring)
        for group in wanted:
            if group.kind == BROWSER:
                actions.extend(self._plan_browser(group))
            else:
                for entry in group.entries:
                    actions.append(self._plan_entry(group, entry))
        actions = self._prune_directories(actions)
        return Plan(actions, list(inventory.skipped), [group.id for group in wanted], self.resolution)

    def _journal_state(self, relative, metadata):
        record = self.context.journal.get(relative)
        if record is None:
            return None
        if record.get("state") == "pending":
            if metadata is not None and safefs.kind_of(metadata) == "file" and \
                    metadata.st_size == record.get("size") and \
                    self.destination.hash(relative)[0] == record.get("sha256"):
                return "imported"
            return None
        if metadata is None:
            return "removed-after-import"
        if record.get("kind") == "symlink":
            if safefs.kind_of(metadata) == "symlink" and \
                    self.destination.readlink(relative) == record.get("target"):
                return "imported"
            return "changed-after-import"
        if (metadata.st_size, metadata.st_mtime_ns) == (record.get("size"), record.get("mtime_ns")):
            return "imported"
        if safefs.kind_of(metadata) == "file" and metadata.st_size == record.get("size") and \
                self.destination.hash(relative)[0] == record.get("sha256"):
            return "imported"
        return "changed-after-import"

    def _source_changed(self, relative, entry):
        record = self.context.journal.get(relative) or {}
        if "source_mtime_ns" not in record:
            return False
        return (record.get("source_size"), record.get("source_mtime_ns")) != (entry.size,
                                                                           entry.mtime_ns)

    def _plan_entry(self, group, entry):
        relative = entry.relative
        base = Action(relative, entry.kind, group.id, "skip", entry=entry, size=entry.size,
                      source=group.source_base / relative)
        try:
            metadata = self.destination.lstat(relative)
        except safefs.UnsafePath as error:
            base.reason = f"{error.path} {error.reason}"
            return base
        expected = None if metadata is None else safefs.Fingerprint.of(metadata)
        base.expected = expected

        if entry.kind == "directory":
            return self._plan_directory(group, entry, base, metadata)

        journal = self._journal_state(relative, metadata)
        if journal == "imported" and self._source_changed(relative, entry):
            # The trial was used again after the last import and this
            # computer's copy is still exactly what that import wrote.
            if relative in textmerge.LIST_FILES or relative.endswith(".keyring"):
                # Merged files keep this computer's entries: merge again.
                return self._plan_file(group, entry, base, metadata)
            planned = (self._plan_symlink if entry.kind == "symlink" else self._plan_file)(
                group, entry, base, None)
            if planned.action == "create":
                planned.action = "replace"
                planned.reason = "updated from the trial"
            return planned
        if journal is not None:
            base.action = journal
            base.reason = {"imported": "imported by an earlier run",
                           "removed-after-import": "you removed it after an earlier import",
                           "changed-after-import": "you changed it after an earlier import"}[journal]
            return base

        if entry.kind == "symlink":
            return self._plan_symlink(group, entry, base, metadata)
        return self._plan_file(group, entry, base, metadata)

    def _plan_directory(self, group, entry, action, metadata):
        if metadata is not None:
            if safefs.kind_of(metadata) != "directory":
                action.action = "skip"
                action.reason = "a file with this name already exists on this computer"
                return action
            action.action = "same"
            return action
        action.action = "mkdir"
        return action

    def _plan_symlink(self, group, entry, action, metadata):
        target = self.context.rewriter.path(entry.target)
        action.content = target.encode()
        if group.kind in CHANGED_ONLY_KINDS and self._trial_default(entry.relative, entry, None):
            action.action = "default"
            return action
        if metadata is None:
            action.action = "create"
            return action
        current_kind = safefs.kind_of(metadata)
        if current_kind == "symlink" and self.destination.readlink(entry.relative) == target:
            action.action = "same"
            return action
        if current_kind == "symlink" and self._home_default(entry.relative, metadata):
            action.action = "replace"
            action.reason = "replaces the link Omarchy set up"
            return action
        action.action = "skip"
        action.reason = "something different already exists here on this computer"
        return action

    def _plan_file(self, group, entry, action, metadata):
        relative = entry.relative
        raw = self._read_trial(group, entry)
        if group.kind in CHANGED_ONLY_KINDS and self._trial_default(relative, entry, raw):
            action.action = "default"
            return action
        if relative.startswith(textmerge.KEYRING_DIRECTORY + "/") and relative.endswith(".keyring"):
            return self._plan_keyring(group, entry, action, metadata, raw)
        content = self._prepare(group.kind, relative, raw) if raw is not None else None
        action.content = content if content is not None and content != raw else None
        if content is not None:
            action.size = len(content)

        if metadata is None:
            action.action = "create"
            return action
        if safefs.kind_of(metadata) != "file":
            action.action = "skip"
            action.reason = "a folder or link with this name already exists on this computer"
            return action

        if content is not None:
            current = self.destination.read(relative, textmerge.TEXT_LIMIT)
            if current == content:
                action.action = "same"
                return action
        elif metadata.st_size == entry.size:
            if self.destination.hash(relative)[0] == safefs.source_hash(group.source_base / relative)[0]:
                action.action = "same"
                return action
            current = None
        else:
            current = None

        if group.kind == FILES:
            return self._conflict(action, "a different file with this name is already here",
                                  files=True)

        if relative in textmerge.LIST_FILES and content is not None and current is not None:
            merged = textmerge.merge_lists(relative, content, current)
            if merged == current:
                action.action = "same"
                return action
            action.action = "merge"
            action.reason = "entries from the trial were added"
            action.content = merged
            action.size = len(merged)
            action.backup = True
            return action

        if self._home_default(relative, metadata):
            action.backup = True
            base = self._trial_skel_content(relative)
            if self.can_merge and content is not None and current is not None and base is not None \
                    and base != current:
                merged, clean = textmerge.merge3(base, content, current)
                if merged is not None and merged != content:
                    action.action = "merge"
                    action.reason = ("your changes were combined with this computer's newer default"
                                     if clean else "combined with this computer's newer default; "
                                     "where both changed the same lines, yours were kept")
                    action.content = merged
                    action.size = len(merged)
                    return action
                if merged is None:
                    action.action = "replace"
                    action.reason = ("your version replaces the one Omarchy set up here, which "
                                     "is kept in the backup")
                    return action
            action.action = "replace"
            action.reason = "replaces Omarchy's default"
            return action
        return self._conflict(action, "you already changed this on this computer")

    def _trial_skel_content(self, relative):
        path = self.context.trial_skel / relative
        metadata = _lstat(path)
        if metadata is None or _entry_kind(metadata) != "file" or metadata.st_size > textmerge.TEXT_LIMIT:
            return None
        data = safefs.source_read(path, textmerge.TEXT_LIMIT)
        data = self._normalize(relative, data)
        if data is not None and textmerge.is_text(data):
            data = self.context.rewriter.text(data)
        return data

    def _conflict(self, action, reason, files=False, beside=False):
        """A file changed on both sides. Files in folders, and anything with
        beside=True, always keep this computer's copy in place."""
        action.action = "conflict"
        action.reason = reason
        if files or beside or self.resolution == "keep":
            if self._conflict_copy_imported(action.relative):
                action.action = "imported"
                action.reason = "an earlier run already put the trial's copy next to it"
                return action
            action.sibling = self._sibling_name(action.relative, files)
        else:
            action.backup = True
        return action

    def _conflict_copy_imported(self, relative):
        """True if an earlier run already wrote the trial's copy next to relative."""
        return any(record.get("state") == "done" and record.get("conflict_of") == relative
                   for record in self.context.journal.values())

    def _sibling_name(self, relative, files):
        parent, _, name = relative.rpartition("/")
        prefix = f"{parent}/" if parent else ""
        for number in range(1, 1000):
            suffix = "" if number == 1 else f" {number}"
            if files:
                stem, dot, extension = name.rpartition(".")
                if not stem:
                    stem, dot, extension = name, "", ""
                candidate = f"{stem} (from Try Omarchy{suffix}){dot}{extension}"
            else:
                candidate = f"{name}.from-try-omarchy{suffix.replace(' ', '-')}"
            try:
                if self.destination.lstat(prefix + candidate) is None:
                    return prefix + candidate
            except safefs.UnsafePath:
                return prefix + candidate
        raise RuntimeError(f"too many earlier copies of {relative}")

    def _plan_keyring(self, group, entry, action, metadata, raw):
        if raw is None or textmerge.keyring_is_encrypted(raw):
            action.action = "skip"
            action.reason = "this keyring is protected by the trial's password"
            return action
        if metadata is None:
            action.action = "create"
            return action
        current = self.destination.read(entry.relative, textmerge.TEXT_LIMIT)
        if current is None or textmerge.keyring_is_encrypted(current):
            return self._conflict(action, "this computer's keyring is protected by a password",
                                  beside=True)
        try:
            merged = textmerge.merge_keyrings(raw, current)
        except (ValueError, UnicodeDecodeError):
            return self._conflict(action, "the keyring could not be read", beside=True)
        if merged == current:
            action.action = "same"
            return action
        action.action = "merge"
        action.reason = "the trial's saved passwords were added to this computer's keyring"
        action.content = merged
        action.size = len(merged)
        action.backup = True
        return action

    def _plan_browser(self, group):
        root = group.entries[0].relative if group.entries else None
        identity = group.id.split("/", 1)[1]
        actions = []
        running = self.context.running_browsers & set(classify.BROWSER_PROCESSES.get(identity, ()))
        try:
            metadata = self.destination.lstat(root) if root else None
        except safefs.UnsafePath as error:
            return [Action(root, "directory", group.id, "skip", f"{error.path} {error.reason}")]
        if running:
            return [Action(root, "directory", group.id, "skip",
                           f"close {group.label} before importing its profile")]
        if any(path == root or path.startswith(root + "/") for path in self.context.journal):
            return [Action(root, "directory", group.id, "imported", "imported by an earlier run")]
        if metadata is not None:
            actions.append(Action(root, "directory", group.id, "replace-profile",
                                  f"this computer's {group.label} profile is moved to the backup",
                                  expected=safefs.Fingerprint.of(metadata), backup=True))
        for entry in group.entries:
            action = Action(entry.relative, entry.kind, group.id,
                            "mkdir" if entry.kind == "directory" else "create",
                            entry=entry, size=entry.size, source=group.source_base / entry.relative)
            if entry.kind == "symlink":
                action.content = self.context.rewriter.path(entry.target).encode()
            actions.append(action)
        return actions

    def _prune_directories(self, actions):
        """Only create folders that will hold something, or that the user made."""
        needed = set()
        for action in actions:
            if action.action in ("create", "replace", "merge", "conflict") and action.kind != "directory":
                parent = action.relative.rpartition("/")[0]
                while parent:
                    needed.add(parent)
                    parent = parent.rpartition("/")[0]
        result = []
        for action in actions:
            if action.action == "mkdir" and action.relative not in needed:
                group_kind = action.group.split("/", 1)[0]
                user_made = action.entry is not None and (
                    group_kind in (FILES, BROWSER)
                    or (self.context.trial_baseline_ns is not None
                        and action.entry.changed_ns > self.context.trial_baseline_ns
                        and _lstat(self.context.trial_skel / action.relative) is None))
                if not user_made:
                    action.action = "default"
                    action.reason = ""
            elif action.action == "same" and action.kind == "directory":
                continue
            result.append(action)
        return result
