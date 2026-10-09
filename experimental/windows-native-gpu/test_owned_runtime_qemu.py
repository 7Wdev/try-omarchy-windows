"""Verify live paging in QEMU owned by the Windows KMT driver process.

This expects incomplete D3D12 initialization; it never signifies a usable GPU.
The supplied runtime image is private and must never be uploaded.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import threading


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'firmware', 'kernel', 'initramfs', 'bridge', 'report'):
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    parser.add_argument('--expected-unimplemented-ioctl', type=int, required=True)
    parser.add_argument('--driver-allocations', action='store_true', help='Explicitly enable diagnostic vendor video-memory allocations')
    parser.add_argument('--minimum-vendor-allocations', type=int, default=0)
    parser.add_argument('--driver-gpuva', action='store_true')
    parser.add_argument('--minimum-vendor-gpuva-maps', type=int, default=0)
    parser.add_argument('--driver-residency', action='store_true')
    parser.add_argument('--minimum-vendor-residency-requests', type=int, default=0)
    parser.add_argument('--driver-cpu', action='store_true')
    parser.add_argument('--minimum-vendor-cpu-locks', type=int, default=0)
    parser.add_argument('--driver-translation', action='store_true')
    parser.add_argument('--minimum-allocation-translations', type=int, default=0)
    parser.add_argument('--driver-hwqueues', action='store_true')
    parser.add_argument('--minimum-hwqueues', type=int, default=0)
    parser.add_argument('--driver-syncs', action='store_true')
    parser.add_argument('--minimum-monitored-fences', type=int, default=0)
    parser.add_argument('--minimum-sync-mutexes', type=int, default=0)
    parser.add_argument('--driver-submit', action='store_true')
    parser.add_argument('--minimum-submissions', type=int, default=0)
    parser.add_argument('--driver-retirement', action='store_true')
    parser.add_argument('--minimum-retirements-with-queues', type=int, default=0)
    parser.add_argument('--expected-allocation-limit', type=int, default=0, help='Require the observed diagnostic allocation-count boundary')
    parser.add_argument('--cpu-store-test', action='store_true', help='Explicit first/last-word diagnostic stores, checked and restored by Windows')
    parser.add_argument('--cpu-eof-test', action='store_true', help='Exit the guest probe while it owns the CPU lock; require VM-exit-first native teardown')
    parser.add_argument('--hwqueue-eof-test', action='store_true', help='Exit the guest probe with its first hardware queue still owned')
    args = parser.parse_args()
    guest_eof = args.cpu_eof_test or args.hwqueue_eof_test
    if args.minimum_vendor_allocations < 0 or (args.minimum_vendor_allocations and not args.driver_allocations):
        parser.error('Minimum allocation acceptance requires the explicit allocation opt-in')
    if args.minimum_vendor_gpuva_maps < 0 or (args.driver_gpuva and not args.driver_allocations) or (args.minimum_vendor_gpuva_maps and not args.driver_gpuva):
        parser.error('GPU-address mapping acceptance requires explicit allocation and GPU-address opt-ins')
    if args.minimum_vendor_residency_requests < 0 or (args.driver_residency and not args.driver_allocations) or (args.minimum_vendor_residency_requests and not args.driver_residency):
        parser.error('Residency acceptance requires explicit allocation and residency opt-ins')
    if args.minimum_vendor_cpu_locks < 0 or (args.driver_cpu and not args.driver_gpuva) or (args.minimum_vendor_cpu_locks and not args.driver_cpu):
        parser.error('CPU-lock acceptance requires explicit CPU and GPU-address opt-ins')
    if args.cpu_store_test and not args.driver_cpu:
        parser.error('CPU store control requires explicit CPU-lock opt-in')
    if args.minimum_allocation_translations < 0 or (args.driver_translation and not args.driver_allocations) or (args.minimum_allocation_translations and not args.driver_translation):
        parser.error('Allocation translation acceptance requires allocation and translation opt-ins')
    if args.minimum_hwqueues < 0 or (args.driver_hwqueues and not args.driver_translation) or (args.minimum_hwqueues and not args.driver_hwqueues):
        parser.error('Hardware queue acceptance requires allocation translation and hardware queue opt-ins')
    if args.expected_allocation_limit < 0 or (args.expected_allocation_limit and not args.driver_allocations):
        parser.error('An allocation-limit boundary requires allocation opt-in')
    if args.minimum_retirements_with_queues < 0 or (args.driver_retirement and not args.driver_hwqueues) or (args.minimum_retirements_with_queues and not args.driver_retirement):
        parser.error('Allocation retirement requires the owned hardware queue opt-in')
    if min(args.minimum_monitored_fences, args.minimum_sync_mutexes) < 0 or ((args.minimum_monitored_fences or args.minimum_sync_mutexes) and not args.driver_syncs):
        parser.error('Synchronization acceptance requires the explicit synchronization opt-in')
    if args.cpu_eof_test and (not args.driver_cpu or args.cpu_store_test):
        parser.error('CPU EOF control requires CPU locks and a separate run from store control')
    if args.hwqueue_eof_test and (not args.driver_hwqueues or args.cpu_eof_test or args.cpu_store_test):
        parser.error('Hardware queue EOF control requires hardware queues and a separate diagnostic run')
    if args.minimum_submissions < 0 or (args.minimum_submissions and not args.driver_submit) or (
            args.driver_submit and (not args.driver_hwqueues or not args.driver_syncs or not args.driver_cpu or not args.driver_residency or
                                    args.cpu_store_test or args.cpu_eof_test or args.hwqueue_eof_test)):
        parser.error('Submission requires queues, syncs, CPU mappings and residency in a separate diagnostic run')
    if sys.platform != 'win32':
        parser.error('Run on the Windows NVIDIA host')
    for path in (args.qemu, args.kernel, args.initramfs, args.bridge):
        if not path.is_file():
            parser.error(f'Missing file: {path}')
    log_path = args.report.with_suffix('.log')
    if args.report.exists() or log_path.exists():
        parser.error('Use a fresh report path')
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    api.OpenProcess.restype = wintypes.HANDLE
    api.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    api.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    owner = subprocess.Popen([str(args.bridge.resolve()), '--run-qemu', str(args.qemu.resolve()),
                              str(args.firmware.resolve()), str(args.kernel.resolve()), str(args.initramfs.resolve()),
                              str(log_path.resolve()), '--driver-contexts', '--driver-queries'] +
                             (['--driver-allocations'] if args.driver_allocations else []) + (['--driver-gpuva'] if args.driver_gpuva else []) +
                             (['--driver-residency'] if args.driver_residency else []) + (['--driver-cpu'] if args.driver_cpu else []) +
                             (['--driver-translation'] if args.driver_translation else []) +
                             (['--driver-hwqueues'] if args.driver_hwqueues else []) +
                             (['--driver-syncs'] if args.driver_syncs else []) +
                             (['--driver-submit'] if args.driver_submit else []) +
                             (['--driver-retirement'] if args.driver_retirement else []) +
                             (['--cpu-store-test'] if args.cpu_store_test else []) + (['--cpu-eof-test'] if args.cpu_eof_test else []) +
                             (['--hwqueue-eof-test'] if args.hwqueue_eof_test else []),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    output, errors, observer_errors = [], [], []
    child_handle = None

    def read_stdout():
        nonlocal child_handle
        try:
            for line in owner.stdout:
                output.append(line)
                message = json.loads(line)
                if 'ownedQemuPid' in message:
                    handle = api.OpenProcess(0x00101001, False, message['ownedQemuPid'])
                    if not handle:
                        raise RuntimeError('Cannot retain the owned QEMU process for timeout cleanup')
                    name, size = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
                    if not api.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)) or pathlib.Path(name.value).resolve() != args.qemu.resolve():
                        api.CloseHandle(handle)
                        raise RuntimeError('Owned QEMU executable identity mismatch')
                    child_handle = handle
        except Exception as error:
            observer_errors.append(str(error))

    reader = threading.Thread(target=read_stdout)
    error_reader = threading.Thread(target=lambda: errors.extend(owner.stderr.readlines()))
    reader.start(); error_reader.start()
    try:
        try:
            owner.wait(timeout=110)
        except subprocess.TimeoutExpired:
            # The retained handle cannot target a reused PID. Stop and reap
            # QEMU first, while the owner still retains the native pages.
            if child_handle:
                if api.WaitForSingleObject(child_handle, 0) != 0:
                    if not api.TerminateProcess(child_handle, 91) or api.WaitForSingleObject(child_handle, 10000) != 0:
                        raise RuntimeError('Cannot reap QEMU; native owner deliberately retained')
            elif any('ownedQemuPid' in line for line in output):
                raise RuntimeError('Cannot verify child exit; native owner deliberately retained')
            owner.kill(); owner.wait(timeout=10)
            raise RuntimeError('Owned runtime deadline exceeded')
        reader.join(timeout=5); error_reader.join(timeout=5)
        if reader.is_alive() or error_reader.is_alive() or observer_errors:
            raise RuntimeError('Owner output observation failed: ' + '; '.join(observer_errors))
        control = next((json.loads(line) for line in output if 'ownedQemuExited' in line), {})
        cleanup = next((json.loads(line) for line in errors if line.startswith('{') and 'driverCleanupVerified' in line), {})
        log = log_path.read_text(encoding='utf-8', errors='replace') if log_path.exists() else ''
        unsupported = sorted(set(int(n) for n in re.findall(r'LINUX_BRIDGE unsupported nr=(\d+)', log)))
        completed = [{'type': int(t), 'bytes': int(b)} for t, b in re.findall(r'LINUX_BRIDGE queryCompleted type=(\d+) bytes=(\d+)', log)]
        fences = [{'offset': int(o), 'value': int(v)} for o, v in re.findall(r'pagingFenceMapped=true direct=true loads=10000 offset=(\d+) value=(\d+)', log)]
        result = re.search(r'^device=([0-9a-f]{8})\s*$', log, re.MULTILINE)
        loaded = 'nvidiaUmdPresentDuringPrivateQuery=true' in log
        contexts = log.count('LINUX_BRIDGE nativeContextCreated=true')
        allocations = log.count('LINUX_BRIDGE nativeVendorAllocationCreated=true')
        translations = log.count('LINUX_BRIDGE allocationDriverAliasCreated=true typedWireIdentityRetained=true')
        hwqueues = log.count('LINUX_BRIDGE nativeHwQueueCreated=true')
        hwqueue_unmaps = log.count('LINUX_BRIDGE nativeHwQueueDestroyed=true')
        hwqueue_fences = [{'offset': int(o), 'value': int(v)} for o, v in re.findall(r'hwQueueFenceMapped=true direct=true loads=10000 offset=(\d+) value=(\d+)', log)]
        sync_types = [int(t) for t in re.findall(r'LINUX_BRIDGE nativeSynchronizationCreated=true type=(\d+)', log)]
        sync_destroyed = [int(t) for t in re.findall(r'LINUX_BRIDGE nativeSynchronizationDestroyed=true type=(\d+)', log)]
        monitored_fences = [{'offset': int(o), 'value': int(v)} for o, v in re.findall(r'monitoredFenceMapped=true direct=true loads=10000 offset=(\d+) value=(\d+)', log)]
        total_fence_mappings = len(fences) + len(hwqueue_fences) + len(monitored_fences)
        gpuva = [{'pages': int(p), 'status': int(s), 'fence': int(f)} for p, s, f in
                 re.findall(r'nativeVendorGpuVaMapped=true pages=(\d+) status=(\d+) fence=(\d+)', log)]
        retirements = [{'target': int(t), 'observed': int(o), 'operation': op or 'gpuva'} for t, o, op in
                       re.findall(r'pagingFenceRetired=true direct=true loads=10000 target=(\d+) observed=(\d+)(?: operation=(\w+))?', log)]
        residency = [{'count': int(c), 'status': int(s), 'fence': int(f), 'bytesToTrim': int(b)} for c, s, f, b in
                     re.findall(r'nativeVendorResident=true count=(\d+) status=(\d+) fence=(\d+) bytesToTrim=(\d+)', log)]
        cpu = [{'bytes': int(b), 'offset': int(o), 'generation': int(g)} for b, o, g in
               re.findall(r'allocationCpuMapped=true direct=true loads=10000 bytes=(\d+) offset=(\d+) generation=(\d+)', log)]
        cpu_unlocks = log.count('nativeVendorCpuUnlocked=true')
        cpu_unmaps = log.count('allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0')
        cpu_released_after_exit = cleanup.get('cpuLocksReleasedAfterVmExit', 0)
        allocation_limit_rejections = log.count('LINUX_BRIDGE ioctlFailed nr=6 errno=24')
        retirement_batches = [{'count': int(c), 'queues': int(q)} for c, q in re.findall(
            r'nativeVendorAllocationsDestroyed=true count=(\d+) hardwareQueuesAlive=(\d+)', log)]
        retirements_with_queues = sum(batch['count'] for batch in retirement_batches if batch['queues'])
        map_retirements = [r for r in retirements if r['operation'] == 'gpuva']
        resident_retirements = [r for r in retirements if r['operation'] == 'residency']
        command_retirements = [r for r in retirements if r['operation'] == 'command']
        submissions = [{'bytes': int(b), 'privateBytes': int(p), 'target': int(t), 'observed': int(o)} for b, p, t, o in
                       re.findall(r'nativeCommandSubmitted=true bytes=(\d+) privateBytes=(\d+) target=(\d+) observed=(\d+)', log)]
        submission_preflight = [json.loads(line) for line in errors if line.startswith('{') and 'nativeSubmissionPreflight' in line]
        accepted_stage_marker = ('nativeCommandSubmitted=true' if args.driver_submit else
                                 'nativeHwQueueCreated=true' if args.driver_hwqueues else
                                 'nativeVendorCpuLocked=true' if args.driver_cpu else
                                 'nativeVendorResident=true' if args.driver_residency else
                                 'nativeVendorGpuVaMapped=true' if args.driver_gpuva else
                                 'nativeVendorAllocationCreated=true' if args.driver_allocations else
                                 'nativeContextCreated=true')
        boundary = re.search(re.escape(accepted_stage_marker) + r'[^\n]*\n[\s\S]*?LINUX_BRIDGE unsupported nr=(\d+)', log)
        first_unsupported_after_stage = int(boundary[1]) if boundary else None
        accepted = (owner.returncode == 0 and loaded and contexts > 0 and len(fences) > 0 and
                    'LINUX_BRIDGE deviceCreated=true' in log and 'factory=00000000' in log and 'list=00000000' in log and
                    {'type': 0, 'bytes': 50616} in completed and
                    control.get('ownedQemuExited') is True and control.get('qemuExit') == 0 and
                    control.get('qemuForcedStop') is False and control.get('fenceControlFailed') is False and
                    control.get('liveFenceMappings') == 0 and control.get('fenceMappingsCreated') == total_fence_mappings and
                    control.get('fenceUnmapAcknowledgements') == (0 if guest_eof else total_fence_mappings) and
                    cleanup.get('driverCleanupVerified') is True and cleanup.get('failedAdapterQueries') == 0 and
                    cleanup.get('completedAdapterQueries') == len(completed) and
                    'transport=virtio-port' in log and 'BRIDGE_RUNTIME_EXIT=1' in log and
                    (guest_eof or first_unsupported_after_stage == args.expected_unimplemented_ioctl) and
                    (guest_eof or (result is not None and int(result[1], 16) & 0x80000000)))
        if args.driver_allocations:
            accepted = (accepted and allocations >= args.minimum_vendor_allocations and
                        cleanup.get('liveVendorAllocations') == 0 and cleanup.get('failedVendorAllocations') == 0 and
                        cleanup.get('completedVendorAllocations') == allocations and cleanup.get('destroyedVendorAllocations') == allocations and
                        6 not in unsupported and 19 not in unsupported)
        retirement_verified = (args.driver_retirement and retirements_with_queues >= max(1, args.minimum_retirements_with_queues) and
                               cleanup.get('allocationRetirementOptIn') is True and
                               cleanup.get('vendorDestructionsWithHwQueues') == retirements_with_queues)
        if args.driver_retirement:
            accepted = (accepted and retirement_verified)
        else:
            accepted = (accepted and cleanup.get('allocationRetirementOptIn') is False and
                        cleanup.get('vendorDestructionsWithHwQueues') == 0 and retirements_with_queues == 0)
        if args.expected_allocation_limit:
            accepted = (accepted and allocation_limit_rejections > 0 and
                        cleanup.get('peakVendorAllocationObjects') == args.expected_allocation_limit and
                        cleanup.get('vendorAllocationObjectLimit') == args.expected_allocation_limit)
        if args.driver_gpuva:
            accepted = (accepted and len(gpuva) >= args.minimum_vendor_gpuva_maps and 12 not in unsupported and
                        cleanup.get('completedVendorGpuVaMaps') == len(gpuva) and cleanup.get('failedVendorGpuVaMaps') == 0 and
                        cleanup.get('completedVendorGpuVaWaits') == len(gpuva) and cleanup.get('liveVendorMappedPages') == 0 and
                        len(map_retirements) == len(gpuva) and
                        all(retired['target'] == mapped['fence'] and retired['observed'] >= mapped['fence'] for retired, mapped in zip(map_retirements, gpuva)))
        if args.driver_translation:
            accepted = (accepted and translations >= args.minimum_allocation_translations and translations == allocations and
                        cleanup.get('completedAllocationTranslations') == translations and
                        cleanup.get('failedAllocationTranslations') == 0 and cleanup.get('liveAllocationTranslations') == 0)
        if args.driver_hwqueues:
            accepted = (accepted and hwqueues >= args.minimum_hwqueues and hwqueues == len(hwqueue_fences) and hwqueue_unmaps == (0 if args.hwqueue_eof_test else hwqueues) and
                        cleanup.get('completedHwQueues') == hwqueues and cleanup.get('destroyedHwQueues') == hwqueues and
                        cleanup.get('failedHwQueues') == 0 and cleanup.get('liveHwQueues') == 0 and 24 not in unsupported and 27 not in unsupported)
        if args.driver_syncs:
            accepted = (accepted and sync_types.count(5) >= args.minimum_monitored_fences and sync_types.count(1) >= args.minimum_sync_mutexes and
                        all(t in (1, 5) for t in sync_types) and sync_types.count(5) == len(monitored_fences) and
                        cleanup.get('completedSyncObjects') == len(sync_types) and cleanup.get('destroyedSyncObjects') == len(sync_types) and
                        cleanup.get('completedMonitoredFences') == sync_types.count(5) and cleanup.get('completedSynchronizationMutexes') == sync_types.count(1) and
                        cleanup.get('failedSyncObjects') == 0 and cleanup.get('liveSyncObjects') == 0 and 16 not in unsupported and 29 not in unsupported and
                        (not guest_eof or not sync_destroyed) and
                        len(sync_destroyed) + cleanup.get('syncObjectsReleasedAfterVmExit', 0) == len(sync_types) and
                        (guest_eof or sorted(sync_destroyed) == sorted(sync_types)))
        if args.driver_submit:
            accepted = (accepted and len(submissions) >= args.minimum_submissions and 52 not in unsupported and
                        cleanup.get('commandSubmissionOptIn') is True and cleanup.get('completedNativeSubmissions') == len(submissions) and
                        cleanup.get('nativeSubmissionAttempts') == len(submissions) and cleanup.get('failedNativeSubmissions') == 0 and
                        cleanup.get('nativeSubmissionTimeouts') == 0 and cleanup.get('submittedCommandBytes') == sum(s['bytes'] for s in submissions) and
                        len(submission_preflight) == len(submissions) and
                        all(p['initialFence'] < p['targetFence'] and p['targetFence'] == s['target'] for p, s in zip(submission_preflight, submissions)) and
                        len(command_retirements) == len(submissions) and
                        all(s['observed'] >= s['target'] and r['observed'] >= r['target'] and s['target'] == r['target']
                            for s, r in zip(submissions, command_retirements)))
        if args.driver_residency:
            accepted = (accepted and len(residency) >= args.minimum_vendor_residency_requests and 11 not in unsupported and
                        all(r['count'] > 0 and r['status'] in (0, 259) for r in residency) and
                        cleanup.get('completedVendorResidencyRequests') == len(residency) and cleanup.get('failedVendorResidencyRequests') == 0 and
                        cleanup.get('completedVendorResidencyWaits') == len(residency) and cleanup.get('liveVendorResidencyAttempts') == 0 and
                        cleanup.get('vendorAllocationsMadeResident') == sum(r['count'] for r in residency) and
                        len(resident_retirements) == len(residency) and
                        all(retired['target'] == made['fence'] and retired['observed'] >= made['fence'] for retired, made in zip(resident_retirements, residency)))
        if args.driver_cpu:
            accepted = (accepted and len(cpu) >= args.minimum_vendor_cpu_locks and 37 not in unsupported and 55 not in unsupported and
                        cpu_unlocks + cpu_released_after_exit == len(cpu) and cpu_unmaps == cpu_unlocks and
                        (not guest_eof or cpu_unlocks == 0) and
                        cleanup.get('completedVendorCpuLocks') == len(cpu) and cleanup.get('completedVendorCpuUnlocks') == len(cpu) and
                        cleanup.get('failedVendorCpuLocks') == 0 and cleanup.get('failedVendorCpuUnlocks') == 0 and
                        cleanup.get('liveVendorCpuBytes') == 0 and control.get('liveAllocationMappings') == 0 and
                        control.get('liveAllocationMappedBytes') == 0 and control.get('allocationMappingsCreated') == len(cpu) and
                        control.get('allocationUnmapAcknowledgements') == cpu_unlocks)
        if args.cpu_eof_test:
            accepted = (accepted and len(cpu) > 0 and 'allocationCpuEofTest=true exitingWithLockOwned=true' in log and
                        cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and result is None)
        if args.hwqueue_eof_test:
            accepted = (accepted and hwqueues > 0 and 'hwQueueEofTest=true exitingWithQueueOwned=true' in log and
                        cleanup.get('hwQueuesReleasedAfterVmExit') == hwqueues and
                        cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and result is None)
        if args.cpu_store_test:
            accepted = (accepted and len(cpu) > 0 and cleanup.get('cpuStoreTestRequested') is True and
                        cleanup.get('completedCpuStoreTests') == len(cpu) and cleanup.get('failedCpuStoreTests') == 0 and
                        log.count('allocationCpuStoreTest=true firstLastReadback=true') == len(cpu) and
                        log.count('allocationCpuReferenceTest=true sameGuestPointer=true intermediateUnlockRetained=true') == len(cpu))
        report = {'schema': 1, 'diagnosticAccepted': bool(accepted), 'runtimeInitializationComplete': False,
                  'stage': 'owned VM exit before native hardware queue and CPU lock cleanup' if args.hwqueue_eof_test else
                           'owned VM exit before native CPU lock cleanup' if args.cpu_eof_test else
                           'live NVIDIA runtime with native Windows synchronization objects' if args.driver_syncs else
                           'live NVIDIA runtime with native Windows hardware queues' if args.driver_hwqueues else
                           'live NVIDIA runtime with Windows allocation CPU locks' if args.driver_cpu else
                           'live NVIDIA runtime with Windows allocation residency' if args.driver_residency else
                           'live NVIDIA runtime with Windows GPU-address mappings' if args.driver_gpuva else
                           'live NVIDIA runtime with Windows video-memory allocation' if args.driver_allocations else
                           'live NVIDIA runtime with dynamic Windows paging fence mappings',
                  'hypervisor': 'QEMU/WHPX', 'hostBridgeExit': owner.returncode,
                  'liveNvidiaLinuxUmdLoaded': loaded, 'nativeKmtDeviceCreatedByLiveRuntime': 'deviceCreated=true' in log,
                  'nativeKmtContextsCreatedByLiveRuntime': contexts, 'nativePagingFenceMappings': fences,
                  'vendorAllocationOptIn': args.driver_allocations, 'nativeVendorAllocationsCreatedByLiveRuntime': allocations,
                  'minimumVendorAllocationsRequired': args.minimum_vendor_allocations,
                  'expectedAllocationObjectLimit': args.expected_allocation_limit,
                  'allocationLimitRejections': allocation_limit_rejections,
                  'allocationRetirementOptIn': args.driver_retirement,
                  'nativeAllocationRetirementWithQueuesVerified': bool(retirement_verified),
                  'guestAllocationRetirementBatches': retirement_batches,
                  'guestAllocationRetirementsWithHardwareQueues': retirements_with_queues,
                  'minimumRetirementsWithHardwareQueuesRequired': args.minimum_retirements_with_queues,
                  'allocationTranslationOptIn': args.driver_translation, 'guestAllocationDriverAliasesCreated': translations,
                  'minimumAllocationTranslationsRequired': args.minimum_allocation_translations,
                  'hardwareQueueOptIn': args.driver_hwqueues, 'nativeHardwareQueuesCreatedByLiveRuntime': hwqueues,
                  'minimumHardwareQueuesRequired': args.minimum_hwqueues, 'directHardwareQueueFences': hwqueue_fences,
                  'nativeHardwareQueueDestructionsAcknowledged': hwqueue_unmaps,
                  'synchronizationOptIn': args.driver_syncs, 'nativeSynchronizationTypesCreated': sync_types,
                  'nativeSynchronizationTypesDestroyedByGuest': sync_destroyed, 'directMonitoredFences': monitored_fences,
                  'minimumMonitoredFencesRequired': args.minimum_monitored_fences, 'minimumSynchronizationMutexesRequired': args.minimum_sync_mutexes,
                  'commandSubmissionOptIn': args.driver_submit, 'nativeCommandSubmissions': submissions,
                  'nativeCommandPreflightChecks': submission_preflight,
                  'minimumSubmissionsRequired': args.minimum_submissions, 'directCommandRetirementChecks': command_retirements,
                  'vendorGpuVaOptIn': args.driver_gpuva, 'nativeVendorGpuVaMappings': gpuva,
                  'directGuestPagingRetirementChecks': retirements,
                  'minimumVendorGpuVaMappingsRequired': args.minimum_vendor_gpuva_maps,
                  'vendorResidencyOptIn': args.driver_residency, 'nativeVendorResidencyRequests': residency,
                  'minimumVendorResidencyRequestsRequired': args.minimum_vendor_residency_requests,
                  'vendorCpuOptIn': args.driver_cpu, 'nativeVendorCpuLocks': cpu,
                  'minimumVendorCpuLocksRequired': args.minimum_vendor_cpu_locks,
                  'guestCpuViewsUnmapped': cpu_unmaps, 'nativeVendorCpuLocksReleased': cpu_unlocks,
                  'cpuLocksRetainedUntilOwnedVmExit': cpu_released_after_exit,
                  'directGuestAllocationLoads': len(cpu) * 10000,
                  'diagnosticCpuStoresRequested': args.cpu_store_test,
                  'diagnosticCpuEofRequested': args.cpu_eof_test,
                  'diagnosticHardwareQueueEofRequested': args.hwqueue_eof_test,
                  'nativeCpuRegionChecks': [json.loads(line) for line in errors if line.startswith('{') and 'vendorCpuRegionCandidate' in line],
                  'directGuestFenceLoads': (total_fence_mappings + len(retirements)) * 10000,
                  'expectedInitializationBoundary': None if guest_eof else args.expected_unimplemented_ioctl,
                  'firstUnsupportedIoctlAfterAcceptedStage': first_unsupported_after_stage,
                  'unsupportedIoctls': unsupported, 'completedNativeQueries': completed,
                  'd3d12DeviceHresult': result[1] if result else None, 'ownedQemuControl': control, 'disconnectCleanup': cleanup,
                  'realWslDxgForwarding': False, 'capturedPrivateFixturesUsed': False,
                  'privateRuntimeImageRedistributable': False, 'kernelSha256': sha256(args.kernel),
                  'initramfsSha256': sha256(args.initramfs), 'driverBridgeSha256': sha256(args.bridge), 'qemuSha256': sha256(args.qemu),
                  'guestArbitraryGpuCommandSubmissionImplemented': False,
                  'boundedNativeCommandSubmissionVerified': bool(args.driver_submit and accepted and submissions),
                  'guestDesktopAcceleratedByThisBackend': False, 'nearNativePerformanceMeasured': False}
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
        args.report.with_suffix('.host.log').write_text(''.join(output + errors), encoding='utf-8', newline='\n')
        for line in log.splitlines():
            if any(marker in line for marker in ('factory=', 'list=', 'nvidia vendor=', 'device=', 'LINUX_BRIDGE', 'LINUX_RUNTIME', 'BRIDGE_RUNTIME')):
                print(line)
        print(json.dumps(report, indent=2))
        return 0 if accepted else 1
    finally:
        if child_handle:
            api.CloseHandle(child_handle)
        # Do not kill a native page owner here: the timeout path verifies
        # child exit first. An unreaped child deliberately retains its owner.


if __name__ == '__main__':
    raise SystemExit(main())
