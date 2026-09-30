"""try-omarchy-import export: pack a trial for another computer or a clean install.

Runs inside Try Omarchy as the trial account. It writes one archive,
omarchy-export-<time>.tar.gz, holding:

- trial-root/: the parts of the trial's filesystem the importer reads, with
  only what the user changed in the chosen groups. Deciding what changed has
  to happen here, where file times are real; an extracted copy cannot tell.
  The /etc/skel copies of exported files come along as merge bases.
- try-omarchy-import.pyz and import.sh, so the new install runs the same
  importer: ./import.sh
- export.json and README.txt.

Keys, sign-ins and browser profiles are only included when picked, and the
archive says so, because it then contains secrets.
"""

import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import tarfile
import tempfile
import time
import zipfile

from . import VERSION, classify
from .packages import PackagePlan
from .plan import TrialDefaults, scan
from .selection import SelectionError, option_rows, resolve_selection
from .trial import BACKGROUND_LINK, THEME_NAME, Trial, TrialError
from .system import running_programs
from .ui import UI, Cancelled, human_size, plain_label

SPOOL_IN_MEMORY = 16 * 1024 * 1024

IMPORT_SH = """#!/bin/bash
# Bring this Try Omarchy export into this Omarchy install. Run it as the
# account that should receive it, from anywhere: ./import.sh
# Add --dry-run to see what would happen, or --help for all options.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
exec python3 "$here/try-omarchy-import.pyz" --root "$here/trial-root" "$@"
"""

README = """This is an export of a Try Omarchy trial, made {created}.

To bring it into an installed Omarchy, extract it and run import.sh as the
account that should receive it:

    tar -xzf {name}.tar.gz
    {name}/import.sh

It shows what it can bring over and asks before changing anything. Anything
it replaces is backed up to ~/.local/share/try-omarchy-import/backups.
{secrets}"""

SECRETS_NOTE = """
This export contains keys, sign-ins or browser profiles. Anyone with the file
can use them: keep it private and delete it once you have imported it.
"""


class ExportError(Exception):
    pass


def importer_archive():
    """The running importer as .pyz bytes: the file itself when running from
    one, otherwise the package zipped the same way (development and tests)."""
    package = Path(__file__).resolve().parent
    for parent in package.parents:
        if parent.suffix == ".pyz" and parent.is_file():
            return parent.read_bytes()
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(package.glob("*.py")):
            archive.writestr(f"omarchy_import/{path.name}", path.read_bytes())
        archive.writestr("__main__.py",
                         "import sys\n\nfrom omarchy_import.cli import main\n\nsys.exit(main())\n")
    return b"#!/usr/bin/env python3\n" + buffer.getvalue()


class Writer:
    """Streams the archive; every member is placed under one top folder."""

    def __init__(self, tar, top):
        self.tar = tar
        self.top = top
        self.directories = set()

    def _info(self, name, kind, mode, mtime):
        info = tarfile.TarInfo(f"{self.top}/{name}")
        info.type = kind
        info.mode = mode
        info.mtime = mtime
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        return info

    def directory(self, name, mode=0o755, mtime=None):
        parts = name.split("/")
        for index in range(1, len(parts) + 1):
            prefix = "/".join(parts[:index])
            if prefix in self.directories:
                continue
            self.directories.add(prefix)
            own_mode = mode if index == len(parts) else 0o755
            self.tar.addfile(self._info(prefix, tarfile.DIRTYPE, own_mode,
                                        mtime if mtime is not None else time.time()))

    def data(self, name, data, mode=0o644, mtime=None):
        self._parent(name)
        info = self._info(name, tarfile.REGTYPE, mode, mtime if mtime is not None else time.time())
        info.size = len(data)
        self.tar.addfile(info, io.BytesIO(data))

    def copy(self, name, source):
        """Copy a regular file or symlink from the trial without following links.

        The trial is running, so files can change or vanish while exporting.
        Each file is read into a spool first and the archive member is written
        from that, so a file that shrinks or grows cannot corrupt the archive.
        Returns the bytes written, or None if the file disappeared.
        """
        try:
            metadata = os.lstat(source)
            if stat.S_ISLNK(metadata.st_mode):
                info = self._info(name, tarfile.SYMTYPE, 0o777, metadata.st_mtime)
                info.linkname = os.readlink(source)
                self._parent(name)
                self.tar.addfile(info)
                return 0
            fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        except FileNotFoundError:
            return None
        with os.fdopen(fd, "rb") as stream, \
                tempfile.SpooledTemporaryFile(max_size=SPOOL_IN_MEMORY) as spool:
            opened = os.fstat(stream.fileno())
            if not stat.S_ISREG(opened.st_mode):
                return None
            shutil.copyfileobj(stream, spool, 1024 * 1024)
            size = spool.tell()
            spool.seek(0)
            info = self._info(name, tarfile.REGTYPE, opened.st_mode & 0o777, opened.st_mtime)
            info.size = size
            self._parent(name)
            self.tar.addfile(info, spool)
        return size

    def _parent(self, name):
        parent = name.rpartition("/")[0]
        if parent:
            self.directory(parent)


