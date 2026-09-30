"""Find Try Omarchy installs on this computer's Windows drives.

Try Omarchy for Windows keeps its data in %LOCALAPPDATA%\\TryOmarchy, or in a
folder the user chose, which the default folder records in
data-location.json. The trial's disk is vm\\disk.raw: a plain ext4 filesystem
with no partition table, so Linux can mount it directly. Portable installs
use vm\\disk.qcow2 instead, which a stock Omarchy cannot read.
"""

from dataclasses import dataclass, field
import json
import os
from pathlib import Path, PureWindowsPath
import struct

EXT4_MAGIC = 0xEF53
INCOMPAT_RECOVER = 0x4
INCOMPAT_64BIT = 0x80
HIBERNATION_SIGNATURES = (b"hibr", b"HIBR")
MAX_POINTER_BYTES = 16 * 1024


@dataclass
class Superblock:
    label: str
    uuid: str
    clean: bool
    needs_recovery: bool
    last_write: int
    size: int = 0


@dataclass
class Volume:
    device: str
    fstype: str
    label: str
    size: int
    mountpoint: str = None


@dataclass
class Install:
    data_dir: Path
    disk: Path
    format: str
    windows_user: str = None
    volume: Volume = None
    size: int = 0
    allocated: int = 0
    last_used: float = 0
    share: str = None
    superblock: Superblock = None
    problems: list = field(default_factory=list)

    def windows_path(self):
        """A readable Windows-style location for messages."""
        return str(self.data_dir)


def read_superblock(disk):
    """Read an ext4 superblock from the start of a raw filesystem image."""
    with open(disk, "rb") as source:
        source.seek(1024)
        block = source.read(1024)
    if len(block) < 1024:
        return None
    (magic,) = struct.unpack_from("<H", block, 0x38)
    if magic != EXT4_MAGIC:
        return None
    (state,) = struct.unpack_from("<H", block, 0x3A)
    (incompat,) = struct.unpack_from("<I", block, 0x60)
    (write_time,) = struct.unpack_from("<I", block, 0x30)
    uuid = block[0x68:0x78].hex()
    uuid = f"{uuid[:8]}-{uuid[8:12]}-{uuid[12:16]}-{uuid[16:20]}-{uuid[20:]}"
    label = block[0x78:0x88].split(b"\0", 1)[0].decode("utf-8", "replace")
    (blocks_low,) = struct.unpack_from("<I", block, 0x04)
    (log_block_size,) = struct.unpack_from("<I", block, 0x18)
    (blocks_high,) = struct.unpack_from("<I", block, 0x150) if incompat & INCOMPAT_64BIT else (0,)
    size = ((blocks_high << 32) | blocks_low) * (1024 << min(log_block_size, 6))
    return Superblock(label, uuid, bool(state & 1), bool(incompat & INCOMPAT_RECOVER), write_time,
                      size)


def hibernated(volume_root):
    """True when Windows hibernated or used Fast Startup on this drive."""
    for name in ("hiberfil.sys", "Hiberfil.sys", "HIBERFIL.SYS"):
        path = Path(volume_root) / name
        try:
            with open(path, "rb") as source:
                return source.read(4) in HIBERNATION_SIGNATURES
        except (FileNotFoundError, NotADirectoryError):
            continue
        except OSError:
            return False
    return False


def _read_json(path):
    try:
        with open(path, "rb") as source:
            data = source.read(MAX_POINTER_BYTES + 1)
    except OSError:
        return None
    if len(data) > MAX_POINTER_BYTES:
        return None
    try:
        return json.loads(data.decode("utf-8-sig"))
    except (ValueError, UnicodeDecodeError):
        return None


def windows_relative(path):
    """Turn C:\\Some\\Folder into Some/Folder; None for UNC or relative paths."""
    windows = PureWindowsPath(path)
    if not windows.drive or not windows.drive.endswith(":") or not windows.root:
        return None
    parts = windows.parts[1:]
    if not parts or any(part in ("..", ".") for part in parts):
        return None
    return "/".join(parts)


