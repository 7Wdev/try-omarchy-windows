"""try-omarchy-import: bring a Try Omarchy trial into this Omarchy install."""

import argparse
import contextlib
import datetime
import os
from pathlib import Path
import signal
import sys

from . import VERSION, attach, classify, locate, packages, report, textmerge
from .apply import Applier, load_journal
from .plan import Context, Planner, scan
from .safefs import Destination
from .system import CommandError, Runner
from .trial import Trial, TrialError, marker_time
from .selection import RESERVE_BYTES, SelectionError, option_rows, resolve_selection
from .ui import UI, Cancelled, human_size, plain_label

class RunState:
    """Whether anything on this computer may have changed yet, and the
    mount session to report on afterwards."""

    def __init__(self):
        self.started = False
        self.session = None


class Stop(Exception):
    """End the run with a message and exit status."""

    def __init__(self, message, status=1):
        super().__init__(message)
        self.status = status


def build_parser():
    parser = argparse.ArgumentParser(
        prog="try-omarchy-import",
        description="Bring your Try Omarchy setup into this Omarchy install: settings, "
                    "themes, apps, files, and optionally browser profiles and sign-ins. "
                    "Inside Try Omarchy, 'try-omarchy-import export' (try-omarchy-export) "
                    "packs a trial into an archive instead.")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--data", metavar="FOLDER",
                        help="a Try Omarchy data folder on a mounted Windows drive")
    source.add_argument("--disk", metavar="FILE", help="the trial's disk.raw")
    source.add_argument("--root", metavar="FOLDER",
                        help="an already mounted trial disk or an extracted export")
    parser.add_argument("--user", help="the trial account to import (default: the first one)")
    parser.add_argument("--select", metavar="LIST",
                        help="comma-separated groups to import instead of asking: settings, "
                             "files, apps, browser, keys, packages, theme, all, defaults, or "
                             "group IDs from --dry-run --json")
    parser.add_argument("--resolution", choices=("trial", "keep"),
                        help="for files you already changed on this computer: use the trial's "
                             "version (trial) or keep this computer's (keep)")
    parser.add_argument("--dry-run", action="store_true", help="show what would happen and stop")
    parser.add_argument("--json", action="store_true", help="print the plan or result as JSON")
    parser.add_argument("--yes", action="store_true", help="accept the defaults without asking")
    parser.add_argument("--cleanup", action="store_true",
                        help="unmount anything an interrupted run left mounted")
    parser.add_argument("--version", action="version", version=f"try-omarchy-import {VERSION}")
    return parser


def main(argv=None, runner=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv[:1] == ["export"]:
        from .export import main as export_main
        return export_main(argv[1:])
    args = build_parser().parse_args(argv)
    ui = UI(interactive=not (args.yes or args.json))
    if args.cleanup:
        return cleanup(ui)
    if os.geteuid() == 0:
        ui.error("run this as the account that should receive your trial, not as root. "
                 "It asks for your password when it needs to mount something.")
        return 2
    if args.json and not (args.yes or args.dry_run):
        ui.error("--json shows the plan with --dry-run, or imports without asking with --yes")
        return 2
    if not ui.interactive and not (args.yes or args.json or args.dry_run):
        ui.error("run this in a terminal to choose what to bring over, or pass --yes to "
                 "accept the defaults")
        return 2
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, lambda number, frame: sys.exit(128 + number))
    runner = runner or Runner()
    state = RunState()
    try:
        try:
            with contextlib.ExitStack() as stack:
                return run(args, ui, runner, stack, state)
        finally:
            if state.session is not None and state.session.errors:
                ui.warn("Some of what the import mounted could not be undone ("
                        + "; ".join(state.session.errors) + "). Run this again with --cleanup.")
    except Stop as stop:
        if str(stop):
            ui.error(str(stop))
        return stop.status
    except (Cancelled, KeyboardInterrupt):
        ui.say()
        if state.started:
            ui.say("Stopped. Anything already imported stays; run this again to continue.")
        else:
            ui.say("Stopped. Nothing was changed.")
        return 130
    except (OSError, CommandError) as error:
        ui.error(f"{error}. Nothing after this point was changed; fix the problem and run "
                 "this again to continue.")
        return 1


