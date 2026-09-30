import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

from omarchy_import import locate
from tests import fixtures


def ext4_image(path, recover=False, clean=True, label=b"omarchy-factory", blocks=2):
    block = bytearray(2048)
    struct.pack_into("<I", block, 1024 + 0x04, blocks)
    struct.pack_into("<H", block, 1024 + 0x38, locate.EXT4_MAGIC)
    struct.pack_into("<H", block, 1024 + 0x3A, 1 if clean else 0)
    struct.pack_into("<I", block, 1024 + 0x60, locate.INCOMPAT_RECOVER if recover else 0)
    struct.pack_into("<I", block, 1024 + 0x30, 1_700_000_000)
    block[1024 + 0x68:1024 + 0x78] = bytes(range(16))
    block[1024 + 0x78:1024 + 0x78 + len(label)] = label
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(block))


class SuperblockTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.base = Path(scratch.name)

    def test_clean_and_unclean_disks(self):
        ext4_image(self.base / "a.raw")
        superblock = locate.read_superblock(self.base / "a.raw")
        self.assertEqual(superblock.label, "omarchy-factory")
        self.assertTrue(superblock.clean)
        self.assertFalse(superblock.needs_recovery)
        self.assertEqual(superblock.uuid, "00010203-0405-0607-0809-0a0b0c0d0e0f")
        ext4_image(self.base / "b.raw", recover=True, clean=False)
        self.assertTrue(locate.read_superblock(self.base / "b.raw").needs_recovery)

    def test_cut_short_disk_is_explained(self):
        data = self.base / "Users/Ada/AppData/Local/TryOmarchy"
        ext4_image(data / "vm/disk.raw", blocks=4096)
        install = locate.describe(data)
        self.assertIn("incomplete", install.problems[0])

    def test_other_formats(self):
        (self.base / "qcow").write_bytes(b"QFI\xfb" + b"\0" * 4096)
        self.assertIsNone(locate.read_superblock(self.base / "qcow"))
        (self.base / "tiny").write_bytes(b"x")
        self.assertIsNone(locate.read_superblock(self.base / "tiny"))

    def test_hibernation_signature(self):
        self.assertFalse(locate.hibernated(self.base))
        (self.base / "hiberfil.sys").write_bytes(b"HIBR" + b"\0" * 100)
        self.assertTrue(locate.hibernated(self.base))
        (self.base / "hiberfil.sys").write_bytes(b"wake" + b"\0" * 100)
        self.assertFalse(locate.hibernated(self.base))


class FindTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.c = Path(scratch.name) / "c"
        self.d = Path(scratch.name) / "d"
        self.volumes = {self.c: locate.Volume("/dev/nvme0n1p3", "ntfs", "Windows", 0),
                        self.d: locate.Volume("/dev/sda1", "ntfs", "Data", 0)}

    def test_default_folder_and_share(self):
        data = self.c / "Users/Ada/AppData/Local/TryOmarchy"
        ext4_image(data / "vm/disk.raw")
        (data / "settings.json").write_text(json.dumps({"share": "C:\\Users\\Ada\\Work"}))
        installs = locate.find_installs(self.volumes)
        self.assertEqual(len(installs), 1)
        self.assertEqual(installs[0].windows_user, "Ada")
        self.assertEqual(installs[0].format, "raw")
        self.assertEqual(installs[0].share, "C:\\Users\\Ada\\Work")
        self.assertEqual(installs[0].problems, [])

    def test_moved_install_on_another_drive_with_different_case(self):
        default = self.c / "users/ada/appdata/local/tryomarchy"
        default.mkdir(parents=True)
        (default / "data-location.json").write_text(
            json.dumps({"version": 1, "path": "D:\\Omarchy\\TryOmarchy"}))
        ext4_image(self.d / "omarchy/TryOmarchy/VM/disk.raw")
        installs = locate.find_installs(self.volumes)
        self.assertEqual(len(installs), 1)
        self.assertEqual(installs[0].volume.label, "Data")

    def test_portable_disk_is_explained(self):
        data = self.c / "Users/Ada/AppData/Local/TryOmarchy/vm"
        data.mkdir(parents=True)
        (data / "disk.qcow2").write_bytes(b"QFI\xfb")
        installs = locate.find_installs(self.volumes)
        self.assertIn("portable", installs[0].problems[0])

    def test_nothing_found(self):
        self.assertEqual(locate.find_installs(self.volumes), [])

    def test_windows_paths(self):
        self.assertEqual(locate.windows_relative("D:\\Omarchy\\Try"), "Omarchy/Try")
        self.assertIsNone(locate.windows_relative("\\\\server\\share\\x"))
        self.assertIsNone(locate.windows_relative("relative\\x"))
        self.assertIsNone(locate.windows_relative("C:\\"))

    def test_lsblk_volumes(self):
        tree = {"blockdevices": [{"path": "/dev/nvme0n1", "fstype": None, "children": [
            {"path": "/dev/nvme0n1p1", "fstype": "vfat", "label": "SYSTEM", "size": 1,
             "mountpoints": ["/boot"]},
            {"path": "/dev/nvme0n1p3", "fstype": "ntfs", "label": "Windows", "size": 500,
             "mountpoints": [None]},
            {"path": "/dev/nvme0n1p5", "fstype": "ntfs", "label": None, "size": 9,
             "mountpoints": ["/run/media/ada/Data"]},
            {"path": "/dev/nvme0n1p6", "fstype": "BitLocker", "label": None, "size": 9,
             "mountpoints": [None]}]}]}
        runner = fixtures.FakeRunner({"lsblk": json.dumps(tree)}, programs={"lsblk"})
        volumes = locate.list_volumes(runner)
        self.assertEqual([(volume.device, volume.fstype, volume.mountpoint) for volume in volumes],
                         [("/dev/nvme0n1p3", "ntfs", None),
                          ("/dev/nvme0n1p5", "ntfs", "/run/media/ada/Data"),
                          ("/dev/nvme0n1p6", "bitlocker", None)])


if __name__ == "__main__":
    unittest.main()
