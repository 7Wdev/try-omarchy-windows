"""Reject false D3D12 initialization milestone reports; no GPU is used."""
import unittest
from test_owned_runtime_qemu import initialization_complete, copy_workload_complete, clear_workload_complete, expected_clear_pixels, no_broadcast_queue_eof_complete


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


class ClearEvidenceTests(unittest.TestCase):
    def setUp(self):
        lines = ['GPU_CLEAR_TEST_BEGIN width=130 height=73 format=R8G8B8A8_UNORM rounds=2',
                 'nativeCommandSubmitted=true', 'nativeCommandSubmitted=true',
                 'GPU_CLEAR_FOOTPRINT width=130 height=73 offset=512 rowPitch=768 rowBytes=520 rows=73 totalBytes=56328']
        for stage, count in (('DirectQueue',1), ('RenderTarget',1), ('Readback',1), ('RtvHeap',1), ('Allocator',1),
                             ('CommandList',1), ('Fence',1), ('Close',2), ('Signal',2), ('ReadbackMap',2),
                             ('AllocatorReset',1), ('CommandListReset',1)):
            lines.extend([f'gpuClear{stage}=00000000'] * count)
        for round_ in (1,2):
            checksum = 14695981039346656037
            for byte in expected_clear_pixels(round_):
                checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
            lines.append(f'GPU_CLEAR_ROUND round={round_} verifiedPixels=9490 expectedHash={checksum:016x} observedHash={checksum:016x} fenceTarget={round_} fenceObserved={round_}')
        pixels = expected_clear_pixels(2)
        lines.extend(f'GPU_CLEAR_PIXEL_ROW round=2 y={y} rgba={pixels[y*520:(y+1)*520].hex()}' for y in range(73))
        lines.append('GPU_CLEAR_TEST_COMPLETE verified=true width=130 height=73 rounds=2')
        self.log = '\n'.join(lines)

    def test_complete_graphics_evidence_required(self):
        self.assertTrue(clear_workload_complete(self.log))
        self.assertFalse(copy_workload_complete(self.log))
        for line in self.log.splitlines():
            with self.subTest(missing=line[:70]):
                self.assertFalse(clear_workload_complete(self.log.replace(line, '', 1)))

    def test_incorrect_layout_pixels_fence_or_status_rejected(self):
        for old,new in (('rowPitch=768','rowPitch=520'), ('totalBytes=56328','totalBytes=4096'), ('offset=512','offset=0'),
                        ('rowBytes=520','rowBytes=512'), ('verifiedPixels=9490','verifiedPixels=0'),
                        ('gpuClearSignal=00000000','gpuClearSignal=80004001'),
                        ('fenceObserved=2','fenceObserved=1'), ('fenceObserved=2','fenceObserved=18446744073709551615'),
                        ('rgba=0000ffff','rgba=ffffffff'), ('y=72 rgba=','y=71 rgba=')):
            self.assertFalse(clear_workload_complete(self.log.replace(old,new)))
        self.assertFalse(clear_workload_complete(self.log + '\ngpuClearDeviceRemoved=887a0005'))

    def test_duplicate_out_of_order_or_stale_pixels_rejected(self):
        for line in self.log.splitlines():
            if line.startswith(('GPU_CLEAR', 'gpuClear')):
                self.assertFalse(clear_workload_complete(self.log + '\n' + line))
        self.assertFalse(clear_workload_complete(self.log.replace('round=1 verifiedPixels','round=2 verifiedPixels')))
        self.assertFalse(clear_workload_complete(self.log.replace('GPU_CLEAR_PIXEL_ROW round=2','GPU_CLEAR_PIXEL_ROW round=1')))


