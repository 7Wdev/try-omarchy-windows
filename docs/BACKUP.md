# VM backup and restore

Try Omarchy provides backup, restore, and reset controls in Settings for
stopped standard installs. Portable installs are not supported yet. These
operations are separate from
[configuration export](MIGRATION.md), which transfers your setup to a physical
Omarchy installation.

## Use Settings

Open Settings and use **Back up**, **Restore**, or **Reset guest** under Backup
and recovery. Close the Omarchy VM first. Recovery uses the settings already
saved on disk; edits in the Settings window remain available after the operation.

- **Back up** opens the Windows save dialog. Choose a new ZIP filename outside
  the installation folder.
- **Restore** asks for a backup and a parent folder, then creates a separate
  restored installation. It adds **Start Omarchy** and **Settings** shortcuts
  inside that new folder. Existing Windows shortcuts are unchanged.
- **Reset guest** offers a full backup first, then asks for confirmation. A
  failed or cancelled backup stops the reset. The factory disk is prepared
  before the old disk is moved into a `vm/before-reset-*` folder. The next normal
  launch starts first-run setup. Windows shared folders and launcher settings
  are kept.

Backup and restore show file progress and support cancellation. Reset retains
only the previous writable disk, not a complete backup of its boot files and
runtime. Keep a full backup before updating or removing the old installation.
The retained disk continues using host space until you remove it yourself.

## Repair unreadable preferences

If a preferences file cannot be read, the launcher or Settings offers to restore
that file's defaults. Choose No to leave it unchanged. Choose Yes to keep a copy
under `preferences-before-repair-*` in the data folder before saving defaults.
Guest files, disk capacity already allocated, and Windows shortcuts are untouched.
Repairing launcher settings leaves shared folders and port forwarding disabled
until you configure them again.

## Create a backup from PowerShell

Shut down Omarchy and close the launcher first. From PowerShell:

```powershell
.\TryOmarchy.exe -backup "D:\Backups\omarchy.zip"
```

The destination folder must already exist on a drive with enough free space, outside the
Try Omarchy data folder. Choose a new filename; an existing backup is never
overwritten. Add `-dir "D:\TryOmarchy"` if you normally use an explicit data path.

The ZIP includes the writable disk, an available factory image, matching kernel and initramfs,
bundled runtime when present, and launcher settings. Shared Windows folders,
external QEMU installations, logs, and the launcher executable are not included.
Keep a copy of the launcher used to create the backup.

A backup contains personal guest files, including any credentials stored in the
guest. It is not encrypted. Store it privately and restore only backups you trust.

Backup refuses a locked disk or a pending update. Finish the update and shut down
normally before trying again. Each archived file has a SHA-256 checksum that is
verified during restore. The ZIP appears at the chosen filename only when the
backup finishes.

## Restore to a separate folder

Close Try Omarchy, then choose a folder that does not exist. Its parent must
already exist on an NTFS or ReFS drive:

```powershell
.\TryOmarchy.exe -restore "D:\Backups\omarchy.zip" -dir "D:\OmarchyRestored"
```

Restore checks and extracts files into a temporary folder before publishing the
new data folder. An error or cancellation leaves the existing installation and
backup unchanged. It never replaces an existing data folder.

In the next normal release, command-line restore also adds **Start Omarchy** and
**Settings** shortcuts inside the restored folder. They point only to that
copy; existing Start Menu and desktop shortcuts are unchanged.

Start the restored copy explicitly:

```powershell
.\TryOmarchy.exe -dir "D:\OmarchyRestored"
```

This does not change the default installation location or existing shortcuts.
Settings may reference shared folders or SSH public keys on the original PC;
review them before starting a restored copy on another machine.

## Space and validation

Backup conservatively requires free space for the full logical size of all
included files, plus 1 GiB. Restore estimates space from the compressed file
sizes, adds a 25 percent margin and 1 GiB, and preserves empty regions as sparse
holes. This is an estimate, not a guarantee that the destination will have enough
space. A disk-full error stops restore before the new folder is published.
Large disks can take a while to read even when mostly empty.

Automated tests cover copying, checksums, cancellation, disk locking, low space,
and preservation of existing folders. Repeat the Settings and restored-guest
boot checks for the exact release candidate using [TESTING.md](TESTING.md).
Keep the original installation until you have checked the restored copy.

For changing the active installation's location, see the next preview's
[move flow](MOVING.md). Restore continues to create an independent copy.

## Template-free installations (unreleased)

A standard raw disk and an independent portable disk can run without a local
factory image. Backup does not fetch one. Full archives retain the existing v1
format; template-free archives use v2 and include boot files, the exact release
receipt, authenticated SHA256SUMS and factory size metadata. Every archive stores
a complete independent raw disk, materializing a portable source first and
verifying its original backing when needed.

Restore v2 with a launcher that supports template-free installations. Released
launchers through v0.10.1 require the full layout and cannot restore or boot a template-free copy. Restored boot components
retain their own release pins and can boot offline. Reset obtains that exact
release's template before moving the old disk. If acquisition fails or is
cancelled, the existing disk and boot receipt stay usable. Retained portable
reset disks are not standalone bundles; restore their matching layout or use a
complete checkpoint.
