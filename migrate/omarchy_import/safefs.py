"""Filesystem access that never follows symlinks.

Every destination path is reached one component at a time from an open
descriptor of the home directory, with O_NOFOLLOW, so a symlinked parent can
never redirect a write outside the home directory. New files are written to a
private temporary name first and then linked into place, which fails instead
of overwriting anything that appeared in the meantime. Replacements go through
rename only after the original is backed up and has not changed since the plan
was made.

The descriptor walk, link-then-unlink publication and "never replace what
changed after planning" checks follow the restore experiment in
omacom/omarchy-mac-installer (explore/try-omarchy-migration, 4b313ad, MIT).
"""

import contextlib
from dataclasses import dataclass
import errno
import hashlib
import os
import stat
import uuid

DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
READ_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC
CHUNK = 1024 * 1024
TEMP_PREFIX = ".try-omarchy-import-"
TEMP_SUFFIX = ".tmp"


class UnsafePath(Exception):
    """A destination component is a symlink, not a directory, or not ours."""

    def __init__(self, path, reason):
        super().__init__(f"{path}: {reason}")
        self.path = path
        self.reason = reason


class Changed(Exception):
    """The destination changed between planning and applying."""


@dataclass(frozen=True)
class Fingerprint:
    kind: str
    device: int
    inode: int
    size: int
    mtime_ns: int
    ctime_ns: int
    mode: int

    @classmethod
    def of(cls, metadata):
        if stat.S_ISREG(metadata.st_mode):
            kind = "file"
        elif stat.S_ISDIR(metadata.st_mode):
            kind = "directory"
        elif stat.S_ISLNK(metadata.st_mode):
            kind = "symlink"
        else:
            kind = "special"
        return cls(kind, metadata.st_dev, metadata.st_ino, metadata.st_size,
                   metadata.st_mtime_ns, metadata.st_ctime_ns, stat.S_IMODE(metadata.st_mode))


def kind_of(metadata):
    return Fingerprint.of(metadata).kind


def split(relative):
    parts = relative.split("/")
    if not relative or any(part in ("", ".", "..") for part in parts) or "\0" in relative:
        raise UnsafePath(relative, "invalid relative path")
    return parts


def is_temporary_name(name):
    return name.startswith(TEMP_PREFIX) and name.endswith(TEMP_SUFFIX)


def copy_fd(source_fd, target_fd, transform=None):
    """Copy everything from source_fd to target_fd; return (sha256, size)."""
    digest = hashlib.sha256()
    size = 0
    if transform is not None:
        data = b""
        while piece := os.read(source_fd, CHUNK):
            data += piece
        data = transform(data)
        _write_all(target_fd, data)
        digest.update(data)
        return digest.hexdigest(), len(data)
    while piece := os.read(source_fd, CHUNK):
        _write_all(target_fd, piece)
        digest.update(piece)
        size += len(piece)
    return digest.hexdigest(), size