def _find_ci(base, relative):
    """Resolve a relative path case-insensitively, as NTFS paths from Windows may differ in case."""
    current = Path(base)
    for part in relative.split("/"):
        exact = current / part
        if exact.exists():
            current = exact
            continue
        try:
            with os.scandir(current) as entries:
                match = next((entry for entry in entries if entry.name.lower() == part.lower()),
                             None)
        except OSError:
            return None
        if match is None:
            return None
        current = Path(match.path)
    return current


def describe(data_dir, volume=None, windows_user=None):
    """Return an Install for a Try Omarchy data folder, or None if it has no disk."""
    data_dir = Path(data_dir)
    for disk_format in ("raw", "qcow2"):
        disk = _find_ci(data_dir, f"vm/disk.{disk_format}")
        if disk is None or not disk.is_file() or disk.is_symlink():
            continue
        metadata = disk.stat()
        install = Install(data_dir, disk, disk_format, windows_user, volume, metadata.st_size,
                          metadata.st_blocks * 512, metadata.st_mtime)
        settings = _read_json(_find_ci(data_dir, "settings.json") or data_dir / "settings.json")
        if isinstance(settings, dict) and isinstance(settings.get("share"), str) and \
                settings.get("share") and not settings.get("shareDisabled"):
            install.share = settings["share"]
        if disk_format == "qcow2":
            install.problems.append(
                "this is a portable install, whose disk Omarchy cannot read directly. Start it in "
                "Try Omarchy on Windows, run try-omarchy-export in a terminal there, and follow "
                "the steps it prints.")
        else:
            try:
                install.superblock = read_superblock(disk)
            except OSError as error:
                install.problems.append(f"the trial disk could not be read: {error.strerror}")
            else:
                if install.superblock is None:
                    install.problems.append("the trial disk is not in a format this importer knows")
                elif install.superblock.size > metadata.st_size:
                    install.problems.append(
                        "the trial disk is incomplete (it is smaller than the filesystem on it), "
                        "so it may have been cut short while being copied or moved")
        return install
    return None


def find_installs(volume_roots):
    """Find Try Omarchy data folders on the mounted Windows drives.

    volume_roots maps a mount path to its Volume.
    """
    found = []
    seen = set()
    pointers = []
    for root, volume in volume_roots.items():
        users = _find_ci(root, "Users")
        if users is None or not users.is_dir():
            continue
        for user in sorted(os.scandir(users), key=lambda item: item.name):
            if not user.is_dir(follow_symlinks=False):
                continue
            default = _find_ci(user.path, "AppData/Local/TryOmarchy")
            if default is None or not default.is_dir():
                continue
            pointer = _read_json(_find_ci(default, "data-location.json") or default / "missing")
            if isinstance(pointer, dict) and isinstance(pointer.get("path"), str):
                pointers.append((pointer["path"], user.name))
            install = describe(default, volume, user.name)
            if install is not None and install.disk.resolve() not in seen:
                seen.add(install.disk.resolve())
                found.append(install)
    for path, windows_user in pointers:
        relative = windows_relative(path)
        if relative is None:
            continue
        for root, volume in volume_roots.items():
            candidate = _find_ci(root, relative)
            if candidate is None or not candidate.is_dir():
                continue
            install = describe(candidate, volume, windows_user)
            if install is not None and install.disk.resolve() not in seen:
                seen.add(install.disk.resolve())
                found.append(install)
    return sorted(found, key=lambda install: install.last_used, reverse=True)


def list_volumes(runner):
    """Windows (NTFS) partitions on this computer, from lsblk. BitLocker
    partitions are listed too, with fstype "bitlocker", so they can be
    explained instead of silently skipped."""
    result = runner.run(["lsblk", "--json", "--bytes", "--paths", "--output",
                         "PATH,FSTYPE,LABEL,SIZE,MOUNTPOINTS,TYPE"])
    try:
        tree = json.loads(result.stdout)
    except ValueError:
        return []
    volumes = []

    def visit(node):
        fstype = (node.get("fstype") or "").lower()
        if fstype in ("ntfs", "ntfs3", "bitlocker") and node.get("path"):
            mountpoints = [point for point in (node.get("mountpoints") or []) if point]
            volumes.append(Volume(node["path"], fstype, node.get("label") or "",
                                  int(node.get("size") or 0),
                                  mountpoints[0] if mountpoints else None))
        for child in node.get("children") or []:
            visit(child)

    for node in tree.get("blockdevices") or []:
        visit(node)
    return volumes
