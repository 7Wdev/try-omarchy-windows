"""Reject false D3D12 initialization milestone reports; no GPU is used."""
import unittest
from test_owned_runtime_qemu import initialization_complete, copy_workload_complete


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


class CopyEvidenceTests(unittest.TestCase):
    def setUp(self):
        lines = ['GPU_COPY_TEST_BEGIN bytes=65536 rounds=2', 'nativeCommandSubmitted=true', 'nativeCommandSubmitted=true']
        for stage, count in (('Upload',1), ('Default',1), ('Readback',1), ('Allocator',1), ('CommandList',1), ('Fence',1),
                             ('UploadMap',2), ('ReadbackMap',2), ('Close',2), ('Signal',2), ('AllocatorReset',1), ('CommandListReset',1)):
            lines.extend([f'gpuCopy{stage}=00000000'] * count)
        for round_, checksum in ((1,'28e3f2dd58641325'), (2,'3ce54625a5a41325')):
            lines.append(f'GPU_COPY_ROUND round={round_} verifiedBytes=65536 expectedHash={checksum} observedHash={checksum} fenceTarget={round_} fenceObserved={round_}')
        lines.append('GPU_COPY_TEST_COMPLETE verified=true bytes=65536 rounds=2')
        self.log = '\n'.join(lines)

    def test_no_cpu_only_or_partial_success(self):
        self.assertTrue(copy_workload_complete(self.log))
        for line in self.log.splitlines():
            with self.subTest(missing=line):
                self.assertFalse(copy_workload_complete(self.log.replace(line, '', 1)))
        for old,new in (('verifiedBytes=65536','verifiedBytes=0'), ('gpuCopySignal=00000000','gpuCopySignal=80004001'),
                        ('observedHash=28e3f2dd58641325','observedHash=0000000000000000'),
                        ('expectedHash=28e3f2dd58641325','expectedHash=0000000000000000'),
                        ('fenceObserved=2','fenceObserved=1'), ('fenceObserved=2','fenceObserved=18446744073709551615')):
            self.assertFalse(copy_workload_complete(self.log.replace(old,new)))

    def test_duplicate_or_out_of_order_evidence_rejected(self):
        for line in self.log.splitlines():
            if line.startswith(('GPU_COPY_TEST','GPU_COPY_ROUND','gpuCopy')):
                self.assertFalse(copy_workload_complete(self.log + '\n' + line))
        self.assertFalse(copy_workload_complete(self.log.replace('round=1 verifiedBytes','round=2 verifiedBytes')))


if __name__ == '__main__':
    unittest.main()