def default_destination():
    if os.path.ismount("/mnt/host") and os.access("/mnt/host", os.W_OK):
        return Path("/mnt/host")
    return Path.home()


def free_bytes(path):
    stats = os.statvfs(path)
    return stats.f_bavail * stats.f_frsize


def build_parser():
    parser = argparse.ArgumentParser(
        prog="try-omarchy-export",
        description="Pack your Try Omarchy setup into one archive for another computer or a "
                    "clean Omarchy install.")
    parser.add_argument("destination", nargs="?",
                        help="folder for the archive (default: the shared Windows folder when "
                             "mounted, otherwise your home folder)")
    parser.add_argument("--select", metavar="LIST",
                        help="comma-separated groups instead of asking, as for the importer")
    parser.add_argument("--yes", action="store_true", help="accept the defaults without asking")
    parser.add_argument("--root", default="/", help=argparse.SUPPRESS)
    parser.add_argument("--version", action="version", version=f"try-omarchy-export {VERSION}")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    ui = UI(interactive=not args.yes)
    try:
        return run(args, ui)
    except (ExportError, TrialError) as error:
        ui.error(str(error))
        return 1
    except OSError as error:
        ui.error(f"{error}. No archive was written.")
        return 1
    except (Cancelled, KeyboardInterrupt):
        ui.say()
        ui.say("Stopped. No archive was written.")
        return 130


def run(args, ui):
    destination = Path(args.destination) if args.destination else default_destination()
    if not destination.is_dir() or not os.access(destination, os.W_OK):
        raise ExportError(f"cannot write to {destination}")
    root = Path(args.root)
    trial = Trial(root)
    mine = [account for account in trial.accounts if account.uid == os.getuid()]
    if mine:
        trial.choose(mine[0].name)
    home = trial.home
    baseline = trial.baseline_ns()
    ui.heading("Export from Try Omarchy")
    ui.say("Looking through your trial...")
    classifier = classify.Classifier(_skel_config_names(trial.skel))
    inventory = scan(home, classifier, baseline)
    packages_added = trial.packages().added
    theme = trial.theme()
    free = free_bytes(destination)

    added = PackagePlan(repo=list(packages_added), flatpaks=trial.flatpaks())
    rows = option_rows(inventory, added, theme, free)
    if args.select:
        try:
            chosen = resolve_selection(args.select, rows)
        except SelectionError as error:
            raise ExportError(str(error)) from None
    else:
        picks = ui.choose_many("What do you want to export?",
                               [plain_label(row[1]) for row in rows], [row[2] for row in rows])
        chosen = {row[0] for row, pick in zip(rows, picks) if pick}
    groups = [inventory.groups[row[0]] for row in rows
              if row[0] in chosen and row[0] in inventory.groups]
    running = running_programs()
    for group in [group for group in groups if group.kind == classify.BROWSER]:
        identity = group.id.split("/", 1)[1]
        if running & set(classify.BROWSER_PROCESSES.get(identity, ())):
            ui.warn(f"{group.label} is open, so its profile is left out. Close it and export "
                    "again to include it.")
            groups.remove(group)
    keyring = _keyring_for_browsers(inventory, groups)
    if keyring is not None:
        groups.append(keyring)
    secrets = any(group.kind in (classify.KEYS, classify.BROWSER) for group in groups)
    if secrets:
        ui.warn("The archive will contain keys, sign-ins or a browser profile. Anyone with the "
                "file can use them, so keep it private.")

    defaults = TrialDefaults(trial.skel, baseline)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    name = f"omarchy-export-{stamp}"
    final = destination / f"{name}.tar.gz"
    partial = destination / f".{name}.tar.gz.partial"
    account = trial.account
    home_prefix = f"trial-root/{account.home.lstrip('/')}"
    written = 0
    included = 0
    try:
        with tarfile.open(partial, "w:gz", compresslevel=6) as tar:
            out = Writer(tar, name)
            out.data("import.sh", IMPORT_SH.encode(), 0o755)
            out.data("try-omarchy-import.pyz", importer_archive(), 0o755)
            out.data("README.txt", README.format(
                created=time.strftime("%B %d %Y"), name=name,
                secrets=SECRETS_NOTE if secrets else "").encode())
            _write_system(out, trial, account, chosen, packages_added)
            out.directory(home_prefix, 0o700)
            for group in groups:
                changed_only = group.kind in (classify.SETTINGS, classify.APPS, classify.KEYS,
                                              classify.FILES)
                for entry in group.entries:
                    source = home / entry.relative
                    if entry.kind == "directory":
                        if not changed_only:
                            out.directory(f"{home_prefix}/{entry.relative}", entry.mode)
                        continue
                    if changed_only and defaults.is_default(
                            entry.relative, entry, read=lambda path=source: _read(path)):
                        continue
                    _write_parents(out, home, home_prefix, entry.relative)
                    copied = out.copy(f"{home_prefix}/{entry.relative}", source)
                    if copied is None:
                        continue
                    written += copied
                    included += 1
                    skel = trial.skel / entry.relative
                    if os.path.lexists(skel) and not os.path.isdir(skel):
                        out.copy(f"trial-root/etc/skel/{entry.relative}", skel)
            _write_state(out, home, home_prefix, trial, chosen)
            out.data("export.json", json.dumps({
                "kind": "try-omarchy-export", "schemaVersion": 2,
                "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "exporter": VERSION, "omarchyVersion": trial.omarchy_version(),
                "user": account.name, "groups": [group.id for group in groups],
                "packages": "packages" in chosen, "theme": theme if "theme" in chosen else None,
                "secrets": secrets,
            }, indent=2).encode() + b"\n")
        os.replace(partial, final)
    except BaseException:
        with contextlib.suppress(FileNotFoundError):
            partial.unlink()
        raise
    ui.say()
    ui.say(f"Exported {included} files ({human_size(written)} before compression) to {final}")
    ui.say("On the new install, extract it and run import.sh inside:")
    ui.say(f"  tar -xzf {final.name} && {name}/import.sh")
    return 0


