"""Read what the importer needs to know about a trial from its root filesystem.

The root is either the trial disk mounted read-only or an export directory
that reproduces the same layout (etc/passwd, etc/skel, home/<user>, ...).
Nothing here writes to the trial.

Every path inside the trial is looked up with Trial.path, which follows links
the way the trial itself would: an absolute link target starts again at the
trial's root and .. never climbs above it. A link on the trial disk can
therefore never point a read at this computer's own files.
"""

from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import stat

from . import names

FINALIZE_MARKER = ".local/state/omarchy/done/finalize-user"
FIRST_RUN_MARKER = ".local/state/omarchy/done/first-run-user"
THEME_NAME = ".local/state/omarchy/current/theme.name"
BACKGROUND_LINK = ".local/state/omarchy/current/background"
CURRENT_THEME_DIR = ".local/state/omarchy/current/theme"
MAX_LINKS = 40

# Services Try Omarchy or a virtual machine needs, which mean nothing on a
# real install.
TRY_UNIT_PREFIXES = ("try-omarchy", "omarchy-provision", "clipboard-bridge", "omarchy-windows-",
                     "mnt-host", "qemu-guest-agent", "spice-vdagent", "vboxservice",
                     "vmtoolsd", "systemd-", "getty@", "serial-getty@")


class TrialError(Exception):
    pass


@dataclass(frozen=True)
class Account:
    name: str
    uid: int
    gid: int
    home: str
    groups: tuple = ()


@dataclass
class Packages:
    explicit: list = field(default_factory=list)
    factory: set = field(default_factory=set)
    added: list = field(default_factory=list)
    # Entries whose name is not a valid package name; never installed.
    invalid: int = 0


