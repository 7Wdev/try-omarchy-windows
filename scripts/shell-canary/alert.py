#!/usr/bin/env python3
"""Maintain one canary issue. Only the workflow's master alert job runs this."""
import json
import os
from pathlib import Path
import sys
import urllib.error
import urllib.request

LABEL = 'shell-canary'
MARKER = '<!-- shell-canary -->'
TITLE = 'Guest shell canary failed with current packages'


def api(method, path, data=None):
    url = os.environ.get('GITHUB_API_URL', 'https://api.github.com') + '/repos/' + os.environ['GITHUB_REPOSITORY'] + path
    request = urllib.request.Request(url, method=method,
        data=json.dumps(data).encode() if data is not None else None,
        headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                 'Accept': 'application/vnd.github+json', 'Content-Type': 'application/json',
                 'X-GitHub-Api-Version': '2022-11-28'})
    with urllib.request.urlopen(request, timeout=20) as response:
        body = response.read()
        return json.loads(body) if body else None


def update(result, summary, run_url, request=api):
    # Bounded pagination includes closed issues so the next failure reopens the
    # same issue. A full last page is an error rather than risking a duplicate.
    issues = []
    for page in range(1, 6):
        batch = request('GET', f'/issues?state=all&labels={LABEL}&per_page=100&page={page}')
        issues.extend(i for i in batch if 'pull_request' not in i and MARKER in (i.get('body') or ''))
        if len(batch) < 100:
            break
    else:
        raise RuntimeError('Canary issue lookup exceeded five pages; refusing to create a duplicate')
    if len(issues) > 1:
        raise RuntimeError('Multiple canary issues found; refusing to create another')
    issue = issues[0] if issues else None
    if result == 'success':
        if issue and issue['state'] == 'open':
            request('POST', f"/issues/{issue['number']}/comments",
                    {'body': f'The guest shell loads with current packages again. [Passing run]({run_url}).'})
            request('PATCH', f"/issues/{issue['number']}", {'state': 'closed', 'state_reason': 'completed'})
        return
    if result != 'failure':
        raise ValueError(f'Unexpected probe result: {result}')
    body = (MARKER + '\nThe pinned guest shell failed its current-package check.\n\n' +
            f'[Workflow run and full logs]({run_url})\n\n' + summary +
            '\nReproduce with Shell canary, review the package change and backport a narrow fix through guest-build. '
            'Do not add QML errors to the warning allowlist.\n')
    if issue:
        request('PATCH', f"/issues/{issue['number']}",
                {'title': TITLE, 'body': body, 'state': 'open'})
    else:
        try:
            request('GET', '/labels/' + LABEL)
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise
            request('POST', '/labels', {'name': LABEL, 'color': 'd73a4a',
                    'description': 'Frozen guest shell compatibility with current packages'})
        request('POST', '/issues', {'title': TITLE, 'body': body, 'labels': [LABEL]})


def main():
    results = Path(sys.argv[1])
    path = results / 'summary.md'
    if path.exists():
        # Keep issues readable; the artifact and job summary retain all packages.
        packages = results / 'packages.txt'
        versions = [line for line in packages.read_text().splitlines()
                    if line.startswith(('qt6-', 'quickshell ', 'sway ', 'mesa '))] if packages.exists() else []
        errors = (results / 'errors.txt').read_text().splitlines()[:20]
        summary = 'Packages:\n```text\n' + '\n'.join(versions) + '\n```\n\nFirst errors:\n```text\n' + '\n'.join(errors) + '\n```\n'
    else:
        summary = 'The probe did not produce a summary. See the workflow setup log.\n'
    run_url = os.environ['GITHUB_SERVER_URL'] + '/' + os.environ['GITHUB_REPOSITORY'] + '/actions/runs/' + os.environ['GITHUB_RUN_ID']
    update(os.environ['PROBE_RESULT'], summary, run_url)


if __name__ == '__main__':
    main()
