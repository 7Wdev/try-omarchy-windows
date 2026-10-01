"""Mount Windows drives and the trial disk read-only, and undo it afterwards.

- Windows drives are mounted with ntfs3, read-only, readable only by the
  person running the importer. A drive the desktop already mounted is used
  where it is.
- The trial disk is attached to a read-only loop device. If the trial was not
  shut down cleanly its ext4 journal still needs replaying; that happens in a
  throwaway device-mapper snapshot whose changes go to a temporary file, so
  the disk on the Windows drive is never written.
- If the trial account's user ID differs from this computer's, the trial is
  also exposed through an ID-mapped mount, so its private files (mode 0600)
  are readable without running the copy as root.

Every step is recorded as it happens and undone in reverse order by close().
"""

import os
from pathlib import Path
import stat
import tempfile
import time

from .system import CommandError

SECTOR = 512


RUNTIME_NAME = "try-omarchy-import"
SNAPSHOT_PREFIX = "try-omarchy-import-"
COW_PREFIX = "try-omarchy-import-"


def _private_directory(path):
    try:
        metadata = os.lstat(path)
    except OSError:
        return False
    return (stat.S_ISDIR(metadata.st_mode) and metadata.st_uid == os.getuid()
            and not metadata.st_mode & 0o077)


def runtime_directory():
    """A fresh private folder for this run's mount points."""
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime and _private_directory(runtime):
        base = Path(runtime) / RUNTIME_NAME
        try:
            base.mkdir(mode=0o700)
        except FileExistsError:
            pass
        if _private_directory(base):
            return Path(tempfile.mkdtemp(dir=base))
    # No usable runtime directory (for example under sudo -u): a random,
    # private folder in /tmp that nobody else can have prepared.
    return Path(tempfile.mkdtemp(prefix=f"{RUNTIME_NAME}-"))


class Session:
    def __init__(self, runner, workdir=None):
        self.runner = runner
        self.workdir = Path(workdir) if workdir else runtime_directory()
        self._undo = []
        self._counter = 0
        self.errors = []

    def __enter__(self):
        return self

    def __exit__(self, *arguments):
        self.finish()

    def finish(self):
        """Undo everything now; later calls do nothing more."""
        self.errors.extend(self.close())

    def _mountpoint(self, name):
        self._counter += 1
        path = self.workdir / f"{name}-{self._counter}"
        path.mkdir(mode=0o700)
        self._undo.append(("rmdir", path))
        return path

    def mount_windows(self, device):
        """Mount an NTFS partition read-only and return where."""
        target = self._mountpoint("windows")
        options = (f"ro,nodev,nosuid,noexec,uid={os.getuid()},gid={os.getgid()},"
                   "fmask=0177,dmask=0077")
        self.runner.run(["mount", "-t", "ntfs3", "-o", options, device, target], sudo=True)
        self._undo.append(("umount", target))
        return target

    def attach_disk(self, disk, needs_recovery=False):
        """Mount the trial's ext4 disk read-only and return the mount path."""
        result = self.runner.run(["losetup", "--find", "--show", "--read-only", disk], sudo=True)
        loop = result.stdout.strip()
        self._undo.append(("losetup", loop))
        device = loop
        if needs_recovery:
            device = self._snapshot(loop)
        target = self._mountpoint("trial")
        self.runner.run(["mount", "-t", "ext4", "-o", "ro,nodev,nosuid,noexec", device, target],
                        sudo=True)
        self._undo.append(("umount", target))
        return target

    def _snapshot(self, origin):
        sectors = int(self.runner.run(["blockdev", "--getsz", origin], sudo=True).stdout.strip())
        cow_fd, cow_path = tempfile.mkstemp(prefix=COW_PREFIX, suffix=".cow", dir="/var/tmp")
        os.close(cow_fd)
        self._undo.append(("remove", Path(cow_path)))
        # Sparse: only what the journal replay writes takes space.
        os.truncate(cow_path, 4 * 1024 * 1024 * 1024)
        cow = self.runner.run(["losetup", "--find", "--show", cow_path], sudo=True).stdout.strip()
        self._undo.append(("losetup", cow))
        name = f"{SNAPSHOT_PREFIX}{os.getpid()}-{self._counter}"
        table = f"0 {sectors} snapshot {origin} {cow} N 8"
        self.runner.run(["dmsetup", "create", name, "--table", table], sudo=True)
        self._undo.append(("dmsetup", name))
        return f"/dev/mapper/{name}"

    def map_owner(self, root, trial_uid, trial_gid):
        """Expose root with the trial's uid/gid mapped to the caller's."""
        uid, gid = os.getuid(), os.getgid()
        if (trial_uid, trial_gid) == (uid, gid):
            return Path(root)
        target = self._mountpoint("mapped")
        mapping = f"u:{trial_uid}:{uid}:1 g:{trial_gid}:{gid}:1"
        self.runner.run(["mount", "--bind", "-o",
                         f"ro,nodev,nosuid,noexec,X-mount.idmap={mapping}", root, target],
                        sudo=True)
        self._undo.append(("umount", target))
        return target

    def close(self):
        errors = []
        while self._undo:
            step, value = self._undo.pop()
            try:
                if step == "umount":
                    self._retry(["umount", value])
                elif step == "losetup":
                    self._retry(["losetup", "--detach", value])
                elif step == "dmsetup":
                    self._retry(["dmsetup", "remove", value])
                elif step == "rmdir":
                    os.rmdir(value)
                elif step == "remove":
                    os.unlink(value)
            except (CommandError, OSError) as error:
                errors.append(f"{step} {value}: {error}")
        try:
            self.workdir.rmdir()
        except OSError:
            pass
        return errors

    def _retry(self, argv):
        for attempt in range(5):
            try:
                self.runner.run(argv, sudo=True)
                return
            except CommandError:
                if attempt == 4:
                    raise
                time.sleep(0.5)


