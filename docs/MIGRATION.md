# Moving from Try Omarchy to a real Omarchy install

If the trial wins you over, you can install Omarchy for real and keep what you
set up: settings, themes, keybindings, the apps you installed, your files and
projects, and if you want them, your browser profile and sign-ins.

There are two ways:

- **Install Omarchy next to Windows on the same PC.** After installing, one
  command reads your trial straight off the Windows drive. Nothing has to be
  exported first. This is the recommended way.
- **Replace Windows, or move to another computer.** Export your setup first
  with `try-omarchy-export` and carry it over. See
  [Replacing Windows](#replacing-windows-or-moving-to-another-computer).

## Install next to Windows and bring your trial along

### Before you leave Windows

Try Omarchy can do most of this for you: open **Settings > Backup and
recovery > Install Omarchy...**. It checks whether Omarchy is still running,
whether Fast Startup is on and whether BitLocker is on for the drives it needs,
and lists only what is left, each with a button that does it or opens the right
place. Once nothing is left it shows the installation steps and the command to
run afterwards. If Windows cannot report encryption status, it offers a button
to check BitLocker settings yourself.

1. **Shut Omarchy down from inside the trial** (Omarchy menu > System >
   Shutdown), so the trial's disk is saved cleanly. Keep Try Omarchy installed
   until the import is done: uninstalling it deletes the trial.
2. **Turn off BitLocker (Device encryption).** The Omarchy installer needs it
   off, and it lets Omarchy read the Windows drive. Settings > Privacy &
   security > Device encryption, or Control Panel > BitLocker Drive
   Encryption. Decrypting can take a while; wait for it to finish.
3. **Turn off Fast Startup.** Control Panel > Power Options > Choose what the
   power buttons do > Change settings that are currently unavailable, then
   clear "Turn on fast startup". With Fast Startup on, Windows never fully
   shuts down and Omarchy would only see an out-of-date copy of your files.
4. **Make room for Omarchy.** Open Disk Management, right-click C: and choose
   Shrink Volume. The space you free up becomes Omarchy's.
5. **Install Omarchy into the free space** by following the Omarchy manual's
   [dual boot guide](https://learn.omacom.io/2/the-omarchy-manual/120/dual-boot-install).
   Omarchy then starts by default. Windows is still there: run `limine-scan`
   in Omarchy to add it to the boot menu.

### After installing

Start Omarchy, open a terminal (SUPER+RETURN) and run:

```
curl -fsSL https://tryomarchy.com/import | bash
```

It asks for your password to read the Windows drive, finds your trial, and
shows a list of what it can bring over. Pick what you want, check the summary
and start the import. When it finishes, log out and back in (or restart).

Nothing on the Windows drive is changed. The trial stays usable in Windows,
and you can run the import again later.

### What comes over

| In the list | What it includes | Picked by default |
| --- | --- | --- |
| Settings and customizations | Hyprland keybindings, input and look, Omarchy settings and toggles, terminal and shell setup, git identity, Neovim, mise, custom themes, web apps, fonts, your scripts in `~/.local/bin` | Yes |
| Each folder in your home folder | Documents, Projects, Pictures and the rest, including Git projects with work you have not committed | Yes, if there is room |
| App data | Settings and data of other apps you used, one entry per app | Yes, for apps under 100 MB |
| Apps you installed | Packages you added (pacman, then the AUR through yay), Flatpak apps, and the tools in your mise config | Yes |
| Theme and background | The theme and background you had picked | Yes |
| Browser profile | Bookmarks, history, extensions, saved logins and signed-in sessions | No |
| Keys and sign-ins | SSH and GPG keys, the login keyring, and command line sign-ins such as the GitHub CLI | No |

Only what you actually changed comes over. A file that is still the Omarchy
default in your trial is left alone, so the new install keeps its newer
defaults. When you changed a file whose default has since changed in a newer
Omarchy, your changes are combined with the new default.

Picking a browser profile also brings the login keyring along, because the
browser's saved passwords are encrypted with a key stored there. Close the
browser on the new install before importing its profile.

### What stays behind

- Anything only Try Omarchy needs: the shared Windows folder link, Windows app
  shortcuts, the virtual display and touchpad setup, and the hidden Suspend
  entry (your real computer can suspend).
- Display settings (`~/.config/hypr/monitors.lua`). The new install sets up
  your real screens.
- Caches, and things that rebuild themselves, such as Neovim plugins and mise
  tool downloads.
- System changes. Services you turned on and groups you joined are listed at
  the end, with the command to do the same on the new install.
- Apps you removed. If you uninstalled preinstalled apps in the trial, remove
  them again on the new install.

### If you already changed something on the new install

If a file was changed on both sides, the importer asks which version should
stay in place. Either way, nothing is lost:

- **The trial's version** (the default): the new install's copy is saved in
  the backup folder.
- **This computer's version**: the trial's copy is saved next to it as
  `name.from-try-omarchy`.

Files in your folders are never replaced. If a document with the same name
but different content already exists, the trial's copy is saved next to it
as `name (from Try Omarchy).ext`.

### Backups and running it again

Anything the import replaces is backed up to
`~/.local/share/try-omarchy-import/backups/<date>`.

You can run the command again at any time. It skips what it already
imported, never brings back something you deleted, never overwrites
something you changed after importing, and picks up anything you changed in
the trial since. If an import is interrupted, run it again to finish it.

### Troubleshooting

- **"Windows was not fully shut down"**: Fast Startup or hibernation is on.
  Start Windows, turn off Fast Startup (see above), shut down, then try
  again.
- **"This is a portable install"**: a portable Try Omarchy keeps its disk in
  a format Omarchy cannot read directly. Use
  [Replacing Windows](#replacing-windows-or-moving-to-another-computer) instead.
- **"The trial was not shut down cleanly"**: nothing to do. The importer
  recovers the trial's last changes in a temporary copy. Next time, shut
  Omarchy down from its menu before leaving Windows.
- **The trial is somewhere else**: a trial you moved to another folder or
  drive in Try Omarchy's settings is found automatically. If it is not, pass
  the folder that contains `vm\disk.raw` with `--data`, as shown below.
- **An import was cut off by a crash or power loss**: run it with `--cleanup`
  to unmount what it left, then run the import again.

Options go at the end of the command, after `bash -s --`. For example, to see
what would happen without changing anything:

```
curl -fsSL https://tryomarchy.com/import | bash -s -- --dry-run
```

Other options: `--data FOLDER`, `--cleanup`, and `--help` for the full list.

## Replacing Windows or moving to another computer

If Windows will be gone, or Omarchy goes on another computer, export your
setup inside the trial first.

### Export inside the trial

Open a terminal (SUPER+RETURN) and run:

```
try-omarchy-export
```

It shows the same list as the import, sized for the space where the archive
goes, and writes `omarchy-export-<date>.tar.gz` to the shared Windows folder
when there is one, otherwise to your home folder. Pass a folder to put it
somewhere else, for example `try-omarchy-export /run/media/$USER/USB`.

Only what you changed goes in. Keys, sign-ins and browser profiles are only
included when you pick them; the archive then says so, because anyone with
the file could use them. Keep it private and delete it once you have imported
it.

### Import on the new install

Copy the archive over, then as the account that should receive it:

```
tar -xzf omarchy-export-<date>.tar.gz
omarchy-export-<date>/import.sh
```

The archive carries its own copy of the importer, so this works without a
network connection. It works like the import above: it shows what it can
bring over, asks before changing anything, backs up what it replaces, and can
be run again.

Exports from earlier Try Omarchy versions contain `restore.sh` instead. Run
that one the same way; it backs up what it replaces under
`~/.omarchy-restore-backup/<time>`.
