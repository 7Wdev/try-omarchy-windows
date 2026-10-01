"""Decide which part of the trial's home directory each path belongs to.

The home directory is split into groups the user can pick from:

- settings: Omarchy and desktop configuration, shell setup, custom themes,
  web apps, fonts and scripts. Picked by default.
- files: each visible folder in the home directory (Documents, Projects...).
  Picked by default when they fit.
- apps: data of other apps, one group per app. Small ones are picked by
  default.
- browser: one group per browser profile. Off by default because it carries
  saved logins and cookies.
- keys: SSH and GPG keys, the login keyring and command line sign-ins. Off by
  default.

Anything that only makes sense inside Try Omarchy, belongs to the new
computer's hardware, or is a cache that rebuilds itself is skipped with a
reason the report can show.
"""

from dataclasses import dataclass
import fnmatch

SETTINGS = "settings"
FILES = "files"
APPS = "apps"
BROWSER = "browser"
KEYS = "keys"
SKIP = "skip"
CONTAINER = "container"

SETTINGS_GROUP = "settings"
KEYS_GROUP = "keys"
LOOSE_FILES_GROUP = "files/.loose"


@dataclass(frozen=True)
class Place:
    kind: str
    group: str = ""
    label: str = ""
    reason: str = ""


def skip(reason):
    return Place(SKIP, reason=reason)


CONTAINERS = {"", ".config", ".local", ".local/share", ".local/state", ".local/state/omarchy",
              ".var", ".var/app"}

TOP_SKIP = {
    ".cache": "caches rebuild themselves",
    ".dbus": "session state",
    ".Xauthority": "session state",
    ".ICEauthority": "session state",
    ".xsession-errors": "session log",
    ".xsession-errors.old": "session log",
    ".sudo_as_admin_successful": "session state",
    ".nv": "graphics driver cache",
    ".npm": "package cache",
    ".pnpm-store": "package cache",
    ".bun": "installed by mise or the bun installer",
    ".rustup": "toolchains reinstall from your config",
    ".vscode-server": "remote editor server",
}

TOP_KEYS = {".ssh", ".gnupg", ".password-store", ".aws", ".azure", ".kube", ".docker", ".netrc",
            ".git-credentials", ".npmrc", ".pypirc", ".pgpass", ".pki", ".terraform.d",
            ".vault-token"}

TOP_SETTINGS_DIRS = {".vim", ".oh-my-zsh", ".oh-my-bash", ".terminfo", ".fonts", ".icons",
                     ".themes", ".tmux", ".zsh", ".emacs.d", ".doom.d", ".config-backup",
                     ".XCompose.d", ".agents"}

BROWSERS = {
    ".config/chromium": ("chromium", "Chromium"),
    ".config/google-chrome": ("google-chrome", "Google Chrome"),
    ".config/google-chrome-beta": ("google-chrome-beta", "Google Chrome Beta"),
    ".config/google-chrome-unstable": ("google-chrome-unstable", "Google Chrome Dev"),
    ".config/BraveSoftware": ("brave", "Brave"),
    ".config/vivaldi": ("vivaldi", "Vivaldi"),
    ".config/vivaldi-snapshot": ("vivaldi-snapshot", "Vivaldi Snapshot"),
    ".config/microsoft-edge": ("microsoft-edge", "Microsoft Edge"),
    ".config/microsoft-edge-beta": ("microsoft-edge-beta", "Microsoft Edge Beta"),
    ".config/microsoft-edge-dev": ("microsoft-edge-dev", "Microsoft Edge Dev"),
    ".config/net.imput.helium": ("helium", "Helium"),
    ".mozilla": ("firefox", "Firefox"),
    ".config/mozilla": ("firefox-xdg", "Firefox"),
    ".librewolf": ("librewolf", "LibreWolf"),
    ".zen": ("zen", "Zen Browser"),
}

