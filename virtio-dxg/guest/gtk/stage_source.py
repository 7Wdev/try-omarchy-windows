"""Stage pinned GTK 4 sources as data and verify recipe hashes."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_version(root, expected_version):
    meson_text = (root / 'meson.build').read_text(encoding='utf-8')
    match = re.search(r"project\s*\([^)]*?\bversion\s*:\s*'([^']+)'", meson_text, re.S)
    if not match or match.group(1) != expected_version:
        raise ValueError('Wrong GTK source version')


def stage(archive, destination, recipe):
    manifest = json.loads((recipe / 'recipe.json').read_text(encoding='utf-8'))
    stage_cfg = manifest['profiles']['gtk4']
    if archive.stat().st_size > 128 * 1024 * 1024:
        raise ValueError('Source archive exceeds 128 MiB')
    if digest(archive) != manifest['sourceArchiveSha256']:
        raise ValueError('Source archive hash differs')
    patch = recipe / stage_cfg['patchFile']
    if digest(patch) != stage_cfg['patchSha256']:
        raise ValueError('GTK recipe patch hash differs')

    destination.mkdir(parents=True, exist_ok=False)
    version = manifest['gtkVersion']
    prefix = 'gtk-' + version
    with tarfile.open(archive, 'r:xz') as source:
        members = source.getmembers()
        if len(members) > 100000 or sum(m.size for m in members) > 1024 ** 3:
            raise ValueError('Expanded source exceeds limits')
        for member in members:
            parts = PurePosixPath(member.name).parts
            if (not parts or parts[0] != prefix or '\\' in member.name or
                    ':' in member.name or '..' in parts or member.size > 64 * 1024 ** 2 or
                    not (member.isfile() or member.isdir() or member.issym() or member.islnk())):
                raise ValueError('Unsupported source member: ' + member.name)
        source.extractall(destination, members=members, filter='data')

    root = destination / prefix
    check_version(root, version)
    for rel, record in stage_cfg['files'].items():
        path = root / rel
        if not path.is_file() or digest(path) != record['originalSha256']:
            raise ValueError('Original GTK source differs: ' + rel)
    command = ['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', str(patch)]
    subprocess.run(command + ['--dry-run'], cwd=root, check=True, timeout=30)
    subprocess.run(command, cwd=root, check=True, timeout=30)
    for rel, record in stage_cfg['files'].items():
        path = root / rel
        if not path.is_file() or digest(path) != record['patchedSha256']:
            raise ValueError('Patched GTK source differs: ' + rel)
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--recipe', type=Path,
                        default=HERE / 'versions' / (HERE / 'CURRENT').read_text().strip())
    args = parser.parse_args()
    print(stage(args.archive.resolve(strict=True), args.destination.resolve(),
                args.recipe.resolve(strict=True)))


if __name__ == '__main__':
    main()
