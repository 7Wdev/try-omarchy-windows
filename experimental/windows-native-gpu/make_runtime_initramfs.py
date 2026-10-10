"""Pack installed Linux GPU libraries into a PRIVATE local QEMU diagnostic.

This image includes proprietary NVIDIA binaries. Never commit or upload it.
It does not install drivers, read a guest disk, or create a graphics desktop.
"""
import argparse
import gzip
import hashlib
import json
import os
import pathlib
import re
import stat
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('init', 'probe', 'shim', 'linux-umd', 'output'):
        parser.add_argument('--' + name, required=True, type=pathlib.Path)
    for name in ('d3d12', 'd3d12core', 'dxcore'):
        parser.add_argument('--' + name, type=pathlib.Path, default=pathlib.Path('/usr/lib/wsl/lib/lib' + name + '.so'))
    parser.add_argument('--strace', type=pathlib.Path, help='Optional locally supplied syscall tracer; failed calls only')
    parser.add_argument('--dependency-directory', type=pathlib.Path, action='append', default=[], help='Optional local ELF dependencies; mapped to the guest library directory')
    parser.add_argument('--extra-runtime', type=pathlib.Path, action='append', default=[], help='Installed library loaded dynamically; preserves its absolute guest path')
    parser.add_argument('--runtime-workload', choices=('init', 'copy', 'clear', 'triangle', 'shared', 'consume', 'native'), default='init', help='Explicit guest D3D12 workload; consume hands completed shared textures to a native GPU consumer')
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('Run in Linux/WSL with locally installed runtime libraries')
    if args.output.exists() or args.output.with_suffix('.manifest.json').exists():
        parser.error('Use a fresh output path; private runtime images are never overwritten')
    module = args.linux_umd.name
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,128}', module):
        parser.error('Invalid Linux UMD module basename')
    files = {'init': args.init, 'guest-probe': args.probe, 'linux-ioctl-bridge.so': args.shim}
    roots = [args.probe, args.shim, args.linux_umd, args.d3d12, args.d3d12core, args.dxcore]
    roots.extend(args.extra_runtime)
    if args.strace:
        files['runtime-tracer'] = args.strace
        roots.append(args.strace)
    for path in [args.init] + roots:
        data = path.read_bytes()
        if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\x3e\x00':
            parser.error(f'{path} must be a little-endian x64 Linux ELF file')
    # Preserve installed runtime paths because DXCore resolves DriverStore
    # filenames to /usr/lib/wsl/drivers; preserve the ELF interpreter path too.
    for path in [args.linux_umd, args.d3d12, args.d3d12core, args.dxcore] + args.extra_runtime:
        if not path.is_absolute():
            parser.error('Installed runtime paths must be absolute')
        files[path.as_posix().lstrip('/')] = path
    environment = dict(os.environ)
    dependency_directories = [p.resolve(strict=True) for p in args.dependency_directory]
    if any(not p.is_dir() for p in dependency_directories):
        parser.error('Dependency directories must exist')
    if dependency_directories:
        environment['LD_LIBRARY_PATH'] = ':'.join(str(p) for p in dependency_directories) + ':' + environment.get('LD_LIBRARY_PATH', '')
    for path in roots:
        result = subprocess.run(['ldd', str(path.resolve())], capture_output=True, text=True, timeout=10, check=True, env=environment)
        if 'not found' in result.stdout:
            parser.error(f'Unresolved dependency of {path}')
        for line in result.stdout.splitlines():
            match = re.search(r'(?:=>\s+)?(/[^\s]+)\s+\(0x[0-9a-f]+\)', line)
            if match:
                dependency = pathlib.Path(match[1])
                name = dependency.as_posix().lstrip('/')
                if any(dependency.resolve().is_relative_to(p) for p in dependency_directories):
                    name = 'usr/lib/x86_64-linux-gnu/' + dependency.name
                if name in files and files[name].resolve() != dependency.resolve():
                    parser.error(f'Conflicting guest dependency: {name}')
                files[name] = dependency
    archive = bytearray()
    inode = 0

    def entry(name, mode, data=b'', major=0, minor=0):
        nonlocal inode
        if name.startswith('/') or '..' in pathlib.PurePosixPath(name).parts or '\0' in name:
            raise ValueError('Unsafe archive path')
        inode += 1
        encoded = name.encode() + b'\0'
        fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0]
        archive.extend(b'070701' + ''.join(f'{f:08x}' for f in fields).encode())
        archive.extend(encoded); archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data); archive.extend(b'\0' * (-len(archive) % 4))
        if len(archive) > 256 * 1024 * 1024:
            raise ValueError('Private runtime image exceeds 256 MiB bound')

    directories = {'dev', 'proc', 'sys', 'tmp'}
    for name in files:
        directories.update(str(p) for p in pathlib.PurePosixPath(name).parents if str(p) != '.')
    for directory in sorted(directories, key=lambda p: (p.count('/'), p)):
        entry(directory, stat.S_IFDIR | (0o1777 if directory == 'tmp' else 0o755))
    entry('dev/console', stat.S_IFCHR | 0o600, major=5, minor=1)
    manifest = {'schema': 1, 'privateImage': True, 'vendorBinariesIncluded': True,
                'redistributionApproved': False, 'runtimeWorkload': args.runtime_workload, 'files': []}
    for name, path in sorted(files.items()):
        data = path.read_bytes()
        entry(name, stat.S_IFREG | 0o755, data)
        manifest['files'].append({'path': '/' + name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    entry('linux-umd-name', stat.S_IFREG | 0o600, module.encode())
    entry('runtime-workload', stat.S_IFREG | 0o600, args.runtime_workload.encode())
    entry('TRAILER!!!', 0)
    packed = gzip.compress(archive, mtime=0)
    with args.output.open('xb') as destination:
        destination.write(packed)
    manifest['initramfsSha256'] = hashlib.sha256(packed).hexdigest()
    manifest['uncompressedBytes'] = len(archive)
    manifest['compressedBytes'] = len(packed)
    with args.output.with_suffix('.manifest.json').open('x', encoding='utf-8') as destination:
        json.dump(manifest, destination, indent=2); destination.write('\n')
    print(f'Created PRIVATE {args.output}: {len(packed)} bytes; never upload this image')


if __name__ == '__main__':
    main()
