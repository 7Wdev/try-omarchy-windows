#!/usr/bin/env python3
"""Read the reviewed announcement for a release."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("tag")
    parser.add_argument("--notes-dir", type=Path, default=Path(".github/release-notes"))
    args = parser.parse_args()

    if Path(args.tag).name != args.tag:
        raise SystemExit("tag must not contain a path")
    notes = args.notes_dir / f"{args.tag}.md"
    if not notes.is_file():
        raise SystemExit(
            f"Missing reviewed release notes: {notes}. "
            "Write the release announcement before publishing."
        )
    body = notes.read_text(encoding="utf-8").strip()
    if not body:
        raise SystemExit(f"{notes} is empty")
    print(body)


if __name__ == "__main__":
    main()
