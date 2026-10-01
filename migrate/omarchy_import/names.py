"""Which names read from a trial the importer passes on.

Package names, Flatpak IDs, services, themes and accounts come from files on
the trial disk. They only reach an installer, a command or a suggested command
line when they have the form the tool in question accepts, so a crafted entry
cannot become a command line option, a path or shell syntax.
"""

import re

# pacman's rules (makepkg lint_pkgname): letters, digits and @._+-, not
# starting with a hyphen or a dot.
PACKAGE = re.compile(r"[A-Za-z0-9@_+][A-Za-z0-9@._+-]*")
# Flatpak's rules: at least three dot-separated parts of letters, digits, _
# and -, none starting with a digit, and only later parts with a hyphen.
FLATPAK = re.compile(r"[A-Za-z_][A-Za-z0-9_-]*(?:\.[A-Za-z_-][A-Za-z0-9_-]*){2,}")
# systemd unit names, without the backslash escapes a shell would eat.
UNIT = re.compile(r"[A-Za-z0-9_][A-Za-z0-9:_.@-]*\.(?:service|socket|timer|path)")
# Omarchy theme folder names.
THEME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._+-]*")
ACCOUNT = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*\$?")


def _matches(pattern, text, limit):
    return isinstance(text, str) and len(text) <= limit and pattern.fullmatch(text) is not None


def package(name):
    return _matches(PACKAGE, name, 255)


def flatpak(app):
    return _matches(FLATPAK, app, 255)


def unit(name):
    return _matches(UNIT, name, 255)


def theme(name):
    return _matches(THEME, name, 128) and ".." not in name


def account(name):
    return _matches(ACCOUNT, name, 64)