def cleanup(ui, runner=None):
    """Undo what an interrupted run left, innermost first: the trial mounts,
    snapshots, loop devices, then the Windows drives they were read from."""
    runner = runner or Runner()
    status = 0
    cleaned = 0
    mounts = attach.leftover_mounts()
    windows = [path for path in mounts if os.path.basename(path).startswith("windows-")]
    inner = [path for path in mounts if path not in windows]

    def unmount(paths):
        nonlocal status, cleaned
        for mountpoint in paths:
            try:
                runner.run(["umount", mountpoint], sudo=True)
                cleaned += 1
            except CommandError as error:
                ui.error(str(error))
                status = 1

    unmount(inner)
    try:
        for line in runner.run(["dmsetup", "ls"], sudo=True).stdout.splitlines():
            name = line.split()[0] if line.split() else ""
            if name.startswith(attach.SNAPSHOT_PREFIX):
                runner.run(["dmsetup", "remove", name], sudo=True, check=False)
                cleaned += 1
        for line in runner.run(["losetup", "--list", "--noheadings", "--output",
                                "NAME,BACK-FILE"], sudo=True).stdout.splitlines():
            parts = line.split(None, 1)
            if len(parts) == 2 and (attach.is_our_loop_backing(parts[1]) or any(
                    parts[1].strip().startswith(path + "/") for path in windows)):
                runner.run(["losetup", "--detach", parts[0]], sudo=True, check=False)
                cleaned += 1
    except CommandError as error:
        ui.error(str(error))
        status = 1
    unmount(windows)
    cleaned += attach.remove_scratch_files()
    ui.say("Cleaned up what an earlier run left behind." if cleaned else "Nothing was left behind.")
    return status


def find_trial(args, ui, runner, session):
    """Return (trial root, install or None)."""
    if args.root:
        return Path(args.root), None
    if args.disk:
        disk = Path(args.disk)
        if not disk.is_file():
            raise Stop(f"{disk} is not a file")
        superblock = locate.read_superblock(disk)
        if superblock is None:
            raise Stop(f"{disk} is not a Try Omarchy disk this importer can read")
        ui.say("Mounting the trial disk read-only. Enter your password if asked.")
        if not runner.sudo_ready():
            raise Stop("sudo is needed to mount the trial disk")
        _warn_recovery(ui, superblock)
        return session.attach_disk(disk, superblock.needs_recovery), None
    if args.data:
        install = locate.describe(Path(args.data))
        if install is None:
            raise Stop(f"{args.data} has no Try Omarchy disk (vm/disk.raw)")
        return _attach_install(ui, runner, session, install), install

    volumes = locate.list_volumes(runner)
    if not volumes:
        raise Stop("No Windows drive was found on this computer. If you installed Omarchy on a "
                   "separate disk or wiped Windows, use try-omarchy-export in Try Omarchy "
                   "instead, or pass --data with the folder that holds your trial.")
    ui.say("Looking for Try Omarchy on your Windows drives. Enter your password if asked.")
    if not runner.sudo_ready():
        raise Stop("sudo is needed to read the Windows drive")
    roots = {}
    problems = []
    for volume in volumes:
        name = volume.label or volume.device
        if volume.fstype == "bitlocker":
            problems.append(f"{name} is encrypted with BitLocker, so Omarchy cannot read it. In "
                            "Windows, turn off BitLocker (Settings > Privacy & security > Device "
                            "encryption), wait until it finishes decrypting, and try again.")
            continue
        if volume.mountpoint:
            root = Path(volume.mountpoint)
        else:
            try:
                root = session.mount_windows(volume.device)
            except CommandError as error:
                problems.append(f"{name} could not be opened ({error.stderr or error}). If Windows "
                                "was hibernated or used Fast Startup, start Windows, turn off Fast "
                                "Startup, shut down fully and try again.")
                continue
        if locate.hibernated(root):
            problems.append(f"{name}: Windows was not fully shut down (Fast Startup or "
                            "hibernation), so its files may be out of date. Start Windows, turn "
                            "off Fast Startup (Control Panel > Power Options > Choose what the "
                            "power buttons do), shut down fully and try again.")
            continue
        roots[root] = volume
    installs = locate.find_installs(roots)
    if not installs:
        if problems:
            # The trial is most likely on the drive that could not be read.
            for problem in problems[1:]:
                ui.warn(problem)
            raise Stop(problems[0])
        raise Stop("No Try Omarchy trial was found on the Windows drives. If you moved it to "
                   "another folder, run again with --data and that folder.")
    for problem in problems:
        ui.warn(problem)
    if len(installs) > 1:
        labels = [plain_label(_install_label(install)) for install in installs]
        install = installs[ui.choose_one("Which trial do you want to bring over?", labels)]
    else:
        install = installs[0]
    ui.say(f"Found your trial: {_install_label(install)}")
    return _attach_install(ui, runner, session, install), install


