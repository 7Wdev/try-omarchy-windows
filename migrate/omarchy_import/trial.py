"""Read what the importer needs to know about a trial from its root filesystem.

The root is either the trial disk mounted read-only or an export directory
that reproduces the same layout (etc/passwd, etc/skel, home/<user>, ...).
Nothing here writes to the trial.
"""

from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import stat

FINALIZE_MARKER = ".local/state/omarchy/done/finalize-user"
FIRST_RUN_MARKER = ".local/state/omarchy/done/first-run-user"
THEME_NAME = ".local/state/omarchy/current/theme.name"
BACKGROUND_LINK = ".local/state/omarchy/current/background"
CURRENT_THEME_DIR = ".local/state/omarchy/current/theme"

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


def _read_text(path, limit=1024 * 1024):
    try:
        with open(path, "rb") as source:
            if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
                return None
            data = source.read(limit + 1)
    except (FileNotFoundError, NotADirectoryError, PermissionError, IsADirectoryError):
        return None
    if len(data) > limit:
        return None
    return data.decode("utf-8", "replace")


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
        if shell.endswith(("nologin", "/false")) or not home.startswith("/home/"):
            continue
        result.append(Account(name, uid, gid, home))
    return result


def groups_of(group_text, name, primary_gid):
    names = []
    for line in (group_text or "").splitlines():
        fields = line.split(":")
        if len(fields) != 4:
            continue
        members = [member for member in fields[3].split(",") if member]
        if name in members or fields[2] == str(primary_gid):
            names.append(fields[0])
    return tuple(sorted(set(names)))


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


class Trial:
    def __init__(self, root, user=None):
        self.root = Path(root)
        if not (self.root / "etc/passwd").is_file():
            raise TrialError("this does not look like an Omarchy disk (no /etc/passwd)")
        passwd = _read_text(self.root / "etc/passwd")
        candidates = people(passwd)
        candidates = [account for account in candidates
                      if (self.root / account.home.lstrip("/")).is_dir()
                      and not (self.root / account.home.lstrip("/")).is_symlink()]
        if user is not None:
            candidates = [account for account in candidates if account.name == user]
            if not candidates:
                raise TrialError(f"the trial has no account named {user}")
        if not candidates:
            raise TrialError("the trial disk has no user account yet; start Try Omarchy once "
                             "and finish setup before importing")
        group_text = _read_text(self.root / "etc/group")
        self.accounts = [Account(account.name, account.uid, account.gid, account.home,
                                 groups_of(group_text, account.name, account.gid))
                         for account in candidates]
        self.account = self.accounts[0]

    def choose(self, name):
        for account in self.accounts:
            if account.name == name:
                self.account = account
                return
        raise TrialError(f"the trial has no account named {name}")

    @property
    def home(self):
        return self.root / self.account.home.lstrip("/")

    @property
    def skel(self):
        return self.root / "etc/skel"

    @property
    def is_try(self):
        return (self.root / "usr/share/try-omarchy").is_dir()

    def omarchy_version(self):
        spec = _read_text(self.root / "usr/share/try-omarchy/build-spec.json")
        if spec:
            try:
                version = json.loads(spec)["upstream"]["version"]
                if isinstance(version, str) and version:
                    return version
            except (ValueError, KeyError, TypeError):
                pass
        version = _read_text(self.root / "usr/share/omarchy/version")
        return version.strip() if version else "unknown"

    def baseline_ns(self):
        """When Omarchy finished setting up the trial account, or None."""
        return marker_time(self.home)

    def theme(self):
        name = _read_text(self.home / THEME_NAME, 4096)
        if name and name.strip():
            return name.strip()
        return None

    def background(self):
        """The trial's current background as a path inside the trial home, or None."""
        link = self.home / BACKGROUND_LINK
        try:
            target = os.readlink(link)
        except OSError:
            return None
        return target

    def packages(self):
        local = self.root / "var/lib/pacman/local"
        result = Packages()
        if local.is_dir():
            for entry in sorted(os.scandir(local), key=lambda item: item.name):
                if not entry.is_dir(follow_symlinks=False):
                    continue
                text = _read_text(Path(entry.path) / "desc")
                if not text:
                    continue
                name, explicit = parse_pacman_desc(text)
                if name and explicit:
                    result.explicit.append(name)
        lock = _read_text(self.root / "usr/share/try-omarchy/packages.lock.txt", 16 * 1024 * 1024)
        if lock:
            result.factory = {line.split()[0] for line in lock.splitlines() if line.strip()}
        result.added = [name for name in result.explicit
                        if name not in result.factory and not name.startswith("try-omarchy")]
        return result

    def flatpaks(self):
        """Flatpak app IDs as (id, scope) pairs, where scope is system or user."""
        apps = []
        for scope, directory in (("system", self.root / "var/lib/flatpak/app"),
                                 ("user", self.home / ".local/share/flatpak/app")):
            if not directory.is_dir():
                continue
            for entry in sorted(os.scandir(directory), key=lambda item: item.name):
                if entry.is_dir(follow_symlinks=False) and (Path(entry.path) / "current").exists():
                    apps.append((entry.name, scope))
        return apps

    def enabled_services(self):
        """System units the user enabled in the trial, minus Try and VM plumbing.

        Units the image or Omarchy's setup enabled were linked before the
        account was set up; only links made later count.
        """
        baseline = self.baseline_ns()
        units = set()
        base = self.root / "etc/systemd/system"
        if not base.is_dir():
            return []
        for wants in base.iterdir():
            if not wants.name.endswith((".wants", ".requires")) or not wants.is_dir():
                continue
            for link in wants.iterdir():
                name = link.name
                if not name.endswith((".service", ".socket", ".timer", ".path")):
                    continue
                if name.startswith(TRY_UNIT_PREFIXES):
                    continue
                if baseline is not None:
                    metadata = os.lstat(link)
                    if max(metadata.st_ctime_ns, metadata.st_mtime_ns) <= baseline:
                        continue
                units.add(name)
        return sorted(units)

    def share_names(self):
        """Names of home entries that link to Try Omarchy's shared Windows folder."""
        names = set()
        try:
            entries = list(os.scandir(self.home))
        except OSError:
            return names
        for entry in entries:
            if entry.is_symlink():
                try:
                    target = os.readlink(entry.path)
                except OSError:
                    continue
                if target == "/mnt/host" or target.startswith("/mnt/host/"):
                    names.add(entry.name)
        return names


def marker_time(home):
    """The latest Omarchy setup marker in home, as st_mtime_ns, or None."""
    times = []
    for marker in (FINALIZE_MARKER, FIRST_RUN_MARKER):
        try:
            metadata = os.stat(Path(home) / marker, follow_symlinks=False)
        except OSError:
            continue
        times.append(max(metadata.st_ctime_ns, metadata.st_mtime_ns))
    return max(times) if times else None
