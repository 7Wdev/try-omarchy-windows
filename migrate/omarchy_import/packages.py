"""Reinstall the apps the trial added, set the theme, and report the rest.

Packages the user added on top of the Try Omarchy image are compared with
what this computer already has and what its repositories offer. The rest are
treated as AUR packages and installed with yay. Flatpak apps are reinstalled
from Flathub. Services and group memberships are only reported, since
changing them is a decision the user should make on this computer.
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from . import names
from .system import CommandError

# Backgrounds outside the home folder come from Omarchy's own themes.
SYSTEM_BACKGROUNDS = "/usr/share/omarchy/"

# Groups that grant nothing a person notices on a current Omarchy install
# (logind hands out device access), or that every account already has.
QUIET_GROUPS = {"users", "audio", "video", "render", "kvm", "storage", "optical", "network",
                "power", "sys", "adm", "systemd-journal", "lp", "wheel"}


@dataclass
class PackagePlan:
    repo: list = field(default_factory=list)
    aur: list = field(default_factory=list)
    installed: list = field(default_factory=list)
    flatpaks: list = field(default_factory=list)
    services: list = field(default_factory=list)
    groups: list = field(default_factory=list)
    has_yay: bool = False
    has_flatpak: bool = False
    # Trial entries left out because they are not valid package or app names.
    invalid: int = 0

    def empty(self):
        return not (self.repo or self.aur or self.flatpaks)


def _lines(runner, argv):
    try:
        return runner.run(argv).stdout.splitlines()
    except (CommandError, OSError):
        return []


def plan_packages(trial, runner):
    plan = PackagePlan()
    trial_packages = trial.packages()
    added = trial_packages.added
    plan.invalid = trial_packages.invalid
    installed = set(_lines(runner, ["pacman", "-Qq"]))
    available = set()
    for line in _lines(runner, ["pacman", "-Sl"]):
        parts = line.split()
        if len(parts) >= 2:
            available.add(parts[1])
    for name in added:
        if name in installed:
            plan.installed.append(name)
        elif name in available:
            plan.repo.append(name)
        else:
            plan.aur.append(name)
    plan.has_yay = runner.which("yay") is not None
    plan.has_flatpak = runner.which("flatpak") is not None
    if plan.has_flatpak:
        present = set(_lines(runner, ["flatpak", "list", "--app", "--columns=application"]))
        plan.flatpaks = [(app, scope) for app, scope in trial.flatpaks() if app not in present]
    else:
        plan.flatpaks = list(trial.flatpaks())
    enabled = set()
    for line in _lines(runner, ["systemctl", "list-unit-files", "--state=enabled", "--no-legend",
                                "--no-pager"]):
        parts = line.split()
        if parts:
            enabled.add(parts[0])
    plan.services = [unit for unit in trial.enabled_services() if unit not in enabled]
    mine = set(" ".join(_lines(runner, ["id", "-Gn"])).split())
    existing = set()
    for line in _lines(runner, ["getent", "group"]):
        existing.add(line.split(":", 1)[0])
    plan.groups = [group for group in trial.account.groups
                   if group not in mine and group in existing and group != trial.account.name
                   and group not in QUIET_GROUPS]
    return plan


def _count(items, noun):
    return f"{len(items)} {noun}" + ("" if len(items) == 1 else "s")


@dataclass
class StepResult:
    name: str
    ok: bool
    detail: str = ""


def _refuse(step, items, valid):
    """A failed step if any name is not one the installer should see, else None."""
    bad = sum(1 for item in items if not valid(item))
    if not bad:
        return None
    return StepResult(step, False, "nothing was installed, because the list has "
                      f"{bad} {'entry that is' if bad == 1 else 'entries that are'} "
                      "not a valid name")


def install_repo(runner, packages):
    if not packages:
        return StepResult("packages", True)
    refused = _refuse("packages", packages, names.package)
    if refused:
        return refused
    try:
        runner.run(["pacman", "-S", "--needed", "--noconfirm", "--", *packages], sudo=True,
                   capture=False)
    except CommandError as error:
        return StepResult("packages", False, f"pacman stopped ({error.returncode}); "
                          f"install these yourself: {' '.join(packages)}")
    return StepResult("packages", True, f"installed {_count(packages, 'package')}")


def install_aur(runner, packages, has_yay):
    """Install with yay, without the yay, makepkg and git settings in this
    home folder: the import may just have brought them, and they can name
    other programs to run."""
    if not packages:
        return StepResult("aur", True)
    refused = _refuse("aur", packages, names.package)
    if refused:
        return refused
    if not has_yay:
        return StepResult("aur", False, "yay is not installed; these came from the AUR: "
                          + " ".join(packages))
    with tempfile.TemporaryDirectory(prefix="try-omarchy-import-yay-") as config:
        # makepkg reads $XDG_CONFIG_HOME/pacman/makepkg.conf instead of
        # ~/.makepkg.conf when it exists.
        os.mkdir(os.path.join(config, "pacman"))
        with open(os.path.join(config, "pacman", "makepkg.conf"), "w", encoding="utf-8") as output:
            output.write("# try-omarchy-import builds with the settings in /etc/makepkg.conf\n")
        environment = dict(os.environ, XDG_CONFIG_HOME=config, GIT_CONFIG_GLOBAL=os.devnull)
        try:
            runner.run(["yay", "-S", "--needed", "--noconfirm", "--", *packages], capture=False,
                       env=environment)
        except CommandError as error:
            return StepResult("aur", False, f"yay stopped ({error.returncode}); "
                              f"install these yourself: {' '.join(packages)}")
    return StepResult("aur", True, f"installed {_count(packages, 'AUR package')}")


def install_flatpaks(runner, apps, has_flatpak):
    if not apps:
        return StepResult("flatpak", True)
    refused = _refuse("flatpak", [app for app, _ in apps], names.flatpak)
    if refused:
        return refused
    if not has_flatpak:
        return StepResult("flatpak", False, "Flatpak is not installed; the trial had: "
                          + " ".join(app for app, _ in apps))
    failed = []
    for scope in ("system", "user"):
        ids = [app for app, app_scope in apps if app_scope == scope]
        if not ids:
            continue
        try:
            runner.run(["flatpak", "install", "--noninteractive", "-y", f"--{scope}", "--",
                        "flathub", *ids], capture=False)
        except CommandError:
            failed.extend(ids)
    if failed:
        return StepResult("flatpak", False, "these Flatpak apps did not install: " + " ".join(failed))
    return StepResult("flatpak", True, f"installed {_count(apps, 'Flatpak app')}")


def mise_install(runner, home):
    config = Path(home) / ".config/mise/config.toml"
    if not config.exists() or runner.which("mise") is None:
        return None
    try:
        runner.run(["mise", "install", "--yes"], capture=False, cwd=home)
    except CommandError as error:
        return StepResult("mise", False, f"mise install stopped ({error.returncode}); "
                          "run it again later")
    return StepResult("mise", True, "installed your mise tools")


def set_theme(runner, theme):
    if not theme:
        return None
    if not names.theme(theme):
        return StepResult("theme", False, "the trial's theme name is not valid; pick a theme from "
                          "the Omarchy menu")
    if runner.which("omarchy-theme-set") is None:
        return StepResult("theme", False, f"pick the {theme} theme from the Omarchy menu")
    try:
        runner.run(["omarchy-theme-set", theme], capture=False)
    except CommandError:
        return StepResult("theme", False, f"the {theme} theme could not be set; "
                          "pick it from the Omarchy menu")
    return StepResult("theme", True, f"switched to {theme}")


def background_path(trial_background, trial_home, home):
    """Where the trial's background is on this computer, or None. Only a file
    in this home folder or in Omarchy's own themes counts."""
    if not trial_background or not trial_background.startswith("/") or \
            any(part in (".", "..") for part in trial_background.split("/")):
        return None
    trial_home = trial_home.rstrip("/")
    theme_backgrounds = f"{trial_home}/.local/state/omarchy/current/theme/backgrounds/"
    if trial_background.startswith(theme_backgrounds):
        candidate = Path(home) / ".local/state/omarchy/current/theme/backgrounds" / \
            trial_background[len(theme_backgrounds):]
    elif trial_background.startswith(trial_home + "/"):
        candidate = Path(home) / trial_background[len(trial_home) + 1:]
    elif trial_background.startswith(SYSTEM_BACKGROUNDS):
        candidate = Path(trial_background)
    else:
        return None
    return candidate if candidate.is_file() else None


