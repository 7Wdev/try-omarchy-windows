# try-omarchy-import

The importer that brings a Try Omarchy trial into an installed Omarchy. It
runs on the new Omarchy install, not in the guest and not on Windows. User
instructions are in [docs/MIGRATION.md](../docs/MIGRATION.md).

It only uses the Python standard library (Omarchy ships Python for uwsm, ufw
and Flatpak), plus tools every Omarchy install has: util-linux (`lsblk`,
`losetup`, `mount`), `dmsetup`, git (for three-way merges), gum (prompts) and
sudo.

## How it works

1. **Find the trial.** It mounts each NTFS partition read-only with ntfs3 and
   looks for `Users/*/AppData/Local/TryOmarchy/vm/disk.raw`, following
   `data-location.json` to installs moved to another folder or drive. A drive
   Windows hibernated or left in Fast Startup is refused, because its files
   may be out of date.
2. **Attach the disk read-only.** `disk.raw` is a bare ext4 filesystem. It is
   attached to a read-only loop device. If the trial was not shut down cleanly,
   the journal is replayed into a throwaway device-mapper snapshot backed by a
   sparse file in `/var/tmp`, so the file on the Windows drive is never
   written. When the trial account's uid differs from the caller's, an
   ID-mapped bind mount makes its private files readable without running the
   copy as root.
3. **Sort the trial's home into groups** (`classify.py`): settings, one group
   per visible folder, other apps' data, browser profiles, and keys and
   sign-ins. Try-only files (the QEMU display profile, the pinch loader,
   `suspend-off`, the shared-folder link, Windows app shortcuts), caches and
   hardware-specific files (`monitors.lua`) stay behind.
4. **Plan** (`plan.py`) against the home directory on this computer. A file
   counts as untouched when it still equals the trial's `/etc/skel` copy, or
   when it last changed (the later of ctime and mtime) before Omarchy
   finished setting the account up (`~/.local/state/omarchy/done/finalize-user`).
   The same rule decides whether this computer's copy is still a default.
   - Untouched in the trial: nothing to bring, this computer's newer default
     stays.
   - Changed in the trial, still a default here: the trial's version replaces
     it. If the default changed between Omarchy versions, `git merge-file`
     combines the user's changes with the newer default, using the trial's
     skeleton copy as the base. Where both touched the same lines, the user's
     lines win.
   - Changed on both sides: a conflict. By default the trial's version goes in
     place and this computer's goes to the backup. `--resolution keep` does
     the opposite and writes `name.from-try-omarchy` next to it.
   - Files in folders are never overwritten; a clash is written as
     `name (from Try Omarchy).ext`.
   - List files (shell history, `known_hosts`, Files bookmarks) and the login
     keyring are merged entry by entry.
   - Paths into the trial's home (`/home/omarchy/...`) are rewritten to the new
     home in text files and symlinks.
5. **Apply** (`apply.py`, `safefs.py`). Destination paths are reached from a
   descriptor of the home directory one component at a time with
   `O_NOFOLLOW`, so a symlinked parent never redirects a write. New files are
   written to a temporary name and linked into place; replacements are backed
   up to `~/.local/share/try-omarchy-import/backups/<run>` first and renamed in
   only if the target still matches the plan. An append-only journal in
   `~/.local/state/try-omarchy-import/journal.jsonl` lets a later run
   recognise its own work: it resumes after an interruption, never brings
   back a file the user deleted, never overwrites a later edit, and picks up
   files the user changed in the trial since the last import.
6. **Finish.** Reinstall added packages (pacman, then yay for the AUR),
   Flatpak apps and mise tools, switch to the trial's theme and background,
   and report enabled services and group memberships the user may want.

## Code

| File | Purpose |
| --- | --- |
| `cli.py` | Arguments and the interactive flow |
| `locate.py` | Windows volumes, Try Omarchy data folders, ext4 superblock, hibernation |
| `attach.py` | Read-only mounts, snapshot, ID mapping, cleanup |
| `trial.py` | Account, versions, theme, packages, services from the trial root |
| `classify.py` | Which group a path belongs to, and what stays behind |
| `plan.py` | Inventory and the per-path decisions |
| `textmerge.py` | Try block stripping, path rewriting, three-way and list merges, keyring merge |
| `safefs.py` | No-follow destination access |
| `apply.py` | Backups, journal, applying a plan |
| `packages.py` | Packages, Flatpak, mise, theme and background |
| `ui.py`, `report.py` | Prompts and summaries |

## Test and build

From the repository root:

```sh
python3 -m unittest discover -s migrate/tests -t migrate
python3 migrate/build.py --version v0.0.0-test --output /tmp/importer
```

The tests use synthetic trial roots and home directories and need git. They
cover the policy above, interrupted imports (a real SIGKILL mid-import), a Git
workspace with staged, modified and untracked files, and the mount steps with
a fake command runner.

`build.py` writes a reproducible `try-omarchy-import.pyz` and
`try-omarchy-import.sh`, the bootstrap behind the curl command. The bootstrap
records the release tag and the `.pyz` digest, prefers a matching copy next to
itself, and otherwise downloads the `.pyz` from the same release. The release
workflow builds both into the guest draft and lists them in `SHA256SUMS`.

To try it against a trial disk without the Windows side, run it as a normal
user with `--disk /path/to/disk.raw`, or `--root` with an already mounted
trial. `--dry-run --json` prints the plan without changing anything.

## Credits

The descriptor walk, the link-then-rename publication, the backup-before-
replace rule and the resume rules (never recreate what the user deleted, never
overwrite a later edit) follow the restore experiment in
[omacom/omarchy-mac-installer](https://github.com/omacom/omarchy-mac-installer)
(`explore/try-omarchy-migration`, commit 4b313ad), MIT licensed, copyright
David Heinemeier Hansson. See `THIRD_PARTY_NOTICES.md`.