def _keyring_for_browsers(inventory, groups):
    """A browser profile's saved logins need the login keyring; bring it along."""
    if not any(group.kind == classify.BROWSER for group in groups):
        return None
    if any(group.id == classify.KEYS_GROUP for group in groups):
        return None
    keys = inventory.groups.get(classify.KEYS_GROUP)
    if keys is None:
        return None
    keyring = type(keys)(classify.KEYS_GROUP, classify.KEYS, keys.label,
                         source_base=keys.source_base)
    for entry in keys.entries:
        if entry.relative.startswith(".local/share/keyrings"):
            keyring.entries.append(entry)
    return keyring if keyring.entries else None


def _read(path):
    with open(path, "rb") as source:
        return source.read()


def _skel_config_names(skel):
    try:
        return {entry.name for entry in os.scandir(Path(skel) / ".config")}
    except OSError:
        return set()


def _write_parents(out, home, prefix, relative):
    parts = relative.split("/")[:-1]
    for index in range(1, len(parts) + 1):
        path = "/".join(parts[:index])
        if f"{prefix}/{path}" in out.directories:
            continue
        metadata = os.lstat(home / path)
        out.directory(f"{prefix}/{path}", metadata.st_mode & 0o777, metadata.st_mtime)


def _write_system(out, trial, account, chosen, packages_added):
    root = trial.root
    passwd_line = f"{account.name}:x:{account.uid}:{account.gid}::{account.home}:/bin/bash\n"
    out.data("trial-root/etc/passwd", passwd_line.encode())
    groups = "".join(f"{group}:x:0:{account.name}\n" for group in account.groups)
    out.data("trial-root/etc/group", groups.encode())
    # Skeleton folder names decide which .config folders count as settings.
    for name in sorted(_skel_config_names(trial.skel)):
        if os.path.isdir(trial.skel / ".config" / name):
            out.directory(f"trial-root/etc/skel/.config/{name}")
    for relative in ("usr/share/try-omarchy/build-spec.json",
                     "usr/share/try-omarchy/packages.lock.txt", "usr/share/omarchy/version"):
        if (root / relative).is_file():
            out.copy(f"trial-root/{relative}", root / relative)
    if "packages" in chosen:
        for name in packages_added:
            out.data(f"trial-root/var/lib/pacman/local/{name}-0-0/desc",
                     f"%NAME%\n{name}\n".encode())
        for app, scope in trial.flatpaks():
            if scope == "system":
                out.directory(f"trial-root/var/lib/flatpak/app/{app}/current")
            else:
                out.directory(f"trial-root/{account.home.lstrip('/')}/.local/share/flatpak/app/"
                              f"{app}/current")
    for unit in trial.enabled_services():
        info = tarfile.TarInfo(f"{out.top}/trial-root/etc/systemd/system/multi-user.target.wants/"
                               f"{unit}")
        info.type = tarfile.SYMTYPE
        info.linkname = f"/usr/lib/systemd/system/{unit}"
        info.mtime = time.time()
        out.directory("trial-root/etc/systemd/system/multi-user.target.wants")
        out.tar.addfile(info)


def _write_state(out, home, prefix, trial, chosen):
    """Theme, background and share links, which the import reads but never copies."""
    if "theme" in chosen:
        for relative in (THEME_NAME, BACKGROUND_LINK):
            if os.path.lexists(home / relative):
                _write_parents(out, home, prefix, relative)
                out.copy(f"{prefix}/{relative}", home / relative)
    for name in trial.share_names():
        out.copy(f"{prefix}/{name}", home / name)
