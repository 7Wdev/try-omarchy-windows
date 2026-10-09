#!/usr/bin/env python3
"""Empty compositor data for the four guest CLI queries used under Sway.

This is test state, not a Hyprland integration test. Unknown queries fail and
are recorded so expanding the shell's dependency surface needs review.
"""
import json
from pathlib import Path
import sys

QUERIES = {
    ('-j', 'clients'): [],
    ('-j', 'devices'): {'keyboards': []},
    ('-j', 'getoption', 'decoration:rounding'): {'int': 0},
    ('-j', 'getoption', 'general:gaps_out'): {'custom': '0 0 0 0'},
}


def main():
    args = tuple(sys.argv[1:])
    with Path('/results/hyprctl-fixture.log').open('a') as log:
        log.write(json.dumps(args) + '\n')
    if args not in QUERIES:
        print('Unsupported headless hyprctl query: ' + repr(args), file=sys.stderr)
        return 1
    print(json.dumps(QUERIES[args]))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
