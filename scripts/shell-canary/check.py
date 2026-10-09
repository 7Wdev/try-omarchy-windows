#!/usr/bin/env python3
"""Reject QML failures and new warnings; never allowlist QML runtime errors."""
import argparse
import json
from pathlib import Path
import re
from hyprctl_fixture import QUERIES

ALLOWLIST = Path(__file__).with_name('warnings.allowlist')
FATAL = re.compile(r'TypeError|ReferenceError|SyntaxError|is not a type|'
                   r'Cannot assign|Unable to assign|Cannot read property|'
                   r'Cannot read properties|Cannot call method|'
                   r'(?:failed|unable) to (?:create|load).*component|'
                   r'component.*(?:failed|not ready)|Error loading configuration|'
                   r'QQmlApplicationEngine failed|Segmentation fault|core dumped', re.I)
WARNING = re.compile(r'\b(?:WARN|ERROR|FATAL)\b|warning:|qml:.*(?:error|undefined)', re.I)


def errors(log, allowlist):
    log = re.sub(r'\x1b\[[0-9;]*m', '', log)
    patterns = [re.compile(line) for line in allowlist.splitlines()
                if line.strip() and not line.startswith('#')]
    return [line for line in log.splitlines() if FATAL.search(line) or
            (WARNING.search(line) and not any(p.search(line) for p in patterns))]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('results', type=Path)
    parser.add_argument('--summary', type=Path)
    args = parser.parse_args()
    args.results.mkdir(exist_ok=True)
    log_path = args.results / 'shell.log'
    failures = errors(log_path.read_text(errors='replace') if log_path.exists() else '', ALLOWLIST.read_text())
    probe_path = args.results / 'probe.json'
    probe = json.loads(probe_path.read_text()) if probe_path.exists() else {}
    fixture_log = args.results / 'hyprctl-fixture.log'
    if fixture_log.exists():
        for query in fixture_log.read_text().splitlines():
            if tuple(json.loads(query)) not in QUERIES:
                failures.append('Unsupported headless hyprctl query: ' + query)
    if not probe.get('success'):
        failures.append(probe.get('error', 'Shell probe did not finish; see setup.log'))
        setup_path = args.results / 'setup.log'
        if not probe and setup_path.exists():
            failures.extend(line for line in setup_path.read_text(errors='replace').splitlines()
                            if re.search(r'^(?:error:|fatal:)|Traceback|Canary probe failed:', line))
    if not log_path.exists() or 'Configuration Loaded' not in log_path.read_text(errors='replace'):
        failures.append('Quickshell did not report Configuration Loaded')
    (args.results / 'errors.txt').write_text('\n'.join(failures) + ('\n' if failures else ''))
    packages = args.results / 'packages.txt'
    versions = packages.read_text() if packages.exists() else 'Package installation did not finish.\n'
    summary = ('## Shell canary\n\n' + ('Failed' if failures else 'Passed') +
               f". Probe: {probe.get('seconds', 'unknown')} seconds.\n\n" +
               'Stages: ' + ', '.join(probe.get('stages', [])) + '\n\n' +
               '### First errors\n\n```text\n' + '\n'.join(failures[:20]) + '\n```\n\n' +
               '### Installed packages (current repositories, no lock)\n\n```text\n' + versions + '```\n')
    (args.results / 'summary.md').write_text(summary)
    if args.summary:
        with args.summary.open('a') as stream:
            stream.write(summary)
    print('\n'.join(failures[:20]) if failures else 'Shell canary passed')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
