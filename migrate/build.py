#!/usr/bin/env python3
"""Build the importer's release files.

    migrate/build.py --version v0.7.0 --output DIR

writes DIR/try-omarchy-import.pyz (a zip application with a python3
shebang) and DIR/try-omarchy-import.sh (the bootstrap that checks the .pyz
against the digest recorded in it). The .pyz is reproducible: the same
sources and version give the same bytes.
"""

import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import zipfile

HERE = Path(__file__).resolve().parent
PACKAGE = HERE / "omarchy_import"
TEMPLATE = HERE / "try-omarchy-import.sh.in"
TIMESTAMP = (2026, 1, 1, 0, 0, 0)
VERSION_PATTERN = re.compile(r"v?[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?|dev")


def _add(archive, name, data):
    info = zipfile.ZipInfo(name, TIMESTAMP)
    info.external_attr = 0o644 << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, data)


def build_pyz(version):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for path in sorted(PACKAGE.glob("*.py")):
            data = path.read_bytes()
            if path.name == "__init__.py":
                stamped = data.replace(b'VERSION = "dev"', f'VERSION = "{version}"'.encode())
                if stamped == data and version != "dev":
                    raise SystemExit("could not stamp the version into __init__.py")
                data = stamped
            _add(archive, f"omarchy_import/{path.name}", data)
        _add(archive, "__main__.py",
             b"import sys\n\nfrom omarchy_import.cli import main\n\nsys.exit(main())\n")
    return b"#!/usr/bin/env python3\n" + buffer.getvalue()


def build_script(version, digest):
    text = TEMPLATE.read_text(encoding="utf-8")
    text = text.replace("@VERSION@", version).replace("@SHA256@", digest)
    if "@" in text.replace("${arguments[@]}", "").replace("$@", ""):
        raise SystemExit("unreplaced placeholder in the bootstrap script")
    return text.encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True, help="release tag, for example v0.7.0")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not VERSION_PATTERN.fullmatch(args.version):
        raise SystemExit(f"invalid version {args.version}")
    args.output.mkdir(parents=True, exist_ok=True)
    pyz = build_pyz(args.version)
    digest = hashlib.sha256(pyz).hexdigest()
    for name, data in (("try-omarchy-import.pyz", pyz),
                       ("try-omarchy-import.sh", build_script(args.version, digest))):
        path = args.output / name
        path.write_bytes(data)
        os.chmod(path, 0o755)
        print(f"{hashlib.sha256(data).hexdigest()}  {name}")


if __name__ == "__main__":
    main()
