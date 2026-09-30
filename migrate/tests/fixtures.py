"""Synthetic trial disks and home directories for the importer tests.

The importer tells Omarchy's own setup apart from the user's changes by the
setup marker's time, compared with each file's ctime (which, unlike mtime,
cannot be set back). So fixtures are written in real order: setup files,
then the marker, then the user's changes, with a pause between so the
coarse kernel clock ticks. mtimes are still set to fixed values for tests
that look at them.
"""

import os
from pathlib import Path
import shutil
import time

SKEL_TIME = 1_700_000_000
TRIAL_SETUP = SKEL_TIME + 100
TRIAL_EDIT = SKEL_TIME + 1_000
HOME_SETUP = SKEL_TIME + 5_000
HOME_EDIT = SKEL_TIME + 6_000

TRIAL_BINDINGS = b"""-- Omarchy bindings
bind("SUPER", "RETURN", "terminal")
bind("SUPER", "B", "browser")
"""
NEW_BINDINGS = b"""-- Omarchy bindings, now with a comment for the new release
bind("SUPER", "RETURN", "terminal")
bind("SUPER", "B", "browser")
"""
PINCH_BLOCK = b"""-- BEGIN TRY OMARCHY PINCH DEVICE
-- Try Omarchy's virtual pinch touchpad only carries Windows pinch gestures.
do
  local rules = "/usr/share/try-omarchy/pinch-input.lua"
end
-- END TRY OMARCHY PINCH DEVICE
"""
INPUT = b'input({ kb_layout = "us" })\n'
MONITORS = b'monitor("", "preferred", "auto", 1)\n'
QEMU_BLOCK = b"-- BEGIN OMARCHY QEMU PROFILE\nif qemu then end\n-- END OMARCHY QEMU PROFILE\n"
GIT_CONFIG = b"[init]\n\tdefaultBranch = main\n"
KEYRING_EMPTY = b"""[keyring]
display-name=Default keyring
ctime=1700000000
mtime=0
lock-on-idle=false
lock-after=false
"""
KEYRING_CHROMIUM = KEYRING_EMPTY + b"""
[1]
item-type=0
display-name=Chromium Safe Storage
secret=trial-secret
mtime=1700001000
ctime=1700001000

[1:attribute0]
name=application
type=string
value=chromium
"""


def write(path, data, mtime, mode=0o644):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    os.chmod(path, mode)
    os.utime(path, (mtime, mtime))


