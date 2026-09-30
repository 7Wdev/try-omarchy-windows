"""Content transforms used while importing text files.

- Try Omarchy appends guarded blocks to some Hyprland files. They are inert on
  a real install, but stripping them before comparing lets an otherwise
  untouched file count as unchanged.
- When the trial account and the new account have different names, paths
  that point into the trial's home directory are rewritten.
- A file the user changed in the trial is combined with the newer default on
  this computer with git merge-file, using the trial's original default as
  the common base.
- A few files are plain lists (bookmarks, known hosts, shell history), which
  are merged line by line instead.
"""

import os
import re
import subprocess
import tempfile

TEXT_LIMIT = 1024 * 1024

TRY_BLOCKS = {
    b"-- BEGIN TRY OMARCHY PINCH DEVICE": b"-- END TRY OMARCHY PINCH DEVICE",
    b"-- BEGIN OMARCHY QEMU PROFILE": b"-- END OMARCHY QEMU PROFILE",
}
LEGACY_PINCH_LINES = (
    b"-- Try Omarchy's host pinch device carries gestures only.",
    b'dofile("/usr/share/try-omarchy/pinch-input.lua")',
)

LIST_FILES = {
    ".config/gtk-3.0/bookmarks": "append",
    ".ssh/known_hosts": "append",
    ".bash_history": "history",
    ".zsh_history": "history",
    ".python_history": "history",
    ".node_repl_history": "history",
}

KEYRING_DIRECTORY = ".local/share/keyrings"


def is_text(data):
    if data is None or len(data) > TEXT_LIMIT or b"\0" in data:
        return False
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return True


def strip_try_blocks(data):
    """Remove Try Omarchy's appended blocks, then trailing blank lines.

    Trimming matches Try Omarchy's own catch-up script, so a file compares
    equal to its default once the blocks are gone. A block without its end
    marker is left alone.
    """
    kept = []
    end = None
    changed = False
    for line in data.splitlines(keepends=True):
        bare = line.rstrip(b"\r\n")
        if end is None and bare in TRY_BLOCKS:
            end = TRY_BLOCKS[bare]
            changed = True
            continue
        if end is not None:
            if bare == end:
                end = None
            continue
        if bare in LEGACY_PINCH_LINES:
            changed = True
            continue
        kept.append(line)
    if end is not None or not changed:
        return data
    text = b"".join(kept).rstrip(b"\n")
    return text + b"\n" if text else b""


class Rewriter:
    """Rewrite /home/<trial user> to the new home directory in text."""

    def __init__(self, trial_home, home, share_names=()):
        self.trial_home = trial_home.rstrip("/")
        self.home = os.fspath(home).rstrip("/")
        self.active = self.trial_home != self.home
        self.pattern = re.compile(re.escape(self.trial_home.encode()) + rb"(?![A-Za-z0-9._-])")
        self.share_names = set(share_names)

    def path(self, value):
        """Rewrite a single path string (for symlink targets)."""
        if value == self.trial_home or value.startswith(self.trial_home + "/"):
            return self.home + value[len(self.trial_home):]
        return value

    def text(self, data):
        if not self.active or self.trial_home.encode() not in data:
            return data
        return self.pattern.sub(self.home.encode().replace(b"\\", b"\\\\"), data)


def clean_bookmarks(data, trial_home, share_names):
    """Drop Files sidebar entries for the shared Windows folder."""
    kept = []
    prefixes = [b"file:///mnt/host"] + [f"file://{trial_home}/{name}".encode()
                                         for name in share_names]
    for line in data.splitlines(keepends=True):
        location = line.split(b" ", 1)[0].rstrip(b"\n")
        if any(location == prefix or location.startswith(prefix + b"/") for prefix in prefixes):
            continue
        kept.append(line)
    return b"".join(kept)


def merge_lists(relative, ours, theirs):
    """Combine the trial's list file (ours) with this computer's (theirs)."""
    mode = LIST_FILES[relative]
    ours_lines = _lines(ours)
    theirs_lines = _lines(theirs)
    if mode == "history":
        # Trial history first, then anything typed on this computer.
        combined = ours_lines + theirs_lines
    else:
        seen = set(theirs_lines)
        combined = theirs_lines + [line for line in ours_lines if line not in seen]
    result = []
    seen_lines = set()
    for line in combined:
        if mode != "history" and line in seen_lines:
            continue
        seen_lines.add(line)
        result.append(line)
    return b"".join(line + b"\n" for line in result)


