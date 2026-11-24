#!/usr/bin/env python3
"""Stages non-destructive tmpfs bind-mount overlays for Omarchy shell UI speed.

Rewrites read-only shell QML/Lua files into /run/virtio-dxg/omarchy-ui/ and
registers a boot-local systemd mount unit so menu and app-launcher opens never
block on synchronous subprocess forks or redundant full-screen redraw delays,
while leaving the guest disk untouched.
"""
import argparse
from pathlib import Path
import shutil

APP_LIBRARY_REL = 'usr/share/omarchy/shell/services/AppLibrary.qml'
MENU_QML_REL = 'usr/share/omarchy/shell/plugins/menu/Menu.qml'
OVERLAY_WINDOW_REL = 'usr/share/omarchy/shell/Ui/OverlayWindow.qml'
SHORTCUTS_REL = 'usr/share/omarchy/default/omarchy/shortcuts'
UTILITIES_LUA_REL = 'usr/share/omarchy/default/hypr/bindings/utilities.lua'
SHELL_QML_REL = 'usr/share/omarchy/shell/shell.qml'
DOCK_QML_REL = 'usr/share/omarchy/shell/plugins/dock/Dock.qml'

PATCHES = {
    APP_LIBRARY_REL: [
        (
            '  property var iconIndex: ({})\n'
            '  property var pendingIconIndex: ({})\n',
            '  property var iconIndex: ({})\n'
            '  property var pendingIconIndex: ({})\n'
            '  property bool iconIndexReady: false\n',
        ),
        (
            '  function refreshIcons() {\n'
            '    if (!iconIndexScan.running) iconIndexScan.running = true\n'
            '  }\n',
            '  function refreshIcons(force) {\n'
            '    if (root.iconIndexReady && !force) return\n'
            '    if (!iconIndexScan.running) iconIndexScan.running = true\n'
            '  }\n',
        ),
        (
            '    onExited: root.iconIndex = root.pendingIconIndex\n',
            '    onExited: {\n'
            '      root.iconIndex = root.pendingIconIndex\n'
            '      root.iconIndexReady = true\n'
            '    }\n',
        ),
    ],
    MENU_QML_REL: [
        (
            '    root.disarmPointer()\n'
            '    root.evaluateGuards()\n'
            '    opened = true\n'
            '    rebuildDisplay()\n',
            '    root.disarmPointer()\n'
            '    opened = true\n'
            '    rebuildDisplay()\n'
            '    if (!root.guardsEvaluatedOnce) root.evaluateGuards()\n'
            '    else if (Date.now() - root.lastGuardEvalMs > 10000) deferredGuardTimer.restart()\n',
        ),
        (
            '  property var disabledResults: ({})   // id \u2192 true|false (dim, skip cursor)\n'
            '  property bool guardsPending: false\n',
            '  property var disabledResults: ({})   // id \u2192 true|false (dim, skip cursor)\n'
            '  property bool guardsPending: false\n'
            '  property bool guardsEvaluatedOnce: false\n'
            '  property double lastGuardEvalMs: 0\n\n'
            '  Timer {\n'
            '    id: deferredGuardTimer\n'
            '    interval: 350\n'
            '    repeat: false\n'
            '    onTriggered: if (Date.now() - root.lastGuardEvalMs > 10000) root.evaluateGuards()\n'
            '  }\n',
        ),
        (
            '      root.whenResults = nextWhen\n'
            '      root.checkedResults = nextChecked\n'
            '      root.disabledResults = nextDisabled\n',
            '      root.whenResults = nextWhen\n'
            '      root.checkedResults = nextChecked\n'
            '      root.disabledResults = nextDisabled\n'
            '      root.guardsEvaluatedOnce = true\n'
            '      root.lastGuardEvalMs = Date.now()\n',
        ),
    ],
    OVERLAY_WINDOW_REL: [
        (
            '  onContentReadyChanged: {\n'
            '    contentRevealed = false\n'
            '    // Resize and scale notifications arrive while Qt is still updating the\n'
            '    // window. Reveal on the next turn, once its content layout has caught up.\n'
            '    if (contentReady) Qt.callLater(function() {\n'
            '      if (window.contentReady) window.contentRevealed = true\n'
            '    })\n'
            '  }\n',
            '  onContentReadyChanged: {\n'
            '    contentRevealed = contentReady\n'
            '  }\n',
        ),
        (
            '  onShownChanged: {\n'
            '    targetScreen = shown ? focusedScreen() : null\n'
            '    if (shown) Hyprland.refreshMonitors()\n'
            '  }\n',
            '  onShownChanged: {\n'
            '    if (shown) {\n'
            '      var nextScreen = focusedScreen()\n'
            '      if (targetScreen !== nextScreen) targetScreen = nextScreen\n'
            '      if (!targetMonitor || Math.round(devicePixelRatio * 120) !== Math.round(targetMonitor.scale * 120))\n'
            '        Hyprland.refreshMonitors()\n'
            '    }\n'
            '  }\n',
        ),
        (
            '    function onScreensChanged() {\n'
            '      if (window.shown && Quickshell.screens.indexOf(window.targetScreen) < 0) {\n'
            '        window.targetScreen = null\n'
            '        // Let the removed output finish closing its old window before mapping\n'
            '        // a replacement, rather than recreating it during Qt\'s teardown.\n'
            '        Qt.callLater(function() {\n'
            '          if (window.shown && !window.targetScreen) window.targetScreen = window.focusedScreen()\n'
            '        })\n'
            '      }\n'
            '    }\n',
            '    function onScreensChanged() {\n'
            '      if (Quickshell.screens.indexOf(window.targetScreen) < 0) {\n'
            '        window.targetScreen = null\n'
            '        if (window.shown) {\n'
            '          Qt.callLater(function() {\n'
            '            if (window.shown && !window.targetScreen) window.targetScreen = window.focusedScreen()\n'
            '          })\n'
            '        }\n'
            '      }\n'
            '    }\n',
        ),
    ],
}

