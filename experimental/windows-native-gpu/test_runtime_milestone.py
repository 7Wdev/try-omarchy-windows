"""Reject false D3D12 initialization milestone reports; no GPU is used."""
import unittest
from test_owned_runtime_qemu import initialization_complete


class MilestoneTests(unittest.TestCase):
    def setUp(self):
        self.log = '\n'.join(('factory=00000000', 'list=00000000', 'nvidia vendor=4318 device=11352',
            'nvidiaUmdPresentDuringPrivateQuery=true', 'LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=true',
            'transport=virtio-port', 'realDxgForwarding=false', 'device=00000000', 'copyQueue=00000000', 'BRIDGE_RUNTIME_EXIT=0'))
        self.control = {'ownedQemuExited':True, 'qemuExit':0, 'qemuForcedStop':False, 'fenceControlFailed':False}
        self.cleanup = {'driverCleanupVerified':True}

    def test_all_conditions_required(self):
        self.assertTrue(initialization_complete(self.log, 0, self.control, self.cleanup))
        for line in self.log.splitlines():
            with self.subTest(missing=line):
                self.assertFalse(initialization_complete(self.log.replace(line, ''), 0, self.control, self.cleanup))
        for key in self.control:
            control = dict(self.control); del control[key]
            self.assertFalse(initialization_complete(self.log, 0, control, self.cleanup))
        self.assertFalse(initialization_complete(self.log, 1, self.control, self.cleanup))
        self.assertFalse(initialization_complete(self.log, 0, self.control, {}))

    def test_partial_or_failed_device_and_queue(self):
        for old, new in (('device=00000000', 'device=8007000e'), ('copyQueue=00000000', 'copyQueue=80004001'),
                         ('BRIDGE_RUNTIME_EXIT=0', 'BRIDGE_RUNTIME_EXIT=1'), ('device=00000000', 'device=000000001')):
            self.assertFalse(initialization_complete(self.log.replace(old, new), 0, self.control, self.cleanup))
        self.assertFalse(initialization_complete('host device=00000000\ncopyQueue=00000000', 0, self.control, self.cleanup))
        for extra in ('device=80004005', 'copyQueue=80004001', 'BRIDGE_RUNTIME_EXIT=1', 'device=00000000'):
            self.assertFalse(initialization_complete(self.log + '\n' + extra, 0, self.control, self.cleanup))


if __name__ == '__main__':
    unittest.main()
