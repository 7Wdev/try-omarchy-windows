"""Record the staged GTK 4 build without loading any installed ELF library."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re


def record(source, installed, recipe):
    manifest = json.loads((recipe / 'recipe.json').read_text(encoding='utf-8'))
    meson_text = (source / 'meson.build').read_text(encoding='utf-8')
    match = re.search(r"project\s*\([^)]*?\bversion\s*:\s*'([^']+)'", meson_text, re.S)
    version = match.group(1) if match else ''
    if version != manifest['gtkVersion']:
        raise ValueError('Cannot relabel a different GTK release')
    for name in manifest['runtimeRequiredPaths']:
        if not (installed / name).is_file():
            raise ValueError('Incomplete GTK cohort: ' + name)
    for name, target in manifest['runtimeAliases'].items():
        path = installed / name
        if not path.is_symlink() or os.readlink(path) != target:
            raise ValueError('Incorrect runtime alias: ' + name)
    inventory = {}
    for path in sorted(installed.rglob('*')):
        relative = path.relative_to(installed).as_posix()
        if relative.split('/', 1)[0] != 'lib':
            continue
        if relative in ('lib/virtio-dxg-gtk-build.json', 'lib/virtio-dxg-gtk-version'):
            continue
        if path.is_symlink():
            if not path.resolve(strict=True).is_relative_to(installed):
                raise ValueError('Installed link leaves the cohort: ' + relative)
            inventory[relative] = {'symlink': os.readlink(path)}
        elif path.is_file():
            inventory[relative] = {
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'bytes': path.stat().st_size,
            }
    receipt = {
        'schema': 1,
        'gtkVersion': version,
        'recipeSha256': hashlib.sha256((recipe / 'recipe.json').read_bytes()).hexdigest(),
        'sourceArchiveSha256': manifest['sourceArchiveSha256'],
        'profile': 'virtio-dxg-gtk',
        'files': inventory,
        'hardwareIntegrationTested': False,
        'dailyUseAccepted': False,
    }
    (installed / 'lib/virtio-dxg-gtk-build.json').write_text(
        json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    (installed / 'lib/virtio-dxg-gtk-version').write_text(
        version + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('installed', type=Path)
    parser.add_argument('recipe', type=Path)
    args = parser.parse_args()
    record(args.source.resolve(strict=True), args.installed.resolve(strict=True),
           args.recipe.resolve(strict=True))