def link(path, target, mtime=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    os.symlink(target, path)
    if mtime is not None:
        os.utime(path, (mtime, mtime), follow_symlinks=False)


def seed(skel, home, mtime=None):
    """Copy a skeleton into a home directory like useradd does."""
    shutil.copytree(skel, home, symlinks=True, dirs_exist_ok=True)


def tick():
    time.sleep(0.03)


def marker(home):
    """Omarchy finished setting the account up, now."""
    tick()
    path = Path(home) / ".local/state/omarchy/done/finalize-user"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.touch()
    tick()


def make_trial(base, user="omarchy", uid=None, gid=None):
    """A trial root at base/trial with a customized home. Returns (root, home)."""
    uid = os.getuid() if uid is None else uid
    gid = os.getgid() if gid is None else gid
    root = Path(base) / "trial"
    write(root / "etc/passwd", (
        "root:x:0:0::/root:/bin/bash\n"
        f"{user}:x:{uid}:{gid}:Try Omarchy:/home/{user}:/bin/bash\n"
        "nobody:x:65534:65534:Nobody:/:/usr/bin/nologin\n").encode(), SKEL_TIME)
    write(root / "etc/group", f"wheel:x:998:{user}\ndocker:x:970:{user}\n{user}:x:{gid}:\n"
          .encode(), SKEL_TIME)
    skel = root / "etc/skel"
    write(skel / ".config/hypr/bindings.lua", TRIAL_BINDINGS, SKEL_TIME)
    write(skel / ".config/hypr/input.lua", INPUT + b"\n" + PINCH_BLOCK, SKEL_TIME)
    write(skel / ".config/hypr/monitors.lua", MONITORS + b"\n" + QEMU_BLOCK, SKEL_TIME)
    write(skel / ".config/git/config", GIT_CONFIG, SKEL_TIME)
    write(skel / ".config/omarchy/branding/about.txt", b"Try Omarchy logo\n", SKEL_TIME)
    write(skel / ".bashrc", b"source /usr/share/omarchy/default/bash/rc\n", SKEL_TIME)
    write(skel / ".local/state/omarchy/toggles/suspend-off", b"", SKEL_TIME)
    link(skel / ".local/share/omarchy", "/usr/share/omarchy")
    write(root / "usr/share/try-omarchy/build-spec.json",
          b'{"upstream": {"version": "4.0.3"}}', SKEL_TIME)
    write(root / "usr/share/try-omarchy/packages.lock.txt", b"base 1-1\nomarchy 4.0.3-1\n",
          SKEL_TIME)
    for name, explicit in (("base", True), ("omarchy", True), ("cowsay", True),
                           ("figlet", True), ("zlib", False)):
        desc = f"%NAME%\n{name}\n\n%VERSION%\n1-1\n\n"
        if not explicit:
            desc += "%REASON%\n1\n\n"
        write(root / f"var/lib/pacman/local/{name}-1-1/desc", desc.encode(), SKEL_TIME)
    link(root / "etc/systemd/system/multi-user.target.wants/docker.service",
         "/usr/lib/systemd/system/docker.service")
    link(root / "etc/systemd/system/multi-user.target.wants/try-omarchy-agent.service",
         "/etc/systemd/system/try-omarchy-agent.service")

    home = root / "home" / user
    seed(skel, home)
    # Written by Omarchy while setting the account up, never touched again.
    write(home / ".XCompose", b"include \"%L\"\n<Multi_key> <n> : \"Try Omarchy\"\n", TRIAL_SETUP - 5)
    write(home / "Work/.mise.toml", b'[tools]\nnode = "latest"\n', TRIAL_SETUP - 5)
    marker(home)
    write(home / ".local/state/omarchy/current/theme.name", b"gruvbox\n", TRIAL_EDIT)
    link(home / ".local/state/omarchy/current/background",
         f"/home/{user}/.local/state/omarchy/current/theme/backgrounds/1-gruvbox.jpg")
    # The user's changes.
    write(home / ".config/hypr/bindings.lua",
          TRIAL_BINDINGS + b'bind("SUPER", "N", "notes")\n', TRIAL_EDIT)
    write(home / ".config/hypr/input.lua",
          b'input({ kb_layout = "us,de" })\n\n' + PINCH_BLOCK, TRIAL_EDIT)
    write(home / ".config/hypr/monitors.lua", b'monitor("", "1920x1080", "auto", 2)\n', TRIAL_EDIT)
    write(home / ".config/git/config",
          GIT_CONFIG + b"[user]\n\tname = Ada\n\temail = ada@example.com\n", TRIAL_EDIT)
    write(home / ".config/nvim/lua/plugins/mine.lua", b"return {}\n", TRIAL_EDIT)
    write(home / ".config/mise/config.toml", b'[tools]\nnode = "22"\n', TRIAL_EDIT)
    write(home / ".config/gtk-3.0/bookmarks",
          f"file:///home/{user}/Downloads Downloads\nfile:///mnt/host Windows\n"
          f"file:///home/{user}/Projects/site Site\n".encode(), TRIAL_EDIT)
    write(home / ".bashrc", b"source /usr/share/omarchy/default/bash/rc\nalias gs='git status'\n"
          b"export EDITOR=nvim\n", TRIAL_EDIT)
    write(home / ".bash_history", b"ls\ngit status\n", TRIAL_EDIT)
    write(home / ".local/share/applications/HEY.desktop", b"[Desktop Entry]\nName=HEY\n",
          TRIAL_EDIT)
    write(home / ".local/share/applications/try-omarchy-windows-0123456789abcdef0123456789abcdef"
          ".desktop", b"[Desktop Entry]\nName=Windows: Notepad\n", TRIAL_EDIT)
    write(home / ".local/state/omarchy/toggles/hypr/window-no-gaps.lua", b"gaps(0)\n", TRIAL_EDIT)
    write(home / ".local/state/wireplumber/default-routes", b"qemu\n", TRIAL_EDIT)
    write(home / ".config/omarchy/themes/sunset/colors.toml", b'accent = "#ff8800"\n', TRIAL_EDIT)
    write(home / ".cache/thumbnails/a.png", b"x" * 10, TRIAL_EDIT)
    # Files and projects.
    write(home / "Documents/resume.md", b"# Ada\n", TRIAL_EDIT)
    write(home / "Pictures/Screenshots/shot.png", b"\x89PNG\r\n" + b"\0" * 100, TRIAL_EDIT)
    write(home / "notes.txt", b"loose notes\n", TRIAL_EDIT)
    link(home / "Windows", "/mnt/host")
    # Keys, a browser profile and another app.
    write(home / ".ssh/id_ed25519", b"-----BEGIN OPENSSH PRIVATE KEY-----\nkey\n", TRIAL_EDIT, 0o600)
    write(home / ".ssh/id_ed25519.pub", b"ssh-ed25519 AAAA ada\n", TRIAL_EDIT)
    write(home / ".ssh/authorized_keys", b"ssh-ed25519 AAAA launcher\n", TRIAL_EDIT, 0o600)
    write(home / ".ssh/known_hosts", b"github.com ssh-ed25519 AAAAgh\n", TRIAL_EDIT)
    os.chmod(home / ".ssh", 0o700)
    write(home / ".config/gh/hosts.yml", b"github.com:\n  oauth_token: secret\n", TRIAL_EDIT, 0o600)
    write(home / ".local/share/keyrings/Default_keyring.keyring", KEYRING_CHROMIUM, TRIAL_EDIT,
          0o600)
    write(home / ".config/chromium/Default/Bookmarks", b'{"roots": {}}\n', TRIAL_EDIT)
    write(home / ".config/chromium/Default/Cache/data_0", b"cache", TRIAL_EDIT)
    link(home / ".config/chromium/SingletonLock", "try-omarchy-1234")
    write(home / ".config/Slack/storage/state.json", b"{}\n", TRIAL_EDIT)
    write(home / ".claude/settings.json", b"{}\n", TRIAL_EDIT)
    write(home / ".claude/.credentials.json", b'{"token": "x"}\n', TRIAL_EDIT, 0o600)
    return root, home


def make_home(base):
    """A freshly installed Omarchy home with newer defaults. Returns (home, skel)."""
    skel = Path(base) / "native-skel"
    write(skel / ".config/hypr/bindings.lua", NEW_BINDINGS, SKEL_TIME + 3_000)
    write(skel / ".config/hypr/input.lua", INPUT, SKEL_TIME + 3_000)
    write(skel / ".config/hypr/monitors.lua", MONITORS, SKEL_TIME + 3_000)
    write(skel / ".config/git/config", GIT_CONFIG, SKEL_TIME + 3_000)
    write(skel / ".bashrc", b"source /usr/share/omarchy/default/bash/rc\n", SKEL_TIME + 3_000)
    write(skel / ".config/chromium/Default/Preferences", b"{}\n", SKEL_TIME + 3_000)
    home = Path(base) / "home"
    seed(skel, home)
    write(home / ".config/git/config", GIT_CONFIG + b"[user]\n\tname = New Name\n", HOME_SETUP - 1)
    write(home / ".local/share/keyrings/Default_keyring.keyring", KEYRING_EMPTY, HOME_SETUP - 1,
          0o600)
    write(home / ".config/gtk-3.0/bookmarks",
          f"file://{home}/Downloads Downloads\n".encode(), HOME_SETUP - 1)
    (home / "Documents").mkdir()
    write(home / "Work/.mise.toml", b'[tools]\nnode = "latest"\n', HOME_SETUP - 1)
    marker(home)
    return home, skel


class FakeRunner:
    """Records commands and answers them from a table of canned outputs."""

    def __init__(self, outputs=None, programs=(), running=(), failing=()):
        self.outputs = dict(outputs or {})
        self.programs = set(programs)
        self.running = set(running)
        self.failing = set(failing)
        self.calls = []

    def which(self, name):
        return f"/usr/bin/{name}" if name in self.programs else None

    def run(self, argv, *, sudo=False, check=True, capture=True, input=None, timeout=None,
            cwd=None, env=None):
        import subprocess
        from omarchy_import.system import CommandError
        argv = [str(part) for part in argv]
        self.calls.append((tuple(argv), sudo))
        key = " ".join(argv)
        for prefix in self.failing:
            if key.startswith(prefix):
                if check:
                    raise CommandError(argv, 1, "failed on purpose")
                return subprocess.CompletedProcess(argv, 1, "", "failed on purpose")
        for prefix, output in self.outputs.items():
            if key.startswith(prefix):
                return subprocess.CompletedProcess(argv, 0, output, "")
        if argv[0] not in self.programs and argv[0] not in (
                "mount", "umount", "losetup", "dmsetup", "blockdev"):
            raise FileNotFoundError(argv[0])
        return subprocess.CompletedProcess(argv, 0, "", "")

    def sudo_ready(self):
        return True

    def running_programs(self):
        return set(self.running)

    def commands(self):
        return [" ".join(argv) for argv, _ in self.calls]