# Browser processes, used to make sure a profile is not in use before it is
# replaced. Compared with /proc/<pid>/comm, which is at most 15 characters.
BROWSER_PROCESSES = {
    "chromium": ("chromium",),
    "google-chrome": ("chrome",),
    "google-chrome-beta": ("chrome",),
    "google-chrome-unstable": ("chrome",),
    "brave": ("brave",),
    "vivaldi": ("vivaldi-bin", "vivaldi"),
    "vivaldi-snapshot": ("vivaldi-bin", "vivaldi"),
    "microsoft-edge": ("msedge",),
    "microsoft-edge-beta": ("msedge",),
    "microsoft-edge-dev": ("msedge",),
    "helium": ("helium",),
    "firefox": ("firefox", "firefox-bin"),
    "firefox-xdg": ("firefox", "firefox-bin"),
    "librewolf": ("librewolf",),
    "zen": ("zen", "zen-bin"),
}

# The keyring application names each Chromium-based browser stores its
# "Safe Storage" key under.
BROWSER_KEYRING_APPS = {
    "chromium": ("chromium",),
    "google-chrome": ("chrome",),
    "google-chrome-beta": ("chrome",),
    "google-chrome-unstable": ("chrome",),
    "brave": ("brave",),
    "vivaldi": ("vivaldi",),
    "vivaldi-snapshot": ("vivaldi",),
    "microsoft-edge": ("microsoft-edge", "msedge"),
    "microsoft-edge-beta": ("microsoft-edge", "msedge"),
    "microsoft-edge-dev": ("microsoft-edge", "msedge"),
    "helium": ("helium",),
}

CONFIG_KEYS = {"gh", "hub", "op", "1Password", "rclone", "github-copilot", "gcloud", "doctl",
               "Bitwarden", "Bitwarden CLI", "netlify", "heroku"}

CONFIG_SKIP = {
    "pulse": "audio devices differ on this computer",
    "dconf": "rebuilt by the desktop",
}

# Configuration that is part of setting up the desktop, the shell or the
# development tools Omarchy ships. Anything Omarchy seeds into /etc/skel on
# either side counts as well.
CONFIG_SETTINGS = {
    "alacritty", "atuin", "autostart", "bat", "btop", "direnv", "elephant", "environment.d",
    "eza", "fastfetch", "fcitx5", "fish", "fontconfig", "foot", "ghostty", "git", "gtk-3.0",
    "gtk-4.0", "helix", "hypr", "imv", "kitty", "lazydocker", "lazygit", "mako", "mise",
    "mimeapps.list", "nvim", "omarchy", "satty", "starship.toml", "swayosd", "systemd", "tmux",
    "user-dirs.dirs", "user-dirs.locale", "uwsm", "walker", "waybar", "wireplumber",
    "xdg-desktop-portal", "xournalpp", "yazi", "zellij", "zed", "chromium-flags.conf",
    "brave-flags.conf", "code-flags.conf", "electron-flags.conf",
}

LOCAL_SHARE_SETTINGS = {"applications", "icons", "fonts", "zoxide", "fish", "atuin",
                        "nautilus-python", "themes", "color-schemes", "konsole"}

LOCAL_SHARE_SKIP = {
    "omarchy": "part of Omarchy itself",
    "nvim": "Neovim reinstalls its plugins on first start",
    "mise": "mise reinstalls tools from your config",
    "flatpak": "Flatpak apps are reinstalled instead",
    "Trash": "deleted files",
    "recently-used.xbel": "recent file history points at the trial",
    "xorg": "session logs",
    "gvfs-metadata": "file manager cache",
    "baloo": "search index",
    "tracker3": "search index",
    "webkitgtk": "cache",
    "sddm": "login screen state",
    "try-omarchy": "Try Omarchy state",
    "try-omarchy-import": "the importer's own backups",
}

LOCAL_STATE_OMARCHY_SETTINGS = {"toggles", "theme-backgrounds"}

