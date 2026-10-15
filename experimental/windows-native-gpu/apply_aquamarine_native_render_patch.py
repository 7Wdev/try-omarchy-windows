"""Apply the pinned, opt-in Aquamarine prototype after checking every source file."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess


def apply(source):
    base=Path(__file__).resolve().parent
    lock=json.loads((base/'aquamarine-native-render.lock.json').read_text())
    patch=base/lock['patch']
    digest=lambda data:hashlib.sha256(data).hexdigest()
    if lock['schema']!=1 or digest(patch.read_bytes())!=lock['patchSha256']:
        raise ValueError('Aquamarine patch manifest/digest mismatch')
    if (source/'VERSION').read_text().strip()!='0.15.1':
        raise ValueError('Aquamarine 0.15.1 source required')
    before=True; after=True
    for name,pins in lock['files'].items():
        path=PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name:
            raise ValueError('Unsafe source path')
        target=(source/name).resolve()
        if not target.is_relative_to(source):
            raise ValueError('Source path escaped checkout')
        actual=digest(target.read_bytes()) if target.exists() else None
        before &= actual==pins['beforeSha256']
        after &= actual==pins['afterSha256']
    if after:
        print('Aquamarine native-render patch already applied; exact hashes verified')
        return
    if not before:
        raise ValueError('Modified or partially patched Aquamarine source; no files changed')
    subprocess.run(['git','apply','--check',str(patch)],cwd=source,check=True)
    subprocess.run(['git','apply',str(patch)],cwd=source,check=True)
    for name,pins in lock['files'].items():
        if digest((source/name).read_bytes())!=pins['afterSha256']:
            raise ValueError('Patched Aquamarine source digest mismatch')
    print('Aquamarine native-render patch applied; exact before/after hashes verified')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    args=parser.parse_args()
    apply(args.source.resolve(strict=True))