DOCK_SHORTCUT_GROUP = {
    SHORTCUTS_REL: [
        (
            'ipc notifications.showHistory\n',
            'ipc notifications.showHistory\nipc dock.apps\n',
        ),
    ],
    UTILITIES_LUA_REL: [
        (
            'o.bind("SUPER + ALT + SPACE", "Apps", "omarchy-shell omarchy.dock apps")',
            'o.bind("SUPER + ALT + SPACE", "Apps", { ipc = "dock.apps" })',
        ),
    ],
    SHELL_QML_REL: [
        (
            'readonly property var ipcShortcutServices: ({ media: "omarchy.media", notifications: "omarchy.notifications" })',
            'readonly property var ipcShortcutServices: ({ media: "omarchy.media", notifications: "omarchy.notifications", dock: "omarchy.dock" })',
        ),
    ],
    DOCK_QML_REL: [
        (
            '  function openApps() {\n'
            '    appGrid.open()\n'
            '  }\n',
            '  function openApps() {\n'
            '    appGrid.open()\n'
            '  }\n\n'
            '  function runShortcut(method) {\n'
            '    if (method === "apps") {\n'
            '      appGrid.toggle()\n'
            '      return true\n'
            '    }\n'
            '    return false\n'
            '  }\n',
        ),
    ],
}


def _apply_replacements(path, replacements):
    if not path.is_file():
        return None
    text = path.read_text(encoding='utf-8')
    for old, new in replacements:
        if new in text:
            continue
        if text.count(old) != 1:
            return None
        text = text.replace(old, new, 1)
    return text


def prepare_omarchy_ui(root):
    root = Path(root)
    stage_base = root / 'run/virtio-dxg/omarchy-ui'
    staged = {}

    for relpath, replacements in PATCHES.items():
        updated = _apply_replacements(root / relpath, replacements)
        if updated is not None:
            staged[relpath] = updated

    group_staged = {}
    for relpath, replacements in DOCK_SHORTCUT_GROUP.items():
        updated = _apply_replacements(root / relpath, replacements)
        if updated is None:
            group_staged.clear()
            break
        group_staged[relpath] = updated
    staged.update(group_staged)

    if not staged:
        return []

    if stage_base.exists():
        shutil.rmtree(stage_base)
    starts = []
    stops = []
    for relpath in sorted(staged):
        dst = stage_base / relpath
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(staged[relpath], encoding='utf-8')
        dst.chmod(0o644)
        starts.append(f'ExecStart=/usr/bin/mount -o bind /run/virtio-dxg/omarchy-ui/{relpath} /{relpath}')
        starts.append(f'ExecStart=/usr/bin/mount -o remount,bind,ro /{relpath}')
        stops.insert(0, f'ExecStop=-/usr/bin/umount /{relpath}')

    unit = root / 'run/systemd/system/virtio-dxg-omarchy-ui.service'
    unit.parent.mkdir(parents=True, exist_ok=True)
    unit.write_text(
        '[Unit]\n'
        'Description=virtio-dxg instant Omarchy shell UI overlays for this boot\n'
        'DefaultDependencies=no\n'
        'After=local-fs.target\n'
        'Before=sysinit.target systemd-user-sessions.service\n\n'
        '[Service]\n'
        'Type=oneshot\n' +
        '\n'.join(starts) + '\n' +
        '\n'.join(stops) + '\n'
        'RemainAfterExit=yes\n',
        encoding='utf-8',
    )
    wants = unit.parent / 'sysinit.target.wants'
    wants.mkdir(exist_ok=True)
    link = wants / unit.name
    if not link.exists() and not link.is_symlink():
        link.symlink_to('../' + unit.name)
    return sorted(staged)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', type=Path, default=Path('/'))
    args = parser.parse_args()
    mounted = prepare_omarchy_ui(args.root)
    print(f'virtio-dxg: staged {len(mounted)} Omarchy UI overlay(s)')
