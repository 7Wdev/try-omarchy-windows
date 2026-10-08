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
            send(op,handle,ntstatus=-1073741811 if mode == 'allocation-failed-destroy' and operations.count(op) == 1 else 0)
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
if mode.startswith('gpuva-'): assert operations == [0x2000], operations
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
                     'gpuva-disabled', 'gpuva-invalid'):
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
