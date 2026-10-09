import importlib.util
from pathlib import Path
import unittest

HERE = Path(__file__).parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check = load('check')
alert = load('alert')


class LogChecks(unittest.TestCase):
    def test_runtime_errors_cannot_be_allowlisted(self):
        for message in ('TypeError: Cannot read property text of undefined',
                        'ReferenceError: Color is not defined', 'Widget is not a type',
                        'Cannot assign null to QColor', 'Unable to assign [undefined] to QColor',
                        'Failed to create component', 'Error loading configuration',
                        'Cannot read properties of undefined', 'Segmentation fault (core dumped)'):
            with self.subTest(message=message):
                self.assertEqual(check.errors(message, '.*'), [message])

    def test_only_known_warnings_pass(self):
        known = ' WARN quickshell.service.upower: Could not connect to DBus. UPower service will not work.'
        self.assertEqual(check.errors(known, check.ALLOWLIST.read_text()), [])
        colored = known.replace('WARN', '\x1b[33mWARN\x1b[97m').replace(': Could', '\x1b[0m: Could')
        self.assertEqual(check.errors(colored, check.ALLOWLIST.read_text()), [])
        unknown = ' WARN qml: new warning'
        self.assertEqual(check.errors(unknown, check.ALLOWLIST.read_text()), [unknown])


class IssueChecks(unittest.TestCase):
    def run_update(self, result, issues):
        calls = []

        def request(method, path, data=None):
            calls.append((method, path, data))
            return issues if path.startswith('/issues?') else {}

        alert.update(result, 'summary', 'https://example.com/run', request)
        return calls

    def test_first_failure_creates_one_issue(self):
        calls = self.run_update('failure', [])
        creates = [c for c in calls if c[:2] == ('POST', '/issues')]
        self.assertEqual(len(creates), 1)
        self.assertIn(alert.MARKER, creates[0][2]['body'])

    def test_repeat_failure_updates_and_next_failure_reopens(self):
        for state in ('open', 'closed'):
            calls = self.run_update('failure', [{'number': 12, 'state': state, 'body': alert.MARKER}])
            self.assertEqual(calls[-1][:2], ('PATCH', '/issues/12'))
            self.assertEqual(calls[-1][2]['state'], 'open')
            self.assertFalse(any(c[:2] == ('POST', '/issues') for c in calls))

    def test_recovery_comments_and_closes_once(self):
        calls = self.run_update('success', [{'number': 12, 'state': 'open', 'body': alert.MARKER}])
        self.assertEqual(calls[-2][:2], ('POST', '/issues/12/comments'))
        self.assertEqual(calls[-1][2]['state'], 'closed')
        calls = self.run_update('success', [{'number': 12, 'state': 'closed', 'body': alert.MARKER}])
        self.assertEqual(len(calls), 1)

    def test_unrelated_labelled_issue_is_untouched(self):
        calls = self.run_update('success', [{'number': 12, 'state': 'open', 'body': 'manual issue'}])
        self.assertEqual(len(calls), 1)

    def test_duplicate_issues_stop_without_writing(self):
        with self.assertRaisesRegex(RuntimeError, 'Multiple'):
            self.run_update('failure', [{'number': n, 'body': alert.MARKER} for n in (12, 13)])


if __name__ == '__main__':
    unittest.main()