def set_background(runner, path, home):
    if path is None or runner.which("omarchy-theme-bg-set") is None:
        return None
    current = Path(home) / ".local/state/omarchy/current/background"
    try:
        if os.path.realpath(current) == os.path.realpath(path):
            return None
    except OSError:
        pass
    try:
        runner.run(["omarchy-theme-bg-set", str(path)], capture=False)
    except CommandError:
        return StepResult("background", False, "the trial's background could not be set")
    return StepResult("background", True, "set your background")


CONFIG_ERROR = re.compile(r"\S+\.(?:lua|conf):\d+")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def verify_hyprland(runner, home):
    """Check the imported Hyprland config parses, so a problem shows now and
    not at the next login. Only a reported error in a config file counts; if
    Hyprland cannot run the check at all, nothing is said."""
    if runner.which("Hyprland") is None:
        return None
    for name in ("hyprland.lua", "hyprland.conf"):
        config = Path(home) / ".config/hypr" / name
        if config.is_file():
            break
    else:
        return None
    environment = dict(os.environ)
    scratch = None
    if not environment.get("XDG_RUNTIME_DIR"):
        scratch = tempfile.mkdtemp(prefix="try-omarchy-import-hypr-")
        environment["XDG_RUNTIME_DIR"] = scratch
    try:
        result = runner.run(["Hyprland", "--verify-config", "-c", str(config)], check=False,
                            timeout=60, env=environment)
    except (OSError, subprocess.TimeoutExpired):
        return None
    finally:
        if scratch:
            shutil.rmtree(scratch, ignore_errors=True)
    output = ANSI.sub("", (result.stdout or "") + (result.stderr or ""))
    if result.returncode == 0 and "config ok" in output:
        return StepResult("hyprland", True)
    problem = next((line[match.start():].strip() for line in output.splitlines()
                    for match in [CONFIG_ERROR.search(line)] if match), None)
    if problem is None:
        return None
    return StepResult("hyprland", False, f"Hyprland found a problem in your settings: {problem}. "
                      "Fix that line, or copy the file back from the backup folder")