def _install_label(install):
    when = datetime.datetime.fromtimestamp(install.last_used).strftime("%b %d %Y")
    who = f"{install.windows_user}, " if install.windows_user else ""
    return f"{who}last used {when}, {human_size(install.allocated)} on disk ({install.data_dir})"


def _warn_recovery(ui, superblock):
    if superblock.needs_recovery or not superblock.clean:
        ui.warn("The trial was not shut down cleanly. Its last changes are recovered in a "
                "temporary copy; the disk on your Windows drive is not changed.")


def _attach_install(ui, runner, session, install):
    if install.problems:
        raise Stop(install.problems[0][0].upper() + install.problems[0][1:])
    if not runner.sudo_ready():
        raise Stop("sudo is needed to mount the trial disk")
    _warn_recovery(ui, install.superblock)
    try:
        return session.attach_disk(install.disk, install.superblock.needs_recovery)
    except CommandError as error:
        raise Stop(f"the trial disk could not be opened ({error.stderr or error}). Start Try "
                   "Omarchy on Windows, let Omarchy finish starting, shut it down from the Omarchy "
                   "menu and try again.") from None


def home_skel():
    # Tests point this at a fixture instead of the machine's /etc/skel.
    return Path(os.environ.get("TRY_OMARCHY_IMPORT_SKEL", "/etc/skel"))


def skel_config_names(*skels):
    names = set()
    for skel in skels:
        try:
            names.update(entry.name for entry in os.scandir(Path(skel) / ".config"))
        except OSError:
            continue
    return names


def free_bytes(path):
    stats = os.statvfs(path)
    return stats.f_bavail * stats.f_frsize