class NoBroadcastQueueEofTests(unittest.TestCase):
    def setUp(self):
        self.log = '\n'.join(('device=00000000', 'copyQueue=00000000',
            'GPU_CLEAR_TEST_BEGIN width=130 height=73 format=R8G8B8A8_UNORM rounds=2',
            'LINUX_BRIDGE guestFenceUnmapped=true',
            'LINUX_BRIDGE allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0 bytes=1052672',
            'LINUX_BRIDGE nativeHwQueueCreated=true privateBytes=144 progressFenceDirect=true flags=2',
            'LINUX_BRIDGE hwQueueEofTest=true exitingWithQueueOwned=true flags=2', 'BRIDGE_RUNTIME_EXIT=1'))
        self.control = {'ownedQemuExited':True, 'qemuExit':0, 'qemuForcedStop':False, 'fenceControlFailed':False,
                        'fenceMappingsCreated':2, 'fenceUnmapAcknowledgements':1,
                        'allocationUnmapAcknowledgements':1, 'allocationSlotUnmapAcknowledgements':2}
        self.cleanup = {'driverCleanupVerified':True, 'completedNoBroadcastSignalHwQueues':1, 'hwQueuesReleasedAfterVmExit':1}

    def test_actual_unmap_acks_and_retained_queue_required(self):
        self.assertTrue(no_broadcast_queue_eof_complete(self.log, self.control, self.cleanup))
        for key in self.control:
            control = dict(self.control); del control[key]
            self.assertFalse(no_broadcast_queue_eof_complete(self.log, control, self.cleanup))
        for key in self.cleanup:
            cleanup = dict(self.cleanup); del cleanup[key]
            self.assertFalse(no_broadcast_queue_eof_complete(self.log, self.control, cleanup))
        for key in ('fenceUnmapAcknowledgements','allocationUnmapAcknowledgements','allocationSlotUnmapAcknowledgements'):
            for bad in (-1,0,3):
                control = dict(self.control); control[key] = bad
                self.assertFalse(no_broadcast_queue_eof_complete(self.log, control, self.cleanup))

    def test_wrong_exit_flag_or_workload_is_rejected(self):
        for old,new in (('flags=2','flags=0'), ('BRIDGE_RUNTIME_EXIT=1','BRIDGE_RUNTIME_EXIT=0'),
                        ('bytes=1052672','bytes=1052673'), ('bytes=1052672','bytes=4198400'),
                        ('copyQueue=00000000','copyQueue=80004005')):
            self.assertFalse(no_broadcast_queue_eof_complete(self.log.replace(old,new), self.control, self.cleanup))
        for extra in ('GPU_COPY_TEST_BEGIN', 'gpuClearDirectQueue=00000000', 'GPU_CLEAR_ROUND', 'GPU_CLEAR_TEST_COMPLETE',
                      'LINUX_BRIDGE hwQueueEofTest=true exitingWithQueueOwned=true flags=2'):
            self.assertFalse(no_broadcast_queue_eof_complete(self.log+'\n'+extra, self.control, self.cleanup))
        self.assertFalse(clear_workload_complete(self.log))
        self.assertFalse(copy_workload_complete(self.log))
        self.assertFalse(initialization_complete(self.log, 0, self.control, self.cleanup))

    def test_combined_wait_queue_requires_exact_native_counts(self):
        log = self.log.replace('flags=2', 'flags=6')
        prior = 'LINUX_BRIDGE nativeHwQueueCreated=true privateBytes=144 progressFenceDirect=true flags=2\n'
        log = prior * 2 + log
        cleanup = dict(self.cleanup, completedNoBroadcastSignalHwQueues=3, completedNoBroadcastWaitHwQueues=1)
        self.assertTrue(no_broadcast_queue_eof_complete(log, self.control, cleanup, 6))
        self.assertFalse(no_broadcast_queue_eof_complete(log, self.control, cleanup))
        self.assertFalse(no_broadcast_queue_eof_complete(self.log, self.control, self.cleanup, 6))
        for flag in (0, 1, 4, 5, 7, 8):
            self.assertFalse(no_broadcast_queue_eof_complete(log, self.control, cleanup, flag))
        for key in ('completedNoBroadcastSignalHwQueues','completedNoBroadcastWaitHwQueues'):
            for bad in (-1,0,2,4,None):
                changed = dict(cleanup); changed[key] = bad
                self.assertFalse(no_broadcast_queue_eof_complete(log, self.control, changed, 6))
        extra = prior.replace('flags=2', 'flags=6')
        self.assertFalse(no_broadcast_queue_eof_complete(log + '\n' + extra, self.control, cleanup, 6))
        self.assertFalse(clear_workload_complete(log))
        self.assertFalse(copy_workload_complete(log))
        self.assertFalse(initialization_complete(log, 0, self.control, cleanup))


if __name__ == '__main__':
    unittest.main()