EXACT_SKIP = {
    ".local/state/omarchy/toggles/suspend-off":
        "Try Omarchy hides Suspend because a virtual machine cannot suspend",
    ".config/hypr/monitors.lua": "display settings belong to this computer",
    ".config/hypr/monitors.conf": "display settings belong to this computer",
    ".config/hypr/input.lua.before-try-omarchy-pinch": "Try Omarchy's backup of input.lua",
    ".ssh/authorized_keys": "controls who could sign in to the trial over SSH",
    ".local/share/applications/mimeinfo.cache": "rebuilt automatically",
    ".local/share/icons/hicolor/icon-theme.cache": "rebuilt automatically",
    ".gnupg/random_seed": "rebuilt automatically",
    ".config/btop/themes/current.theme": "recreated when the theme is set",
    ".claude/themes/omarchy.json": "recreated when the theme is set",
    ".pi/agent/themes/omarchy-system.json": "recreated when the theme is set",
}

PATTERN_SKIP = (
    (".local/share/applications/try-omarchy-windows-*",
     "Windows app shortcuts only work in Try Omarchy"),
    (".gnupg/*.lock", "lock file"),
    (".gnupg/.#lk*", "lock file"),
    (".ssh/*.lock", "lock file"),
)

# Credentials that live inside otherwise ordinary app folders.
KEY_FILES = {
    ".claude/.credentials.json", ".codex/auth.json", ".gemini/oauth_creds.json",
    ".local/share/opencode/auth.json", ".cargo/credentials", ".cargo/credentials.toml",
    ".config/zed/credentials.json",
}

# Names of cache folders and lock files inside app and browser data.
CACHE_NAMES = {
    "Cache", "Code Cache", "GPUCache", "DawnCache", "DawnGraphiteCache", "DawnWebGPUCache",
    "GrShaderCache", "GraphiteDawnCache", "ShaderCache", "CachedData", "CachedExtensionVSIXs",
    "Crashpad", "component_crx_cache", "extensions_crx_cache", "startupCache", "cache2",
    "Service Worker/CacheStorage", "Service Worker/ScriptCache",
}
LOCK_NAMES = {"SingletonLock", "SingletonSocket", "SingletonCookie", "lockfile", ".parentlock",
              "lock", "parent.lock"}


# Friendlier names for app folders in the selection list.
APP_LABELS = {
    ".claude": "Claude Code", ".codex": "Codex", ".gemini": "Gemini CLI", ".pi": "Pi",
    ".hermes": "Hermes", ".cursor": "Cursor", ".vscode": "VS Code extensions",
    ".vscode-oss": "Code - OSS extensions", ".cargo": "Rust (cargo)", ".m2": "Maven",
    ".gradle": "Gradle", ".android": "Android tools", ".docker": "Docker",
    ".config/Code": "VS Code", ".config/Cursor": "Cursor", ".config/VSCodium": "VSCodium",
    ".config/Slack": "Slack", ".config/discord": "Discord", ".config/Signal": "Signal",
    ".config/spotify": "Spotify", ".config/obsidian": "Obsidian", ".config/Typora": "Typora",
    ".config/libreoffice": "LibreOffice", ".config/zoom.us": "Zoom",
    ".local/share/Steam": "Steam", ".local/share/containers": "Podman containers",
    ".local/share/fish": "fish", ".local/share/opencode": "opencode",
}


def app_label(relative, name):
    if relative in APP_LABELS:
        return APP_LABELS[relative]
    return name.lstrip(".") or name


def link_is_try_only(target):
    return target == "/mnt/host" or target.startswith("/mnt/host/") or "/try-omarchy" in target