def is_our_loop_backing(path):
    """True for loop devices this importer creates: a trial disk inside a Try
    Omarchy data folder, or one of our snapshot scratch files."""
    path = path.strip()
    name = os.path.basename(path)
    if name.startswith(COW_PREFIX) and name.endswith(".cow") and os.path.dirname(path) == "/var/tmp":
        return True
    parts = Path(path).parts
    return name == "disk.raw" and len(parts) >= 3 and parts[-2].lower() == "vm" \
        and parts[-3].lower().startswith("tryomarchy")


def remove_scratch_files():
    """Delete snapshot scratch files of ours that no loop device uses anymore."""
    removed = 0
    try:
        names = os.listdir("/var/tmp")
    except OSError:
        return 0
    for name in names:
        if not (name.startswith(COW_PREFIX) and name.endswith(".cow")):
            continue
        path = os.path.join("/var/tmp", name)
        try:
            metadata = os.lstat(path)
            if metadata.st_uid != os.getuid() or not stat.S_ISREG(metadata.st_mode):
                continue
            if _loop_uses(path):
                continue
            os.unlink(path)
            removed += 1
        except OSError:
            continue
    return removed


def _loop_uses(path):
    try:
        for name in os.listdir("/sys/block"):
            if not name.startswith("loop"):
                continue
            try:
                with open(f"/sys/block/{name}/loop/backing_file", encoding="utf-8") as source:
                    if source.read().strip() == path:
                        return True
            except OSError:
                continue
    except OSError:
        return True
    return False


def leftover_mounts():
    """Mounts an interrupted run by this user left behind."""
    found = []
    try:
        with open("/proc/self/mountinfo", encoding="utf-8") as source:
            for line in source:
                fields = line.split()
                mountpoint = fields[4].replace("\\040", " ")
                parent = os.path.dirname(mountpoint)
                if (f"/{RUNTIME_NAME}/" in mountpoint or f"/{RUNTIME_NAME}-" in mountpoint) \
                        and _private_directory(parent):
                    found.append(mountpoint)
    except OSError:
        return []
    return sorted(found, key=len, reverse=True)