def run(args, ui, runner, stack, state):
    session = stack.enter_context(attach.Session(runner))
    state.session = session
    home = Path.home()
    ui.heading("Import from Try Omarchy")
    root, install = find_trial(args, ui, runner, session)
    try:
        trial = Trial(root, args.user)
    except TrialError as error:
        raise Stop(str(error)) from None
    if len(trial.accounts) > 1 and args.user is None:
        names = [account.name for account in trial.accounts]
        trial.choose(names[ui.choose_one("Which trial account do you want to bring over?", names)])
    account = trial.account
    if not args.root and (account.uid, account.gid) != (os.getuid(), os.getgid()):
        try:
            mapped = session.map_owner(root, account.uid, account.gid)
        except CommandError as error:
            raise Stop(f"the trial's files could not be opened for your account: "
                       f"{error.stderr or error}") from None
        trial = Trial(mapped, account.name)
    ui.say(f"Trial account {account.name}, Omarchy {trial.omarchy_version()}")

    classifier = classify.Classifier(skel_config_names(trial.skel, home_skel()))
    ui.say("Looking through your trial...")
    inventory = scan(trial.home, classifier, trial.baseline_ns())
    theme = trial.theme()
    package_plan = packages.plan_packages(trial, runner)
    free = free_bytes(home)
    rows = option_rows(inventory, package_plan, theme, free)
    if args.select:
        try:
            chosen = resolve_selection(args.select, rows)
        except SelectionError as error:
            raise Stop(str(error), 2) from None
    else:
        picks = ui.choose_many("What do you want to bring over?",
                               [plain_label(row[1]) for row in rows], [row[2] for row in rows])
        chosen = {row[0] for row, pick in zip(rows, picks) if pick}
    group_ids = [row[0] for row in rows if row[0] in chosen and row[0] in inventory.groups]

    context = Context(
        trial_home=trial.home, trial_skel=trial.skel, trial_baseline_ns=trial.baseline_ns(),
        home=home, home_skel=home_skel(), home_baseline_ns=marker_time(home),
        rewriter=textmerge.Rewriter(account.home, home, trial.share_names()),
        share_names=trial.share_names(), journal=load_journal(home),
        running_browsers=runner.running_programs())
    destination = stack.enter_context(Destination(home))
    resolution = args.resolution or "trial"
    plan = Planner(context, destination, resolution).plan(inventory, group_ids)
    if plan.conflicts() and args.resolution is None and ui.interactive:
        ui.say()
        ui.say(f"{len(plan.conflicts())} files you import were already changed on this computer:")
        for action in plan.conflicts()[:8]:
            ui.say(f"  {action.relative}")
        if len(plan.conflicts()) > 8:
            ui.say(f"  and {len(plan.conflicts()) - 8} more")
        choice = ui.choose_one("Which version should stay in place?", [
            "The trial's (this computer's copy goes to the backup folder)",
            "This computer's (the trial's copy is saved next to it)"])
        if choice == 1:
            resolution = "keep"
            plan = Planner(context, destination, resolution).plan(inventory, group_ids)

    needed = plan.bytes_needed()
    if needed > free - RESERVE_BYTES // 4:
        raise Stop(f"Not enough free space: this needs {human_size(needed)} and "
                   f"{human_size(free)} is free. Pick fewer folders.")

    if args.json and args.dry_run:
        ui.say(report.plan_json(plan, inventory, {"packages": _packages_json(package_plan),
                                                  "theme": theme}))
        return 0
    ui.say()
    for line in report.plan_summary(plan):
        ui.say(f"  {line}")
    ui.say(f"  Space needed: {human_size(needed)} of {human_size(free)} free")
    if args.dry_run:
        ui.pager(report.plan_details(plan))
        return 0
    while ui.interactive:
        choice = ui.choose_one("Ready?", ["Import now", "Show everything first", "Cancel"])
        if choice == 0:
            break
        if choice == 2:
            raise Cancelled()
        ui.pager(report.plan_details(plan))

    state.started = True
    applier = Applier(destination, progress=lambda done, total, action: ui.progress(
        done, total, action.relative))
    result = applier.apply(plan)
    steps = []
    if "packages" in chosen and not package_plan.empty():
        steps.extend(_install_packages(ui, runner, package_plan))
    if _imported(result, ".config/mise/"):
        steps.append(packages.mise_install(runner, home))
    if _imported(result, ".config/hypr/"):
        steps.append(packages.verify_hyprland(runner, home))
    if "theme" in chosen:
        if theme != _current_theme(home):
            steps.append(packages.set_theme(runner, theme))
        background = packages.background_path(trial.background(), account.home, home)
        steps.append(packages.set_background(runner, background, home))

    summary = report.result_summary(result, steps)
    notes = _notes(package_plan, account)
    if args.json:
        ui.say(report.plan_json(plan, None, {"result": [r.__dict__ for r in result.results],
                                             "steps": [s.__dict__ for s in steps if s],
                                             "notes": notes}))
    else:
        ui.say()
        for line in summary + notes:
            ui.say(line)
        ui.say()
        ui.say("Log out and back in (or restart) so everything picks up your settings.")
    incomplete = result.failures() or any(step is not None and not step.ok for step in steps)
    return 1 if incomplete else 0


def _imported(result, prefix):
    return any(item.status == "done" and item.relative.startswith(prefix)
               for item in result.results)


def _current_theme(home):
    try:
        return (Path(home) / ".local/state/omarchy/current/theme.name").read_text().strip()
    except OSError:
        return None


def _install_packages(ui, runner, plan):
    steps = []
    if plan.repo:
        ui.say(f"Installing {len(plan.repo)} packages: {' '.join(plan.repo)}")
        steps.append(packages.install_repo(runner, plan.repo))
    if plan.aur:
        ui.say(f"Installing {len(plan.aur)} AUR packages: {' '.join(plan.aur)}")
        steps.append(packages.install_aur(runner, plan.aur, plan.has_yay))
    if plan.flatpaks:
        ui.say(f"Installing {len(plan.flatpaks)} Flatpak apps")
        steps.append(packages.install_flatpaks(runner, plan.flatpaks, plan.has_flatpak))
    return steps


def _notes(plan, account):
    notes = []
    if plan is None:
        return notes
    if plan.services:
        units = " ".join(plan.services)
        notes.append(f"These services were turned on in the trial: {units}. "
                     f"To turn them on here: sudo systemctl enable --now {units}")
    if plan.groups:
        groups = ",".join(plan.groups)
        notes.append(f"Your trial account was also in these groups: {groups}. "
                     f"To add yourself here: sudo usermod -aG {groups} $USER")
    return notes


def _packages_json(plan):
    if plan is None:
        return None
    return {"repo": plan.repo, "aur": plan.aur, "installed": plan.installed,
            "flatpaks": [list(item) for item in plan.flatpaks], "services": plan.services,
            "groups": plan.groups}

