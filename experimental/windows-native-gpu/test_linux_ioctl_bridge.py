"""Exercise diagnostic transport failures without WSL or NVIDIA hardware."""
import argparse
import os
import pathlib
import subprocess
import tempfile


WORKER = r'''#!/usr/bin/env python3
import os, struct, sys
def get(n):
    data = sys.stdin.buffer.read(n)
    if len(data) != n:
        raise EOFError
    return data
def send(op, handle, value=0, data=b'', padding=0, hello=False, ntstatus=0):
    packet = struct.pack('<IIiI',op,handle,0,padding)
    caps = 103 | (128 if mode.startswith('paging-') and mode != 'paging-disabled' else 0)
    caps |= 256 if mode.startswith('allocation-') and mode != 'allocation-disabled' else 0
    caps |= 256 if mode.startswith('gpuva-') else 0
    caps |= 512 if mode == 'gpuva-invalid' else 0
    caps |= 1024 if mode == 'resident-invalid' else 0
    caps |= 256 if mode.startswith('cpu-') else 0
    caps |= 2048 if mode.startswith('cpu-') and mode != 'cpu-disabled' else 0
    caps |= 256 if mode.startswith('translation-') else 0
    caps |= 4096 if mode.startswith('translation-') and mode != 'translation-disabled' else 0
    caps |= 8192 if mode.startswith('hwqueue-') and mode != 'hwqueue-disabled' else 0
    caps |= 16384 if mode.startswith('sync-') and mode != 'sync-disabled' else 0
    caps |= 32768 if mode.startswith('submit-') and mode != 'submit-disabled' else 0
    packet += struct.pack('<IIII',1,caps,4318,11352) if hello else struct.pack('<iIQ',ntstatus,0,value) + data
    sys.stdout.buffer.write(struct.pack('<I',len(packet)) + packet)
    sys.stdout.buffer.flush()
mode = os.environ['BRIDGE_FAULT_TEST']
operations = []
try:
    while True:
        size, = struct.unpack('<I',get(4))
        packet = get(size)
        op, handle, status, padding = struct.unpack_from('<IIiI',packet)
        assert not status and not padding
        operations.append(op)
        if op == 0x2000:
            if mode == 'wrong-version':
                raw = struct.pack('<IIiIIIII',op,0,0,0,99,103,4318,11352)
                sys.stdout.buffer.write(struct.pack('<I',len(raw)) + raw); sys.stdout.buffer.flush()
            else:
                send(op,0,hello=True)
            if mode == 'closed-worker': break
        elif mode == 'bad-reply': send(op,1,padding=1)
        elif op == 0x2001: send(op,1)
        elif op == 0x2003: send(op,handle)
        elif op == 0x2004: send(op,2)
        elif op == 0x2005: send(op,handle)
        elif op == 0x2070:
            mutex = mode in ('sync-mutex', 'sync-failed-destroy', 'sync-mutex-bad-map') or mode.startswith('sync-destroy-')
            assert handle == 2 and packet[16:] == struct.pack('<IIIIQ',1 if mutex else 5,0,0 if mutex else 1,0,1 if mutex else 42)
            failed = mode in ('sync-nt-failure','sync-bad-failure')
            identity = 0 if failed or mode == 'sync-bad-id' else 3
            offset = 0 if failed or mutex else 1 if mode == 'sync-bad-alignment' else 262144 if mode == 'sync-bad-offset' else 8192
            gpu = 0 if failed or mutex or mode == 'sync-zero-gpu' else 65537 if mode == 'sync-bad-gpu-alignment' else 1 << 48 if mode == 'sync-bad-gpu-range' else 65536
            if mode in ('sync-bad-failure', 'sync-mutex-bad-map'): offset = 8192
            data = struct.pack('<QQ',offset,gpu)
            if mode == 'sync-short': data = data[:-1]
            send(op,identity,value=1 if mode == 'sync-bad-value' else 0,data=data,
                 ntstatus=-1073741811 if failed else 259 if mode == 'sync-bad-status' else 0)
        elif op == 0x2071:
            assert handle == 3 and len(packet) == 16
            failed = mode == 'sync-failed-destroy' and operations.count(op) == 1
            send(op,0 if mode == 'sync-destroy-bad-id' else handle,
                 value=1 if mode == 'sync-destroy-bad-value' else 0,
                 data=b'X' if mode == 'sync-destroy-long' else b'',
                 ntstatus=-1073741811 if failed else 259 if mode == 'sync-destroy-bad-status' else 0)
        elif op == 0x2020:
            assert handle == 2
            desc = struct.unpack_from('<6I',packet,16)
            assert desc == ((0,0,8,0,0,0) if mode == 'hwqueue-sync-context' else (0,1,16,12,4,0))
            send(op,3,data=b'' if mode == 'hwqueue-sync-context' else bytes([37 ^ 255,38,39,40]))
        elif op == 0x2060:
            assert handle == 3 and packet[16:32] == struct.pack('<4I',0,4,0,0)
            assert packet[32:] == bytes([37,38,39,40])
            failed = mode in ('hwqueue-nt-failure','hwqueue-bad-failure')
            queue = 0 if failed or mode == 'hwqueue-bad-id' else 4
            sync = 0 if failed or mode == 'hwqueue-bad-sync' else 4 if mode == 'hwqueue-same-sync' else 5
            offset = 0 if failed else 1 if mode == 'hwqueue-bad-alignment' else 262144 if mode == 'hwqueue-bad-offset' else 8192
            gpu = 0 if failed or mode == 'hwqueue-zero-gpu' else 65537 if mode == 'hwqueue-bad-gpu-alignment' else 1 << 48 if mode == 'hwqueue-bad-gpu-range' else 65536
            if mode == 'hwqueue-bad-failure': sync = 5
            data = struct.pack('<IIQQ',sync,1 if mode == 'hwqueue-bad-reserved' else 0,offset,gpu) + bytes([37 ^ 255,38,39,40])
            if mode == 'hwqueue-short': data = data[:-1]
            send(op,queue,value=1 if mode == 'hwqueue-bad-value' else 0,data=data,
                 ntstatus=-1073741811 if failed else 259 if mode == 'hwqueue-bad-status' else 0)
        elif op == 0x2061:
            assert handle == 4 and len(packet) == 16
            send(op,handle)
        elif op == 0x2040:
            sync = 0 if mode == 'paging-bad-sync' else 4
            offset = 262144 if mode == 'paging-bad-offset' else 0
            reserved = 1 if mode == 'paging-bad-reserved' else 0
            data = struct.pack('<IIQ',sync,reserved,offset)
            if mode == 'paging-short': data = data[:-1]
            send(op,3,value=1 if mode == 'paging-bad-value' else 0,data=data)
        elif op == 0x2008: send(op,handle)
        elif op == 0x2050:
            assert handle == 2 and struct.unpack_from('<6I',packet,16) == (4,0x78100000,0,4,0,0)
            assert packet[40:] == bytes([37,38,39,40])
            data = bytes([37 ^ 255,38,39,40])
            if mode == 'allocation-short': data = data[:-1]
            send(op,0 if mode in ('allocation-bad-id','allocation-nt-failure') else 3,
                 value=1 if mode == 'allocation-bad-va' else 0,data=data,
                 ntstatus=-1073741811 if mode == 'allocation-nt-failure' else 0)
        elif op == 0x2051:
            assert handle == 2 and packet[16:] == struct.pack('<III',1,0,3)
            send(op,0 if mode == 'allocation-destroy-bad-id' else handle,
                 value=1 if mode == 'allocation-destroy-bad-value' else 0,
                 data=b'X' if mode == 'allocation-destroy-long' else b'',
                 ntstatus=-1073741811 if (mode == 'allocation-failed-destroy' and operations.count(op) == 1) or mode == 'translation-failed-cleanup' else
                          259 if mode == 'allocation-destroy-bad-status' else 0)
        elif op == 0x2056:
            assert handle == 3 and packet[16:] == struct.pack('<IIII',2,1,0,0)
            failed = mode in ('translation-nt-failure','translation-bad-failure','translation-failed-cleanup')
            token = 0 if failed or mode == 'translation-zero' else 1 << 32 if mode == 'translation-overflow' else 0x10000003
            if mode == 'translation-bad-failure': token = 1
            send(op,0 if mode == 'translation-bad-id' else handle,value=token,
                 data=b'X' if mode == 'translation-long' else b'',
                 ntstatus=-1073741811 if failed else 259 if mode == 'translation-bad-status' else 0)
        elif op == 0x2054:
            assert handle == 3 and packet[16:] == struct.pack('<II',2,0)
            native_failure = mode in ('cpu-nt-failure','cpu-bad-failure')
            size = 0 if native_failure or mode == 'cpu-bad-bytes' else 65537 if mode == 'cpu-bad-alignment' else 65536
            reserved = 1 if mode == 'cpu-bad-reserved' else 0
            generation = 0 if native_failure or mode == 'cpu-bad-generation' else 1
            offset = 67108864 if mode == 'cpu-bad-offset' else 1 if mode == 'cpu-bad-slot' else 0
            if mode == 'cpu-bad-failure': size = 4096
            data = struct.pack('<IIQ',size,reserved,generation)
            if mode == 'cpu-short': data = data[:-1]
            send(op,0 if mode == 'cpu-bad-id' else handle,value=offset,data=data,
                 ntstatus=-1073741811 if native_failure else 259 if mode == 'cpu-bad-status' else 0)
        elif op in (0x2030,0x2031,0x2032): send(op,handle,4)
        elif op == 0x2033: send(op,handle,4,struct.pack('<I',0x231b))
        elif op == 0x2034: send(op,handle)
        else: raise AssertionError('Unexpected host operation')
except EOFError:
    pass
if mode == 'reuse': assert operations == [0x2000], operations
if mode in ('paging-disabled','paging-invalid'): assert 0x2040 not in operations, operations
if mode == 'paging-no-hub': assert operations[-2:] == [0x2040,0x2008], operations
if mode in ('allocation-disabled','allocation-invalid'): assert 0x2050 not in operations, operations
if mode == 'allocation-normal': assert operations.count(0x2051) == 1, operations
if mode == 'allocation-failed-destroy': assert operations.count(0x2051) == 2, operations
if mode.startswith('allocation-destroy-'): assert operations.count(0x2051) == 1, operations
if mode.startswith('gpuva-'): assert operations == [0x2000], operations
if mode.startswith('resident-'): assert operations == [0x2000], operations
if mode in ('cpu-disabled','cpu-invalid'): assert 0x2054 not in operations, operations
if mode.startswith('cpu-'): assert 0x2055 not in operations, operations
if mode == 'translation-disabled': assert 0x2056 not in operations, operations
if mode in ('translation-normal','translation-invalid','translation-disabled','translation-nt-failure','translation-failed-cleanup'):
    assert operations.count(0x2051) == 1, operations
if mode.startswith('translation-'): assert 0x2016 not in operations, operations
if mode in ('hwqueue-disabled','hwqueue-invalid','hwqueue-sync-context'): assert 0x2060 not in operations, operations
if mode == 'hwqueue-no-hub': assert operations[-2:] == [0x2060,0x2061], operations
if mode in ('sync-disabled','sync-invalid'): assert 0x2070 not in operations and 0x2071 not in operations, operations
if mode == 'sync-no-hub': assert operations[-2:] == [0x2070,0x2071], operations
if mode == 'sync-mutex' or mode.startswith('sync-destroy-'): assert operations.count(0x2071) == 1, operations
if mode == 'sync-failed-destroy': assert operations.count(0x2071) == 2, operations
if mode.startswith('submit-'): assert 0x2062 not in operations, operations
print('FAKE_WORKER_EOF=true',file=sys.stderr)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shim', required=True, type=pathlib.Path)
    parser.add_argument('--probe', required=True, type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wddm-ioctl-fault-') as directory:
        worker = pathlib.Path(directory) / 'owned-worker'
        worker.write_text(WORKER, encoding='utf-8'); worker.chmod(0o700)
        for mode in ('no-worker', 'wrong-version', 'reuse', 'bad-reply', 'closed-worker', 'normal',
                     'paging-disabled', 'paging-invalid', 'paging-bad-sync', 'paging-bad-offset',
                     'paging-bad-reserved', 'paging-bad-value', 'paging-short', 'paging-no-hub',
                     'allocation-normal', 'allocation-disabled', 'allocation-invalid', 'allocation-nt-failure',
                     'allocation-bad-id', 'allocation-bad-va', 'allocation-short', 'allocation-failed-destroy',
                     'allocation-destroy-bad-id', 'allocation-destroy-bad-status', 'allocation-destroy-bad-value', 'allocation-destroy-long',
                     'gpuva-disabled', 'gpuva-invalid', 'resident-disabled', 'resident-invalid',
                     'cpu-disabled', 'cpu-invalid', 'cpu-nt-failure', 'cpu-bad-failure', 'cpu-bad-id',
                     'cpu-bad-bytes', 'cpu-bad-alignment', 'cpu-bad-reserved', 'cpu-bad-generation',
                     'cpu-bad-offset', 'cpu-bad-slot', 'cpu-short', 'cpu-bad-status', 'cpu-no-hub',
                     'translation-normal', 'translation-invalid', 'translation-disabled', 'translation-nt-failure',
                     'translation-failed-cleanup', 'translation-zero', 'translation-overflow', 'translation-bad-failure',
                     'translation-bad-status', 'translation-bad-id', 'translation-long',
                     'hwqueue-disabled', 'hwqueue-invalid', 'hwqueue-sync-context', 'hwqueue-nt-failure', 'hwqueue-bad-failure',
                     'hwqueue-bad-id', 'hwqueue-bad-sync', 'hwqueue-same-sync', 'hwqueue-bad-alignment', 'hwqueue-bad-offset',
                     'hwqueue-zero-gpu', 'hwqueue-bad-gpu-alignment', 'hwqueue-bad-gpu-range', 'hwqueue-bad-reserved',
                     'hwqueue-short', 'hwqueue-bad-value', 'hwqueue-bad-status', 'hwqueue-no-hub',
                     'sync-mutex', 'sync-failed-destroy', 'sync-disabled', 'sync-invalid', 'sync-nt-failure', 'sync-bad-failure',
                     'sync-bad-id', 'sync-bad-alignment', 'sync-bad-offset', 'sync-zero-gpu', 'sync-bad-gpu-alignment',
                     'sync-bad-gpu-range', 'sync-short', 'sync-bad-value', 'sync-bad-status', 'sync-no-hub', 'sync-mutex-bad-map',
                     'sync-destroy-bad-id', 'sync-destroy-bad-value', 'sync-destroy-long', 'sync-destroy-bad-status',
                     'submit-disabled', 'submit-invalid', 'submit-unowned', 'submit-ignored-pointer'):
            environment = {k: v for k, v in os.environ.items() if not k.startswith('WDDM_BRIDGE_') and k != 'LD_PRELOAD'}
            environment['LD_PRELOAD'] = str(args.shim.resolve())
            environment['BRIDGE_FAULT_TEST'] = mode
            if mode != 'no-worker': environment['WDDM_BRIDGE_WINDOWS_WORKER'] = str(worker)
            result = subprocess.run([str(args.probe.resolve()), mode], env=environment,
                                    capture_output=True, text=True, timeout=20)
            if result.returncode or 'realDxgForwarding=true' in result.stderr:
                raise RuntimeError(f'{mode} failed ({result.returncode}): {result.stderr}')
            if mode != 'no-worker' and ('workerReaped=true exit=0' not in result.stderr or 'FAKE_WORKER_EOF=true' not in result.stderr):
                raise RuntimeError(f'{mode}: owned worker did not terminate cleanly: {result.stderr}')
            print('PASS: Linux ioctl diagnostic ' + mode)


if __name__ == '__main__':
    main()
