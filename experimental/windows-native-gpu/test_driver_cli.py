"""Reject malformed aperture settings before native device or VM startup."""
import argparse
import pathlib
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bridge', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'win32' or not args.bridge.is_file():
        parser.error('Run on Windows with the built bridge')
    cases = [(['--driver-cpu-slots', value], 'CPU aperture slots must be specified once')
             for value in ('0', '8', '17', '33', '65', '-1', '4294967296', '32x')]
    cases += [(['--driver-cpu-slots', '16', '--driver-cpu-slots', '32'], 'specified once'),
              (['--stdio', '--driver-cpu-slots', '32'], 'capacity requires')]
    for options, message in cases:
        result = subprocess.run([str(args.bridge.resolve()), *options], capture_output=True, text=True,
                                timeout=5, creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode != 1 or message not in result.stderr or result.stdout:
            raise RuntimeError(f'Invalid capacity reached startup: {options}: {result.returncode}')
    print(f'PASS: {len(cases)} native capacity rejection cases before runtime startup')


if __name__ == '__main__':
    main()
