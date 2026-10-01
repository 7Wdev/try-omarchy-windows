"""Running commands, with sudo only where it is needed.

Commands come only from the system's own folders, never from the caller's
PATH. An import can bring scripts into ~/.local/bin, which usually comes first
in PATH, and a script that happens to share a tool's name must not run in its
place, least of all under sudo. Commands also get a PATH of just those
folders, so scripts such as omarchy-theme-set cannot pick up an imported
helper either. The importer never writes to any of them.
"""

import errno
import os
import shutil
import subprocess

SYSTEM_DIRECTORIES = ("/usr/share/omarchy/bin", "/usr/local/sbin", "/usr/local/bin", "/usr/bin",
                      "/usr/sbin", "/bin", "/sbin")


def find_tool(name, directories=SYSTEM_DIRECTORIES):
    """The full path of a command in the system folders, or None."""
    if not name or "/" in name:
        return None
    return shutil.which(name, path=os.pathsep.join(directories))


def system_environment(base=None, directories=SYSTEM_DIRECTORIES):
    """base (default: this process's environment) with PATH set to the system folders."""
    environment = dict(os.environ if base is None else base)
    environment["PATH"] = os.pathsep.join(directories)
    return environment


class CommandError(Exception):
    def __init__(self, argv, returncode, stderr):
        self.argv = list(argv)
        self.returncode = returncode
        self.stderr = (stderr or "").strip()
        detail = self.stderr.splitlines()[-1] if self.stderr else f"exit status {returncode}"
        super().__init__(f"{os.path.basename(self.argv[0])} failed: {detail}")


class Runner:
    """Runs commands. Tests replace it with a fake that records calls."""

    def __init__(self, directories=SYSTEM_DIRECTORIES):
        self.directories = tuple(directories)

    def which(self, name):
        return find_tool(name, self.directories)

    def _tool(self, name):
        path = self.which(name)
        if path is None:
            raise FileNotFoundError(errno.ENOENT, "not found in the system folders", name)
        return path

    def run(self, argv, *, sudo=False, check=True, capture=True, input=None, timeout=None,
            cwd=None, env=None):
        argv = [str(part) for part in argv]
        argv[0] = self._tool(argv[0])
        if sudo and os.geteuid() != 0:
            argv = [self._tool("sudo"), "--", *argv]
        result = subprocess.run(argv, text=True, input=input, timeout=timeout, cwd=cwd,
                                env=system_environment(env, self.directories),
                                stdout=subprocess.PIPE if capture else None,
                                stderr=subprocess.PIPE if capture else None)
        if check and result.returncode != 0:
            raise CommandError(argv, result.returncode, result.stderr if capture else "")
        return result

    def running_programs(self):
        return running_programs()

    def sudo_ready(self):
        """Ask for the sudo password once, in the terminal, before real work starts."""
        if os.geteuid() == 0:
            return True
        sudo = self.which("sudo")
        if sudo is None:
            return False
        return subprocess.run([sudo, "-v"],
                              env=system_environment(directories=self.directories)).returncode == 0

    def forget_sudo(self):
        """Drop the cached sudo password before running what the import brought
        (mise tools, Hyprland's config, theme hooks), so none of it can act as
        root without asking."""
        sudo = self.which("sudo")
        if sudo is None or os.geteuid() == 0:
            return
        subprocess.run([sudo, "-k"], env=system_environment(directories=self.directories),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def running_programs():
    """Names from /proc/<pid>/comm of everything running as this user."""
    names = set()
    uid = os.getuid()
    try:
        entries = os.listdir("/proc")
    except OSError:
        return names
    for entry in entries:
        if not entry.isdigit():
            continue
        try:
            if os.stat(f"/proc/{entry}").st_uid != uid:
                continue
            with open(f"/proc/{entry}/comm", encoding="utf-8", errors="replace") as source:
                names.add(source.read().strip())
        except OSError:
            continue
    return names
