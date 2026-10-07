"""Exercise real WDDM control calls from a disk-free QEMU Linux guest on Windows."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'kernel', 'initramfs', 'bridge', 'report'):
        parser.add_argument(f'--{name}', type=pathlib.Path, required=True)
    args = parser.parse_args()
    for path in (args.qemu, args.kernel, args.initramfs, args.bridge):
        if not path.is_file():
            parser.error(f'Missing file: {path}')
    if sys.platform != 'win32':
        parser.error('Run this harness on the Windows NVIDIA host')
    hidden = subprocess.CREATE_NO_WINDOW
    helper = subprocess.Popen([str(args.bridge.resolve()), '--listen', '0'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                              creationflags=hidden)
    qemu = None
    try:
        ready = json.loads(helper.stdout.readline())
        port = ready['port']
        if ready['transport'] != 'tcp-loopback' or not isinstance(port, int) or not 0 < port < 65536:
            raise RuntimeError('Invalid bridge endpoint')
        command = [str(args.qemu.resolve()), '-machine', 'q35,accel=whpx', '-cpu', 'host',
                   '-m', '512', '-smp', '1', '-display', 'none', '-monitor', 'none',
                   '-serial', 'stdio', '-nodefaults', '-no-reboot',
                   '-kernel', str(args.kernel.resolve()), '-initrd', str(args.initramfs.resolve()),
                   '-append', 'console=ttyS0 rdinit=/init panic=1',
                   '-device', 'virtio-serial-pci,id=bridge-serial',
                   '-chardev', f'socket,id=wddm,host=127.0.0.1,port={port}',
                   '-device', 'virtserialport,bus=bridge-serial.0,chardev=wddm,name=org.7wdev.wddm']
        # No disk, NIC, host share, or existing VM process is attached.
        qemu = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors='replace', creationflags=hidden)
        log, _ = qemu.communicate(timeout=60)
        _, helper_errors = helper.communicate(timeout=10)
        cleanup = json.loads(helper_errors.strip()) if helper.returncode == 0 else {}
        identity = re.search(r'BRIDGE_VENDOR=(\d+) BRIDGE_DEVICE=(\d+)', log)
        allocation_checks = re.findall(r'BRIDGE_ALLOCATION bytes=65536 cpuRoundtrip=true gpuVaMapped=true residency=[12]', log)
        success = (qemu.returncode == 0 and helper.returncode == 0 and
                   'PASS: QEMU guest WDDM allocation bridge, 5 lifecycle cycles' in log and
                   'BRIDGE_GUEST_EXIT=0' in log and identity is not None and
                   len(allocation_checks) == 6 and
                   cleanup.get('driverCleanupVerified') is True)
        log_path = args.report.with_suffix('.log')
        log_path.write_text(log + '\nHOST BRIDGE:\n' + helper_errors, encoding='utf-8', newline='\n')
        report = {'schema': 1, 'success': success, 'hypervisor': 'QEMU/WHPX',
                  'transport': 'virtio-serial to loopback Windows worker', 'lifecycleCycles': 5 if success else 0,
                  'qemuExit': qemu.returncode, 'hostBridgeExit': helper.returncode,
                  'vendorId': int(identity[1]) if identity else None,
                  'deviceId': int(identity[2]) if identity else None,
                  'disconnectCleanup': cleanup,
                  'kernelSha256': sha256(args.kernel), 'initramfsSha256': sha256(args.initramfs),
                  'driverBridgeSha256': sha256(args.bridge),
                  'allocationCycles': len(allocation_checks), 'allocationBytesPerCycle': 65536,
                  'cpuRoundtripBytes': len(allocation_checks) * 65536,
                  'hostBackedAllocationOperationsImplemented': True,
                  'sharedGuestRamMappingImplemented': False, 'guestGpuSubmissionImplemented': False,
                  'guestDesktopAcceleratedByThisBackend': False}
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
        for line in log.splitlines():
            if 'WDDM enum=' in line or 'PASS:' in line or 'FAIL:' in line or 'BRIDGE_' in line:
                print(line)
        print(json.dumps(report, indent=2))
        return 0 if success else 1
    finally:
        for process in (qemu, helper):
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate(timeout=10)


if __name__ == '__main__':
    raise SystemExit(main())
