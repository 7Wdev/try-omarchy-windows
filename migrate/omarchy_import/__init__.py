"""Bring a Try Omarchy trial into an installed Omarchy.

The importer reads the trial's disk (or an export of it) and copies what the
user made in the trial into their new home directory: settings, themes, apps,
files, and optionally browser profiles and sign-ins. It only uses the Python
standard library, so it runs on a fresh Omarchy install without extra
packages.
"""

# build.py replaces this with the release tag when it builds the .pyz.
VERSION = "dev"
