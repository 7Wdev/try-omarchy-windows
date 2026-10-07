"""Add the Windows section backend to the exact pinned QEMU lab source."""
import argparse
import json
import pathlib
import shutil
import subprocess


def prepare(source):
    recipe = pathlib.Path(__file__).resolve().parent
    root = recipe.parents[1]
    lock = json.loads((root / 'runtime-build/sources.lock.json').read_text())
    pinned = lock['qemu']['commit']
    head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if head != pinned:
        raise RuntimeError(f'Expected QEMU {pinned}, got {head}')
    if subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip():
        raise RuntimeError('QEMU checkout must be clean')
    patch = recipe / 'external-shared-ram.patch'
    subprocess.run(['git', '-C', str(source), 'apply', '--check', str(patch)], check=True)
    subprocess.run(['git', '-C', str(source), 'apply', str(patch)], check=True)
    shutil.copyfile(recipe / 'hostmem-win32-section.c', source / 'backends/hostmem-win32-section.c')
    meson = source / 'backends/meson.build'
    original = meson.read_text()
    marker = "if host_os != 'windows'\n"
    if original.count(marker) != 1:
        raise RuntimeError('Pinned QEMU backend build layout changed')
    addition = "if host_os == 'windows'\n  system_ss.add(files('hostmem-win32-section.c'))\nendif\n"
    meson.write_text(original.replace(marker, addition + marker), newline='\n')
    print(f'Prepared QEMU {pinned} with experimental Windows section memory')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=pathlib.Path)
    prepare(parser.parse_args().source.resolve())