def _write_all(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        view = view[written:]


def hash_fd(fd):
    digest = hashlib.sha256()
    size = 0
    while piece := os.read(fd, CHUNK):
        digest.update(piece)
        size += len(piece)
    return digest.hexdigest(), size


def hash_bytes(data):
    return hashlib.sha256(data).hexdigest()


def source_open(path):
    """Open a source file without following a final symlink."""
    fd = os.open(path, READ_FLAGS)
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        raise UnsafePath(str(path), "not a regular file")
    return fd


def source_read(path, limit=None):
    fd = source_open(path)
    try:
        chunks = []
        total = 0
        while piece := os.read(fd, CHUNK):
            chunks.append(piece)
            total += len(piece)
            if limit is not None and total > limit:
                return None
        return b"".join(chunks)
    finally:
        os.close(fd)


def source_hash(path):
    fd = source_open(path)
    try:
        return hash_fd(fd)
    finally:
        os.close(fd)


class Source:
    """Read-only access below a trial folder (its /etc/skel) one component at
    a time with O_NOFOLLOW, so a link inside it cannot point a read anywhere
    else. Anything reached through a link counts as missing."""

    def __init__(self, base):
        self.base = None if base is None else os.fspath(base)

    @contextlib.contextmanager
    def parent(self, relative):
        """Yield (folder descriptor or None, final name) for relative."""
        parts = split(relative)
        if self.base is None:
            yield None, parts[-1]
            return
        opened = []
        try:
            try:
                current = os.open(self.base, DIRECTORY_FLAGS)
                opened.append(current)
                for name in parts[:-1]:
                    current = os.open(name, DIRECTORY_FLAGS, dir_fd=current)
                    opened.append(current)
            except (FileNotFoundError, NotADirectoryError, PermissionError):
                yield None, parts[-1]
                return
            except OSError as error:
                if error.errno != errno.ELOOP:
                    raise
                yield None, parts[-1]
                return
            yield current, parts[-1]
        finally:
            for fd in opened:
                os.close(fd)

    def lstat(self, relative):
        with self.parent(relative) as (parent, name):
            if parent is None:
                return None
            try:
                return os.stat(name, dir_fd=parent, follow_symlinks=False)
            except (FileNotFoundError, NotADirectoryError):
                return None

    def readlink(self, relative):
        with self.parent(relative) as (parent, name):
            if parent is None:
                raise FileNotFoundError(relative)
            return os.readlink(name, dir_fd=parent)

    def read(self, relative, limit=None):
        """The file's bytes; None if it is missing, not a regular file or over limit."""
        with self.parent(relative) as (parent, name):
            if parent is None:
                return None
            try:
                fd = os.open(name, READ_FLAGS, dir_fd=parent)
            except OSError:
                return None
        try:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                return None
            chunks = []
            total = 0
            while piece := os.read(fd, CHUNK):
                chunks.append(piece)
                total += len(piece)
                if limit is not None and total > limit:
                    return None
            return b"".join(chunks)
        finally:
            os.close(fd)


class Destination:
    """The user's home directory, reached only through no-follow descriptors."""

    def __init__(self, home):
        self.home = os.fspath(home)
        self.fd = os.open(self.home, DIRECTORY_FLAGS)
        metadata = os.fstat(self.fd)
        if metadata.st_uid != os.geteuid():
            os.close(self.fd)
            raise UnsafePath(self.home, "the home directory belongs to another user")
        # Modes from the trial are applied through this umask, so a file or
        # folder the trial left writable for everyone is not writable for
        # everyone here.
        self.umask = os.umask(0o022)
        os.umask(self.umask)

    def mode(self, mode):
        """The permission bits to give something the trial had with mode."""
        return mode & 0o777 & ~self.umask

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *arguments):
        self.close()

    @contextlib.contextmanager
    def directory(self, relative, *, create=False, mode=0o755):
        """Yield a descriptor for the folder at relative, or None if it is missing.

        Every component is opened with O_NOFOLLOW from the home descriptor.
        Raises UnsafePath if a component is a symlink, not a folder, or owned
        by someone else.
        """
        if relative in ("", "."):
            yield self.fd
            return
        opened = []
        try:
            current = self.fd
            parts = split(relative)
            for index, name in enumerate(parts):
                try:
                    fd = os.open(name, DIRECTORY_FLAGS, dir_fd=current)
                except FileNotFoundError:
                    if not create:
                        yield None
                        return
                    try:
                        os.mkdir(name, self.mode(mode) | 0o700, dir_fd=current)
                    except FileExistsError:
                        pass
                    fd = os.open(name, DIRECTORY_FLAGS, dir_fd=current)
                except OSError as error:
                    if error.errno in (errno.ELOOP, errno.ENOTDIR):
                        where = "/".join(parts[:index + 1])
                        raise UnsafePath(where, "is a link or not a folder on this computer") from None
                    raise
                opened.append(fd)
                if os.fstat(fd).st_uid != os.geteuid():
                    raise UnsafePath("/".join(parts[:index + 1]), "belongs to another user")
                current = fd
            yield current
        finally:
            for fd in opened:
                os.close(fd)

    @contextlib.contextmanager
    def parent(self, relative, *, create=False, mode=0o755):
        """Yield (parent descriptor or None, final name) for relative."""
        parts = split(relative)
        with self.directory("/".join(parts[:-1]), create=create, mode=mode) as fd:
            yield fd, parts[-1]

    def lstat(self, relative):
        """Return lstat of relative, None if missing. Raises UnsafePath for unsafe parents."""
        with self.parent(relative) as (parent, name):
            if parent is None:
                return None
            try:
                return os.stat(name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                return None

    def fingerprint(self, relative):
        metadata = self.lstat(relative)
        return None if metadata is None else Fingerprint.of(metadata)

    def readlink(self, relative):
        with self.parent(relative) as (parent, name):
            if parent is None:
                raise FileNotFoundError(relative)
            return os.readlink(name, dir_fd=parent)

    @contextlib.contextmanager
    def open_file(self, relative):
        with self.parent(relative) as (parent, name):
            if parent is None:
                raise FileNotFoundError(relative)
            fd = os.open(name, READ_FLAGS, dir_fd=parent)
        try:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise UnsafePath(relative, "not a regular file")
            yield fd
        finally:
            os.close(fd)

    def read(self, relative, limit=None):
        with self.open_file(relative) as fd:
            chunks = []
            total = 0
            while piece := os.read(fd, CHUNK):
                chunks.append(piece)
                total += len(piece)
                if limit is not None and total > limit:
                    return None
            return b"".join(chunks)

    def hash(self, relative):
        with self.open_file(relative) as fd:
            return hash_fd(fd)

    def _write_temporary(self, parent, content, mode, mtime_ns, durable):
        """content is bytes or a (source_fd, transform) pair; returns (name, sha, size)."""
        name = f"{TEMP_PREFIX}{uuid.uuid4().hex}{TEMP_SUFFIX}"
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                     0o600, dir_fd=parent)
        try:
            if isinstance(content, (bytes, bytearray)):
                _write_all(fd, content)
                sha, size = hash_bytes(content), len(content)
            else:
                source_fd, transform = content
                sha, size = copy_fd(source_fd, fd, transform)
            os.fchmod(fd, self.mode(mode))
            if mtime_ns is not None:
                os.utime(fd, ns=(mtime_ns, mtime_ns))
            if durable:
                os.fsync(fd)
        except BaseException:
            os.close(fd)
            with contextlib.suppress(OSError):
                os.unlink(name, dir_fd=parent)
            raise
        os.close(fd)
        return name, sha, size

    def create_file(self, relative, content, *, mode=0o644, mtime_ns=None, durable=False,
                    directory_mode=0o755):
        """Publish a new file; raises FileExistsError if anything is already there."""
        with self.parent(relative, create=True, mode=directory_mode) as (parent, name):
            temporary, sha, size = self._write_temporary(parent, content, mode, mtime_ns, durable)
            try:
                os.link(temporary, name, src_dir_fd=parent, dst_dir_fd=parent, follow_symlinks=False)
            finally:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(temporary, dir_fd=parent)
            if durable:
                os.fsync(parent)
        return sha, size

    def replace_file(self, relative, content, expected, *, mode=0o644, mtime_ns=None, durable=True):
        """Replace a file that must still match the planned fingerprint."""
        with self.parent(relative) as (parent, name):
            if parent is None:
                raise Changed(relative)
            temporary, sha, size = self._write_temporary(parent, content, mode, mtime_ns, durable)
            try:
                current = os.stat(name, dir_fd=parent, follow_symlinks=False)
                if Fingerprint.of(current) != expected:
                    raise Changed(relative)
                # rename is not a compare-and-swap; the window between the
                # check and the rename is as small as we can make it.
                os.rename(temporary, name, src_dir_fd=parent, dst_dir_fd=parent)
            except BaseException:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(temporary, dir_fd=parent)
                raise
            if durable:
                os.fsync(parent)
        return sha, size

    def create_symlink(self, relative, target, *, directory_mode=0o755):
        with self.parent(relative, create=True, mode=directory_mode) as (parent, name):
            os.symlink(target, name, dir_fd=parent)

    def replace_symlink(self, relative, target, expected):
        with self.parent(relative) as (parent, name):
            if parent is None:
                raise Changed(relative)
            temporary = f"{TEMP_PREFIX}{uuid.uuid4().hex}{TEMP_SUFFIX}"
            os.symlink(target, temporary, dir_fd=parent)
            try:
                current = os.stat(name, dir_fd=parent, follow_symlinks=False)
                if Fingerprint.of(current) != expected:
                    raise Changed(relative)
                os.rename(temporary, name, src_dir_fd=parent, dst_dir_fd=parent)
            except BaseException:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(temporary, dir_fd=parent)
                raise

    def make_directory(self, relative, mode):
        """Create a folder (and its parents); return True if it was created."""
        with self.parent(relative, create=True, mode=mode) as (parent, name):
            try:
                os.mkdir(name, self.mode(mode) | 0o700, dir_fd=parent)
            except FileExistsError:
                metadata = os.stat(name, dir_fd=parent, follow_symlinks=False)
                if not stat.S_ISDIR(metadata.st_mode):
                    raise UnsafePath(relative, "is not a folder on this computer") from None
                return False
            return True

    def rename_away(self, relative, target_relative, expected):
        """Move relative to target_relative on the same filesystem, checking it first."""
        with self.parent(target_relative, create=True, mode=0o700) as (target_parent, target_name):
            with self.parent(relative) as (parent, name):
                if parent is None:
                    raise Changed(relative)
                current = os.stat(name, dir_fd=parent, follow_symlinks=False)
                if Fingerprint.of(current) != expected:
                    raise Changed(relative)
                os.rename(name, target_name, src_dir_fd=parent, dst_dir_fd=target_parent)

    def remove_temporaries(self, relative):
        """Remove our own leftover temporary files from an interrupted run."""
        removed = 0
        with self.directory(relative) as fd:
            if fd is None:
                return 0
            for name in os.listdir(fd):
                if not is_temporary_name(name):
                    continue
                metadata = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if metadata.st_uid == os.geteuid() and (stat.S_ISREG(metadata.st_mode)
                                                        or stat.S_ISLNK(metadata.st_mode)):
                    os.unlink(name, dir_fd=fd)
                    removed += 1
        return removed