def _read_text(path, limit=1024 * 1024):
    """A regular file's text, or None. Never follows a link in the last
    component and never waits on a pipe or device."""
    if path is None:
        return None
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return None
    with os.fdopen(fd, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            return None
        data = source.read(limit + 1)
    if len(data) > limit:
        return None
    return data.decode("utf-8", "replace")


def _components(path):
    return [part for part in path.split("/") if part not in ("", ".")]


def valid_home(home):
    """An account home the importer accepts: /home/<name>[/...], absolute,
    without . or .. parts and without control characters."""
    parts = home.split("/")
    return (len(parts) >= 3 and parts[0] == "" and parts[1] == "home"
            and all(part not in ("", ".", "..") for part in parts[2:])
            and not any(ord(character) < 32 or ord(character) == 127 for character in home))


def parse_passwd(text):
    accounts = []
    for line in (text or "").splitlines():
        fields = line.split(":")
        if len(fields) != 7:
            continue
        name, _, uid, gid, _, home, shell = fields
        try:
            uid, gid = int(uid), int(gid)
        except ValueError:
            continue
        accounts.append((name, uid, gid, home, shell))
    return accounts


def people(passwd_text):
    """Accounts a person signs in with: normal uid range and a real shell."""
    result = []
    for name, uid, gid, home, shell in parse_passwd(passwd_text):
        if not 1000 <= uid < 60000:
            continue
        if len(home) > 1:
            home = home.rstrip("/")
        if shell.endswith(("nologin", "/false")) or not valid_home(home) \
                or not names.account(name):
            continue
        result.append(Account(name, uid, gid, home))
    return result


def groups_of(group_text, name, primary_gid):
    found = []
    for line in (group_text or "").splitlines():
        fields = line.split(":")
        if len(fields) != 4:
            continue
        members = [member for member in fields[3].split(",") if member]
        if name in members or fields[2] == str(primary_gid):
            found.append(fields[0])
    return tuple(sorted(set(found)))


def parse_pacman_desc(text):
    """Return (name, explicit) from a pacman local database desc file."""
    name = None
    reason = "0"
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line == "%NAME%" and index + 1 < len(lines):
            name = lines[index + 1].strip()
        elif line == "%REASON%" and index + 1 < len(lines):
            reason = lines[index + 1].strip()
    return name, reason != "1"


def _change_time(path):
    try:
        metadata = os.stat(path, follow_symlinks=False)
    except (OSError, TypeError):
        return None
    return max(metadata.st_ctime_ns, metadata.st_mtime_ns)


class Trial:
    def __init__(self, root, user=None):
        self.root = Path(os.path.realpath(root))
        passwd = _read_text(self.path("etc/passwd"))
        if passwd is None:
            raise TrialError("this does not look like an Omarchy disk (no /etc/passwd)")
        self._homes = {}
        candidates = []
        for account in people(passwd):
            home = self._home_directory(account.home)
            if home is not None:
                self._homes[account.name] = home
                candidates.append(account)
        if user is not None:
            candidates = [account for account in candidates if account.name == user]
            if not candidates:
                raise TrialError(f"the trial has no account named {user}")
        if not candidates:
            raise TrialError("the trial disk has no user account yet; start Try Omarchy once "
                             "and finish setup before importing")
        group_text = _read_text(self.path("etc/group"))
        self.accounts = [Account(account.name, account.uid, account.gid, account.home,
                                 groups_of(group_text, account.name, account.gid))
                         for account in candidates]
        self.account = self.accounts[0]

    def path(self, relative, follow=True):
        """Where relative, a path inside the trial such as etc/passwd, is on
        this computer, or None if it is missing, something on the way is not
        a folder, or links loop. Links are followed inside the trial (see the
        module notes); with follow=False a link in the last part is not."""
        pending = _components(str(relative))
        resolved = []
        links = 0
        while pending:
            name = pending.pop(0)
            if name == "..":
                if resolved:
                    resolved.pop()
                continue
            current = os.path.join(self.root, *resolved, name)
            if not pending and not follow:
                resolved.append(name)
                break
            try:
                metadata = os.lstat(current)
            except OSError:
                return None
            if stat.S_ISLNK(metadata.st_mode):
                links += 1
                if links > MAX_LINKS:
                    return None
                try:
                    target = os.readlink(current)
                except OSError:
                    return None
                if target.startswith("/"):
                    resolved = []
                pending = _components(target) + pending
                continue
            if pending and not stat.S_ISDIR(metadata.st_mode):
                return None
            resolved.append(name)
        return Path(self.root, *resolved)

    def _home_directory(self, home):
        """The account's home folder inside the trial, if it is a real folder."""
        path = self.path(home, follow=False)
        try:
            metadata = os.lstat(path) if path is not None else None
        except OSError:
            return None
        if metadata is None or not stat.S_ISDIR(metadata.st_mode):
            return None
        return path

    def _in_home(self, relative, follow=True):
        return self.path(f"{self.account.home}/{relative}", follow=follow)

    def choose(self, name):
        for account in self.accounts:
            if account.name == name:
                self.account = account
                return
        raise TrialError(f"the trial has no account named {name}")

    @property
    def home(self):
        return self._homes[self.account.name]

    @property
    def skel(self):
        """The trial's /etc/skel folder, or None."""
        path = self.path("etc/skel")
        return path if path is not None and path.is_dir() else None

    @property
    def is_try(self):
        path = self.path("usr/share/try-omarchy")
        return path is not None and path.is_dir()

    def omarchy_version(self):
        spec = _read_text(self.path("usr/share/try-omarchy/build-spec.json"))
        if spec:
            try:
                version = json.loads(spec)["upstream"]["version"]
                if isinstance(version, str) and version:
                    return version
            except (ValueError, KeyError, TypeError):
                pass
        version = _read_text(self.path("usr/share/omarchy/version"))
        return version.strip() if version else "unknown"

    def baseline_ns(self):
        """When Omarchy finished setting up the trial account, or None."""
        times = [_change_time(self._in_home(marker, follow=False))
                 for marker in (FINALIZE_MARKER, FIRST_RUN_MARKER)]
        times = [value for value in times if value is not None]
        return max(times) if times else None

    def theme(self):
        name = _read_text(self._in_home(THEME_NAME), 4096)
        if name and names.theme(name.strip()):
            return name.strip()
        return None

    def background(self):
        """The trial's current background as a path inside the trial home, or None."""
        link = self._in_home(BACKGROUND_LINK, follow=False)
        try:
            return os.readlink(link) if link is not None else None
        except OSError:
            return None

    def packages(self):
        local = self.path("var/lib/pacman/local")
        result = Packages()
        if local is not None and local.is_dir():
            for entry in sorted(os.scandir(local), key=lambda item: item.name):
                if not entry.is_dir(follow_symlinks=False):
                    continue
                text = _read_text(Path(entry.path) / "desc")
                if not text:
                    continue
                name, explicit = parse_pacman_desc(text)
                if not name or not explicit:
                    continue
                if names.package(name):
                    result.explicit.append(name)
                else:
                    result.invalid += 1
        lock = _read_text(self.path("usr/share/try-omarchy/packages.lock.txt"), 16 * 1024 * 1024)
        if lock:
            result.factory = {line.split()[0] for line in lock.splitlines() if line.strip()}
        result.added = [name for name in result.explicit
                        if name not in result.factory and not name.startswith("try-omarchy")]
        return result

    def flatpaks(self):
        """Flatpak app IDs as (id, scope) pairs, where scope is system or user."""
        apps = []
        for scope, relative in (("system", "var/lib/flatpak/app"),
                                ("user", f"{self.account.home}/.local/share/flatpak/app")):
            directory = self.path(relative)
            if directory is None or not directory.is_dir():
                continue
            for entry in sorted(os.scandir(directory), key=lambda item: item.name):
                if not entry.is_dir(follow_symlinks=False) or not names.flatpak(entry.name):
                    continue
                if self.path(f"{relative}/{entry.name}/current") is not None:
                    apps.append((entry.name, scope))
        return apps

    def enabled_services(self):
        """System units the user enabled in the trial, minus Try and VM plumbing.

        Units the image or Omarchy's setup enabled were linked before the
        account was set up; only links made later count.
        """
        baseline = self.baseline_ns()
        units = set()
        base = self.path("etc/systemd/system")
        if base is None or not base.is_dir():
            return []
        for wants in sorted(os.scandir(base), key=lambda item: item.name):
            if not wants.name.endswith((".wants", ".requires")) or \
                    not wants.is_dir(follow_symlinks=False):
                continue
            for link in sorted(os.scandir(wants.path), key=lambda item: item.name):
                name = link.name
                if not names.unit(name) or name.startswith(TRY_UNIT_PREFIXES):
                    continue
                if baseline is not None:
                    metadata = os.lstat(link.path)
                    if max(metadata.st_ctime_ns, metadata.st_mtime_ns) <= baseline:
                        continue
                units.add(name)
        return sorted(units)

    def share_names(self):
        """Names of home entries that link to Try Omarchy's shared Windows folder."""
        found = set()
        try:
            entries = list(os.scandir(self.home))
        except OSError:
            return found
        for entry in entries:
            if entry.is_symlink():
                try:
                    target = os.readlink(entry.path)
                except OSError:
                    continue
                if target == "/mnt/host" or target.startswith("/mnt/host/"):
                    found.add(entry.name)
        return found


def marker_time(home):
    """The latest Omarchy setup marker in home on this computer, in nanoseconds, or None."""
    times = [_change_time(Path(home) / marker) for marker in (FINALIZE_MARKER, FIRST_RUN_MARKER)]
    times = [value for value in times if value is not None]
    return max(times) if times else None