class Classifier:
    """Maps home-relative paths to groups.

    skel_config_names: names under .config in the trial's or this computer's
    /etc/skel, which count as Omarchy settings.
    """

    def __init__(self, skel_config_names=()):
        self.skel_config_names = set(skel_config_names)

    def is_container(self, relative):
        return relative in CONTAINERS

    def place(self, relative, kind, link_target=None):
        """Place a direct child of a container (see is_container)."""
        if kind == "symlink" and link_target is not None and link_is_try_only(link_target):
            return skip("points at Try Omarchy's shared Windows folder")
        parent, _, name = relative.rpartition("/")
        if parent == "":
            return self._top(name, kind)
        if parent == ".config":
            return self._config(relative, name)
        if parent == ".local":
            if name in ("share", "state"):
                return Place(CONTAINER)
            if name == "bin":
                return Place(SETTINGS, SETTINGS_GROUP)
            return Place(APPS, f"apps/{relative}", app_label(relative, name))
        if parent == ".local/share":
            if name in LOCAL_SHARE_SETTINGS:
                return Place(SETTINGS, SETTINGS_GROUP)
            if name in ("keyrings", "pki"):
                return Place(KEYS, KEYS_GROUP)
            if name in LOCAL_SHARE_SKIP:
                return skip(LOCAL_SHARE_SKIP[name])
            return Place(APPS, f"apps/{relative}", app_label(relative, name))
        if parent == ".local/state":
            if name == "omarchy":
                return Place(CONTAINER)
            return skip("temporary app state")
        if parent == ".local/state/omarchy":
            if name in LOCAL_STATE_OMARCHY_SETTINGS:
                return Place(SETTINGS, SETTINGS_GROUP)
            return skip("Omarchy keeps its own state on this computer")
        if parent == ".var":
            if name == "app":
                return Place(CONTAINER)
            return skip("Flatpak state")
        if parent == ".var/app":
            return Place(APPS, f"apps/{relative}", f"{name} (Flatpak)")
        raise ValueError(f"not a container child: {relative}")

    def _top(self, name, kind):
        if name in (".config", ".local", ".var"):
            return Place(CONTAINER) if kind == "directory" else Place(SETTINGS, SETTINGS_GROUP)
        if not name.startswith("."):
            if kind == "directory":
                return Place(FILES, f"files/{name}", name)
            return Place(FILES, LOOSE_FILES_GROUP, "Files in your home folder")
        if name in TOP_SKIP:
            return skip(TOP_SKIP[name])
        if name in TOP_KEYS:
            return Place(KEYS, KEYS_GROUP)
        if name in BROWSERS:
            identity, label = BROWSERS[name]
            return Place(BROWSER, f"browser/{identity}", label)
        if kind != "directory" or name in TOP_SETTINGS_DIRS:
            return Place(SETTINGS, SETTINGS_GROUP)
        return Place(APPS, f"apps/{name}", app_label(name, name))

    def _config(self, relative, name):
        if relative in BROWSERS:
            identity, label = BROWSERS[relative]
            return Place(BROWSER, f"browser/{identity}", label)
        if name in CONFIG_KEYS:
            return Place(KEYS, KEYS_GROUP)
        if name in CONFIG_SKIP:
            return skip(CONFIG_SKIP[name])
        if name in CONFIG_SETTINGS or name in self.skel_config_names:
            return Place(SETTINGS, SETTINGS_GROUP)
        return Place(APPS, f"apps/{relative}", app_label(relative, name))

    def override(self, relative, place, kind, link_target=None):
        """Adjust the place of a path inside a group. Returns a Place."""
        if relative in EXACT_SKIP:
            return skip(EXACT_SKIP[relative])
        for pattern, reason in PATTERN_SKIP:
            if fnmatch.fnmatchcase(relative, pattern):
                return skip(reason)
        if kind == "symlink" and link_target is not None and link_is_try_only(link_target):
            return skip("points at Try Omarchy")
        if kind == "special":
            return skip("sockets and devices are not copied")
        if relative in KEY_FILES:
            return Place(KEYS, KEYS_GROUP)
        if place.kind in (APPS, BROWSER):
            name = relative.rpartition("/")[2]
            if kind == "directory" and (name in CACHE_NAMES or any(
                    relative.endswith("/" + cache) for cache in CACHE_NAMES if "/" in cache)):
                return skip("cache")
            if name in LOCK_NAMES:
                return skip("lock file")
        return place
