"""Running commands, with sudo only where it is needed."""

import os
import shutil
import subprocess


class CommandError(Exception):
    def __init__(self, argv, returncode, stderr):
        self.argv = list(argv)
        self.returncode = returncode
        self.stderr = (stderr or "").strip()
        detail = self.stderr.splitlines()[-1] if self.stderr else f"exit status {returncode}"
        super().__init__(f"{os.path.basename(self.argv[0])} failed: {detail}")


class Runner:
    """Runs commands. Tests replace it with a fake that records calls."""

    def which(self, name):
        return shutil.which(name)

    def run(self, argv, *, sudo=False, check=True, capture=True, input=None, timeout=None,
            cwd=None, env=None):
        argv = [str(part) for part in argv]
        if sudo and os.geteuid() != 0:
            argv = ["sudo", "--", *argv]
        result = subprocess.run(argv, text=True, input=input, timeout=timeout, cwd=cwd, env=env,
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
        return subprocess.run(["sudo", "-v"]).returncode == 0


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
