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


if __name__ == '__main__':
    unittest.main()
