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
    args = parser.parse_args()
    if args.minimum_vendor_allocations < 0 or (args.minimum_vendor_allocations and not args.driver_allocations):
        parser.error('Minimum allocation acceptance requires the explicit allocation opt-in')
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
                             (['--driver-allocations'] if args.driver_allocations else []),
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
        cleanup = next((json.loads(line) for line in errors if line.startswith('{')), {})
        log = log_path.read_text(encoding='utf-8', errors='replace') if log_path.exists() else ''
        unsupported = sorted(set(int(n) for n in re.findall(r'LINUX_BRIDGE unsupported nr=(\d+)', log)))
        completed = [{'type': int(t), 'bytes': int(b)} for t, b in re.findall(r'LINUX_BRIDGE queryCompleted type=(\d+) bytes=(\d+)', log)]
        fences = [{'offset': int(o), 'value': int(v)} for o, v in re.findall(r'pagingFenceMapped=true direct=true loads=10000 offset=(\d+) value=(\d+)', log)]
        result = re.search(r'^device=([0-9a-f]{8})\s*$', log, re.MULTILINE)
        loaded = 'nvidiaUmdPresentDuringPrivateQuery=true' in log
        contexts = log.count('LINUX_BRIDGE nativeContextCreated=true')
        allocations = log.count('LINUX_BRIDGE nativeVendorAllocationCreated=true')
        accepted = (owner.returncode == 0 and loaded and contexts > 0 and len(fences) > 0 and
                    'LINUX_BRIDGE deviceCreated=true' in log and 'factory=00000000' in log and 'list=00000000' in log and
                    {'type': 0, 'bytes': 50616} in completed and
                    control.get('ownedQemuExited') is True and control.get('qemuExit') == 0 and
                    control.get('qemuForcedStop') is False and control.get('fenceControlFailed') is False and
                    control.get('liveFenceMappings') == 0 and control.get('fenceMappingsCreated') == len(fences) and
                    control.get('fenceUnmapAcknowledgements') == len(fences) and
                    cleanup.get('driverCleanupVerified') is True and cleanup.get('failedAdapterQueries') == 0 and
                    cleanup.get('completedAdapterQueries') == len(completed) and
                    'transport=virtio-port' in log and 'BRIDGE_RUNTIME_EXIT=1' in log and
                    args.expected_unimplemented_ioctl in unsupported and result is not None and int(result[1], 16) & 0x80000000)
        if args.driver_allocations:
            accepted = (accepted and allocations >= args.minimum_vendor_allocations and
                        cleanup.get('liveVendorAllocations') == 0 and cleanup.get('failedVendorAllocations') == 0 and
                        cleanup.get('completedVendorAllocations') == allocations and cleanup.get('destroyedVendorAllocations') == allocations and
                        6 not in unsupported and 19 not in unsupported)
        report = {'schema': 1, 'diagnosticAccepted': bool(accepted), 'runtimeInitializationComplete': False,
                  'stage': 'live NVIDIA runtime with Windows video-memory allocation' if args.driver_allocations else
                           'live NVIDIA runtime with dynamic Windows paging fence mappings',
                  'hypervisor': 'QEMU/WHPX', 'hostBridgeExit': owner.returncode,
                  'liveNvidiaLinuxUmdLoaded': loaded, 'nativeKmtDeviceCreatedByLiveRuntime': 'deviceCreated=true' in log,
                  'nativeKmtContextsCreatedByLiveRuntime': contexts, 'nativePagingFenceMappings': fences,
                  'vendorAllocationOptIn': args.driver_allocations, 'nativeVendorAllocationsCreatedByLiveRuntime': allocations,
                  'minimumVendorAllocationsRequired': args.minimum_vendor_allocations,
                  'directGuestFenceLoads': len(fences) * 10000, 'expectedInitializationBoundary': args.expected_unimplemented_ioctl,
                  'unsupportedIoctls': unsupported, 'completedNativeQueries': completed,
                  'd3d12DeviceHresult': result[1] if result else None, 'ownedQemuControl': control, 'disconnectCleanup': cleanup,
                  'realWslDxgForwarding': False, 'capturedPrivateFixturesUsed': False,
                  'privateRuntimeImageRedistributable': False, 'kernelSha256': sha256(args.kernel),
                  'initramfsSha256': sha256(args.initramfs), 'driverBridgeSha256': sha256(args.bridge), 'qemuSha256': sha256(args.qemu),
                  'guestArbitraryGpuCommandSubmissionImplemented': False,
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