def _lines(data):
    return [line for line in data.split(b"\n") if line] if data else []


def git_available():
    try:
        subprocess.run(["git", "--version"], capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return False
    return True


def merge3(base, ours, theirs):
    """Three-way merge. Returns (merged bytes, clean) or (None, False).

    Where both sides changed the same lines, ours (the trial's version) is
    kept and the result is not clean; every other change from theirs (this
    computer's newer default) still applies.
    """
    if not (is_text(base) and is_text(ours) and is_text(theirs)):
        return None, False
    with tempfile.TemporaryDirectory(prefix="try-omarchy-merge-") as scratch:
        paths = []
        for name, data in (("ours", ours), ("base", base), ("theirs", theirs)):
            path = os.path.join(scratch, name)
            with open(path, "wb") as output:
                output.write(data)
            paths.append(path)
        environment = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C",
                       "HOME": scratch, "GIT_CONFIG_NOSYSTEM": "1"}
        try:
            clean = subprocess.run(["git", "merge-file", "-p", "-q", *paths],
                                   capture_output=True, env=environment, timeout=60)
            if clean.returncode == 0:
                return clean.stdout, True
            if clean.returncode < 0 or clean.returncode > 127:
                return None, False
            favoured = subprocess.run(["git", "merge-file", "-p", "-q", "--ours", *paths],
                                      capture_output=True, env=environment, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            return None, False
    if favoured.returncode != 0:
        return None, False
    return favoured.stdout, False


# GNOME Keyring's unencrypted keyring format, which Omarchy uses for its
# default keyring (install/user/default-keyring.sh). The file is an ini-like
# list of sections: [keyring], then [N] per item and [N:attributeK] per item
# attribute.

def parse_keyring(data):
    header = []
    items = {}
    order = []
    current = None
    for raw in data.decode("utf-8").splitlines():
        line = raw.rstrip("\r")
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1]
            if name == "keyring":
                current = header
                continue
            item = name.split(":", 1)[0]
            if not item.isdigit():
                raise ValueError(f"unexpected keyring section {name}")
            if item not in items:
                items[item] = []
                order.append(item)
            current = items[item]
            current.append(line)
            continue
        if current is None:
            if line.strip():
                raise ValueError("keyring data before a section")
            continue
        current.append(line)
    return header, [(item, items[item]) for item in order]


def _item_identity(lines):
    """An item is identified by its display name and attributes, not its secret."""
    keep = []
    in_attribute = False
    for line in lines:
        if line.startswith("["):
            in_attribute = ":" in line
            if in_attribute:
                keep.append("[attr]")
            continue
        if not in_attribute and line.startswith(("display-name=", "item-type=")):
            keep.append(line)
        elif in_attribute and line.strip():
            keep.append(line)
    return tuple(keep)


def keyring_is_encrypted(data):
    return not data.startswith(b"[keyring]")


def keyring_items(data):
    return parse_keyring(data)[1]


def merge_keyrings(trial, current):
    """Merge the trial keyring into this computer's; trial items win on a clash.

    Items are renumbered; an item that only exists on this computer keeps its
    content.
    """
    trial_header, trial_items = parse_keyring(trial)
    current_header, current_items = parse_keyring(current)
    trial_identities = {_item_identity(lines) for _, lines in trial_items}
    merged = [lines for _, lines in trial_items]
    merged += [lines for _, lines in current_items if _item_identity(lines) not in trial_identities]
    header = current_header or trial_header
    output = ["[keyring]", *[line for line in header if line.strip()], ""]
    for number, lines in enumerate(merged, start=1):
        for line in lines:
            if line.startswith("["):
                section = line[1:-1]
                rest = section.split(":", 1)
                output.append(f"[{number}{':' + rest[1] if len(rest) == 2 else ''}]")
            else:
                output.append(line)
        if output[-1].strip():
            output.append("")
    return ("\n".join(output).rstrip("\n") + "\n").encode("utf-8")
