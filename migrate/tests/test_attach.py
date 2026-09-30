import os
from pathlib import Path
import tempfile
import unittest

from omarchy_import import attach
from tests import fixtures


class SessionTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.workdir = Path(scratch.name) / "work"
        self.workdir.mkdir()
        self.runner = fixtures.FakeRunner({
            "losetup --find --show --read-only": "/dev/loop7\n",
            "losetup --find --show /var/tmp": "/dev/loop8\n",
            "blockdev --getsz": "14680064\n",
        })

    def test_clean_disk_is_mounted_read_only_and_undone_in_reverse(self):
        session = attach.Session(self.runner, self.workdir)
        target = session.attach_disk("/win/disk.raw")
        self.assertTrue(target.is_dir())
        session.close()
        commands = self.runner.commands()
        self.assertEqual(commands[0], "losetup --find --show --read-only /win/disk.raw")
        self.assertTrue(commands[1].startswith("mount -t ext4 -o ro,nodev,nosuid,noexec /dev/loop7"))
        self.assertTrue(commands[2].startswith("umount"))
        self.assertEqual(commands[3], "losetup --detach /dev/loop7")
        self.assertTrue(all(sudo for _, sudo in self.runner.calls))
        self.assertFalse(target.exists())

    def test_unclean_disk_is_recovered_in_a_snapshot(self):
        session = attach.Session(self.runner, self.workdir)
        session.attach_disk("/win/disk.raw", needs_recovery=True)
        commands = self.runner.commands()
        create = next(command for command in commands if command.startswith("dmsetup create"))
        self.assertIn("snapshot /dev/loop7 /dev/loop8 N 8", create)
        self.assertTrue(any(command.startswith("mount -t ext4 -o ro,nodev,nosuid,noexec "
                                               "/dev/mapper/try-omarchy-import-")
                            for command in commands))
        cow = [path for path in Path("/var/tmp").glob(f"try-omarchy-import-*.cow")
               if str(path) in " ".join(commands)]
        self.assertEqual(len(cow), 1)
        session.close()
        self.assertFalse(cow[0].exists())
        tail = self.runner.commands()[-4:]
        self.assertTrue(tail[0].startswith("umount"))
        self.assertTrue(tail[1].startswith("dmsetup remove"))
        self.assertEqual(tail[2], "losetup --detach /dev/loop8")
        self.assertEqual(tail[3], "losetup --detach /dev/loop7")

    def test_owner_mapping_only_when_needed(self):
        session = attach.Session(self.runner, self.workdir)
        self.assertEqual(session.map_owner("/root", os.getuid(), os.getgid()), Path("/root"))
        mapped = session.map_owner("/root", 1000 if os.getuid() != 1000 else 1001, 1000)
        self.assertNotEqual(mapped, Path("/root"))
        command = self.runner.commands()[-1]
        self.assertIn("X-mount.idmap=u:", command)
        self.assertIn(f":{os.getuid()}:1 g:", command)
        session.close()

    def test_windows_drive_is_read_only_and_private(self):
        session = attach.Session(self.runner, self.workdir)
        session.mount_windows("/dev/nvme0n1p3")
        command = self.runner.commands()[0]
        self.assertIn("-t ntfs3", command)
        self.assertIn("ro,", command)
        self.assertIn("fmask=0177,dmask=0077", command)
        session.close()

    def test_failed_undo_steps_are_reported(self):
        session = attach.Session(self.runner, self.workdir)
        session.attach_disk("/win/disk.raw")
        self.runner.failing.add("umount")
        errors = session.close()
        self.assertTrue(any("umount" in error for error in errors))


class LeftoverTests(unittest.TestCase):
    def test_only_our_loop_devices_are_recognised(self):
        self.assertTrue(attach.is_our_loop_backing(
            "/run/media/ada/Windows/Users/Ada/AppData/Local/TryOmarchy/vm/disk.raw"))
        self.assertTrue(attach.is_our_loop_backing("/var/tmp/try-omarchy-import-x1y2.cow"))
        self.assertFalse(attach.is_our_loop_backing("/home/ada/vms/win/vm/disk.raw"))
        self.assertFalse(attach.is_our_loop_backing("/home/ada/try-omarchy-import-x.cow"))
        self.assertFalse(attach.is_our_loop_backing("/var/tmp/other.cow"))

    def test_cleanup_unmounts_windows_drives_last(self):
        from omarchy_import import cli
        from omarchy_import.ui import UI
        import io
        from unittest import mock
        runner = fixtures.FakeRunner({
            "dmsetup ls": "try-omarchy-import-77-2\t(254:0)\n",
            "losetup --list": "/dev/loop5 /run/user/1000/try-omarchy-import/x/windows-1/VMs/vm/disk.raw\n",
        })
        mounts = ["/run/user/1000/try-omarchy-import/x/windows-1",
                  "/run/user/1000/try-omarchy-import/x/trial-2"]
        with mock.patch.object(attach, "leftover_mounts", return_value=mounts), \
                mock.patch.object(attach, "remove_scratch_files", return_value=0):
            cli.cleanup(UI(interactive=False, stream=io.StringIO()), runner)
        commands = runner.commands()
        order = [commands.index(command) for command in (
            "umount /run/user/1000/try-omarchy-import/x/trial-2",
            "dmsetup remove try-omarchy-import-77-2",
            "losetup --detach /dev/loop5",
            "umount /run/user/1000/try-omarchy-import/x/windows-1")]
        self.assertEqual(order, sorted(order))

    def test_cleanup_removes_snapshots_before_loops(self):
        from omarchy_import import cli
        from omarchy_import.ui import UI
        import io
        runner = fixtures.FakeRunner({
            "dmsetup ls": "try-omarchy-import-77-2\t(254:0)\nluks-root\t(254:1)\n",
            "losetup --list": "/dev/loop3 /var/tmp/try-omarchy-import-abc.cow\n"
                              "/dev/loop4 /home/ada/other.img\n",
        })
        status = cli.cleanup(UI(interactive=False, stream=io.StringIO()), runner)
        self.assertEqual(status, 0)
        commands = runner.commands()
        self.assertLess(commands.index("dmsetup remove try-omarchy-import-77-2"),
                        commands.index("losetup --detach /dev/loop3"))
        self.assertNotIn("dmsetup remove luks-root", commands)
        self.assertNotIn("losetup --detach /dev/loop4", commands)


if __name__ == "__main__":
    unittest.main()
