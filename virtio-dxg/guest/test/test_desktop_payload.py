"""CPU/data controls only. Synthetic files are never loaded as libraries."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
VERSION = (HERE.parent / 'mesa/CURRENT').read_text().strip()


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


payload = module('desktop_payload', HERE / 'initcpio/desktop_payload.py')
omarchy_ui = module('omarchy_ui_boost', HERE / 'initcpio/omarchy_ui_boost.py')
build = module('record_build', HERE.parent / 'mesa/record_build.py')


class DesktopData(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / 'VERSION').write_text(VERSION + '\n')
        self.recipe = self.root / 'recipe'
        self.recipe.mkdir()
        manifest = json.loads((HERE.parent / 'mesa/versions' / VERSION / 'recipe.json').read_text())
        (self.recipe / 'recipe.json').write_text(json.dumps(manifest))
        self.stage = self.root / 'stage'
        for name in manifest['runtimeRequiredPaths']:
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if name not in manifest['runtimeAliases']:
                path.write_bytes(b'CPU fixture; not an ELF\\n' + name.encode())
        for name, target in manifest['runtimeAliases'].items():
            (self.stage / name).symlink_to(target)
        icd = self.stage / 'share/vulkan/icd.d/dzn_icd.x86_64.json'
        icd.parent.mkdir(parents=True, exist_ok=True)
        icd.write_text(json.dumps({'ICD': {'library_path': '/usr/lib/libvulkan_dzn.so'}}))
        (self.stage / 'include').mkdir()
        (self.stage / 'include/gbm.h').write_text('Build-only SDK header; not shipped')
        build.record(self.source, self.stage, self.recipe)

    def archive(self):
        path = self.root / 'payload.tar'
        with tarfile.open(path, 'w', dereference=False) as tar:
            for name in ('lib', 'share'):
                tar.add(self.stage / name, arcname=name)
        return path

    def extract(self):
        return payload.extract(self.archive(), self.root / 'installed', self.recipe / 'recipe.json')

    def test_valid_cohort(self):
        self.assertEqual(self.extract(), VERSION)
        # The session user loads these libraries: the staged directory must
        # not keep mkdtemp's owner-only mode.
        self.assertEqual((self.root / 'installed').stat().st_mode & 0o055, 0o055)
        data = json.loads((self.root / 'installed/share/vulkan/icd.d/dzn_icd.x86_64.json').read_text())
        self.assertEqual(data['ICD']['library_path'], str(self.root / 'installed/lib/libvulkan_dzn.so'))
        self.assertEqual((self.root / 'installed/lib/libgbm.so').read_bytes(),
                         (self.root / 'installed/lib/libgbm.so.1').read_bytes())

    def test_corrupt_component(self):
        (self.stage / 'lib/libgbm.so.1').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Changed component'):
            self.extract()
        self.assertFalse((self.root / 'installed').exists())

    def test_same_size_corruption_before_publication(self):
        path = self.stage / 'lib/libvulkan_dzn.so'
        original = path.read_bytes()
        path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        with self.assertRaisesRegex(ValueError, 'Changed component'):
            self.extract()
        self.assertFalse((self.root / 'installed').exists())

    def test_receipt_covers_only_shipped_roots(self):
        inventory = json.loads((self.stage / 'lib/virtio-dxg-build.json').read_text())['files']
        self.assertNotIn('include/gbm.h', inventory)
        self.assertTrue(inventory)
        self.assertTrue(all(name.split('/', 1)[0] in ('lib', 'share') for name in inventory))

    def test_missing_gpu_loader_rejected_even_with_matching_inventory(self):
        name = 'lib/dri/d3d12_dri.so'
        (self.stage / name).unlink()
        receipt = self.stage / 'lib/virtio-dxg-build.json'
        data = json.loads(receipt.read_text())
        del data['files'][name]
        receipt.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'Incomplete graphics cohort'):
            self.extract()

    def test_wrong_loader_alias_rejected_at_build(self):
        path = self.stage / 'lib/dri/d3d12_dri.so'
        path.unlink()
        path.symlink_to(f'../libgallium-{VERSION}.so')
        with self.assertRaisesRegex(ValueError, 'Incorrect runtime alias'):
            build.record(self.source, self.stage, self.recipe)

    def test_partial_cohort(self):
        (self.stage / 'lib/libEGL_mesa.so.0').unlink()
        with self.assertRaisesRegex(ValueError, 'inventory differs'):
            self.extract()

    def test_mismatched_recipe(self):
        with (self.recipe / 'recipe.json').open('a') as stream:
            stream.write('\n')
        with self.assertRaisesRegex(ValueError, 'recipes differ'):
            self.extract()

    def test_wrong_version_label(self):
        (self.stage / 'lib/virtio-dxg-mesa-version').write_text('26.2.4')
        with self.assertRaisesRegex(ValueError, 'version label'):
            self.extract()

    def test_no_receipt(self):
        (self.stage / 'lib/virtio-dxg-build.json').unlink()
        with self.assertRaisesRegex(ValueError, 'receipt'):
            self.extract()

    def test_existing_destination(self):
        (self.root / 'installed').mkdir()
        with self.assertRaisesRegex(ValueError, 'Destination exists'):
            self.extract()

    def test_symlink_escape(self):
        link = self.stage / 'lib/libgbm.so'
        link.unlink()
        link.symlink_to('../../outside')
        with self.assertRaises((ValueError, FileNotFoundError)):
            build.record(self.source, self.stage, self.recipe)

    def test_unsafe_names(self):
        for name in ('', '.', '/lib/x', 'lib/../x', 'lib//x', 'lib\\x', 'C:/x', 'etc/passwd'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                payload.canonical(name)

    def test_desktop_actions_and_urls(self):
        original = ('[Desktop Entry]\nName=Chromium\nName[fr]=Navigateur\n'
                    'Exec=/usr/bin/chromium %U\nTryExec=chromium\nActions=new-window;\n'
                    '[Desktop Action new-window]\nExec=chromium --new-window %U\n')
        changed = payload.desktop_entry(original)
        self.assertEqual(changed.count('Exec=/run/virtio-dxg/bin/chromium'), 3)
        self.assertIn('--new-window %U\n', changed)
        self.assertIn('Name[fr]=Navigateur\n', changed)

    def test_unknown_desktop_not_rewritten(self):
        with self.assertRaises(ValueError):
            payload.desktop_entry('Exec=/opt/another-browser %U\n')

    def test_overlay_preparation_is_data_only(self):
        binary = self.root / 'usr/bin/chromium'
        binary.parent.mkdir(parents=True)
        binary.write_text('not executed')
        desktop = self.root / 'usr/share/applications/chromium.desktop'
        desktop.parent.mkdir(parents=True)
        desktop.write_text('Exec=/usr/bin/chromium %U\n')
        self.assertTrue(payload.prepare_browser(self.root, HERE / 'initcpio/chromium-wrapper'))
        unit = (self.root / 'run/systemd/system/virtio-dxg-bin.service').read_text()
        self.assertIn('lowerdir=/run/virtio-dxg/applications:', unit)
        self.assertIn('upperdir=/run/virtio-dxg/app-upper', unit)
        self.assertTrue((self.root / 'run/systemd/system/sysinit.target.wants/virtio-dxg-bin.service').is_symlink())

    def test_wrapper_arguments_without_browser(self):
        # Only redirect exec to an owned argv recorder. No browser starts.
        stub = self.root / 'argv-recorder'
        stub.write_text('#!/usr/bin/env python3\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n')
        stub.chmod(0o755)
        wrapper = self.root / 'chromium-wrapper'
        wrapper.write_text((HERE / 'initcpio/chromium-wrapper').read_text().replace(
            'exec /usr/bin/chromium', 'exec ' + str(stub)))
        arguments = ['--new-window', 'https://example.test/a b?c=1&d=2']
        for display, passthrough, disable in [(' :1', '', False), ('', '', False),
                                             (':1', 'passthrough', False), (':1', '', True)]:
            argv = arguments + (['--disable-gpu'] if disable else [])
            env = dict(os.environ, DISPLAY=display, VIRTIO_DXG_CHROMIUM=passthrough)
            result = subprocess.run(['sh', str(wrapper), *argv], env=env,
                                    capture_output=True, text=True, check=True, timeout=5)
            observed = json.loads(result.stdout)
            self.assertEqual(observed[:len(argv)], argv)
            self.assertNotIn('--no-sandbox', observed)
            if display and not passthrough and not disable:
                self.assertIn('--use-gl=angle', observed)
                self.assertIn('--disable-accelerated-video-encode', observed)
            else:
                self.assertEqual(observed, argv)

    def test_launch_paths_stages_xdg_settings_fast_without_browser(self):
        xdg_bin = self.root / 'usr/bin/xdg-settings'
        xdg_bin.parent.mkdir(parents=True)
        xdg_bin.write_text('#!/bin/sh\necho fallback:$*\n')
        xdg_bin.chmod(0o755)
        self.assertTrue(payload.prepare_launch_paths(
            self.root, xdg_settings_fast=HERE / 'initcpio/xdg-settings-fast'))
        staged = self.root / 'run/virtio-dxg/bin/xdg-settings'
        self.assertTrue(staged.is_file())
        unit = (self.root / 'run/systemd/system/virtio-dxg-bin.service').read_text()
        self.assertIn('lowerdir=/run/virtio-dxg/bin:/usr/local/bin', unit)
        self.assertNotIn('/usr/share/applications', unit)

    def test_xdg_settings_fast_default_browser_and_fallback(self):
        cfg = self.root / 'cfg'
        apps = self.root / 'data/applications'
        cfg.mkdir(parents=True)
        apps.mkdir(parents=True)
        (apps / 'chromium.desktop').write_text('[Desktop Entry]\nName=Chromium\n')
        (cfg / 'mimeapps.list').write_text(
            '[Default Applications]\n'
            'text/html=other.desktop\n'
            'x-scheme-handler/https=chromium.desktop;\n'
        )
        stub = self.root / 'fallback-xdg-settings'
        stub.write_text('#!/bin/sh\necho "FALLBACK:$*"\n')
        stub.chmod(0o755)
        fast = self.root / 'xdg-settings-fast'
        fast.write_text((HERE / 'initcpio/xdg-settings-fast').read_text().replace(
            '/usr/bin/xdg-settings', str(stub)))
        env = dict(os.environ, XDG_CONFIG_HOME=str(cfg),
                   XDG_DATA_HOME=str(self.root / 'data'),
                   XDG_DATA_DIRS=str(self.root / 'empty'))
        res = subprocess.run(['bash', str(fast), 'get', 'default-web-browser'],
                             env=env, capture_output=True, text=True, check=True, timeout=5)
        self.assertEqual(res.stdout.strip(), 'chromium.desktop')
        res2 = subprocess.run(['bash', str(fast), 'check', 'default-web-browser', 'chromium.desktop'],
                              env=env, capture_output=True, text=True, check=True, timeout=5)
        self.assertEqual(res2.stdout.strip(), 'FALLBACK:check default-web-browser chromium.desktop')

    def test_app_prefetch_list_and_syntax(self):
        fake_apps = self.root / 'apps/applications'
        fake_apps.mkdir(parents=True)
        (fake_apps / 'test.desktop').write_text(
            '[Desktop Entry]\nType=Application\nExec=/bin/sh -c true\n'
        )
        env = dict(os.environ, XDG_DATA_HOME=str(self.root / 'apps'),
                   XDG_DATA_DIRS=str(self.root / 'empty'),
                   XDG_CACHE_HOME=str(self.root / 'cache'))
        res = subprocess.run(['bash', str(HERE / 'initcpio/app-prefetch'), '--list'],
                             env=env, capture_output=True, text=True, check=True, timeout=10)
        self.assertIn('/sh', res.stdout)
        subprocess.run(['bash', '-n', str(HERE / 'initcpio/desktop-boost')], check=True, timeout=5)
        subprocess.run(['bash', '-n', str(HERE / 'initcpio/browser-boost')], check=True, timeout=5)
        subprocess.run(['sh', '-n', str(HERE / 'initcpio/desktop-priority')], check=True, timeout=5)
        subprocess.run(['sh', '-n', str(HERE / 'initcpio/chromium-wrapper')], check=True, timeout=5)
        subprocess.run(['sh', '-n', str(HERE / 'initcpio/app-fastlaunch')], check=True, timeout=5)
        subprocess.run(['sh', '-n', str(HERE / 'initcpio/virtio_dxg')], check=True, timeout=5)

    def test_app_fastlaunch_stages_installed_apps_and_desktops_only(self):
        for app in ('foot', 'code', 'nautilus', 'omacalc', 'gnome-calculator', 'vkcube',
                    'uwsm-app', 'gtk-launch', 'xdg-terminal-exec'):
            p = self.root / 'usr/bin' / app
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('#!/bin/sh\n')
        omarchy_bin = self.root / 'usr/share/omarchy/bin'
        omarchy_bin.mkdir(parents=True, exist_ok=True)
        (omarchy_bin / 'omarchy-launch-webapp').write_text('#!/bin/sh\n')
        apps_dir = self.root / 'usr/share/applications'
        apps_dir.mkdir(parents=True, exist_ok=True)
        (apps_dir / 'foot.desktop').write_text('[Desktop Entry]\nExec=foot\n')
        (apps_dir / 'omacalc.desktop').write_text('[Desktop Entry]\nExec=/usr/bin/omacalc\n')
        (apps_dir / 'uwsm-app.desktop').write_text('[Desktop Entry]\nExec=uwsm-app\n')
        self.assertTrue(payload.prepare_launch_paths(
            self.root, app_fastlaunch=HERE / 'initcpio/app-fastlaunch'))
        for app in ('foot', 'code', 'nautilus', 'omacalc', 'gnome-calculator', 'vkcube',
                    'uwsm-app', 'gtk-launch', 'xdg-terminal-exec'):
            self.assertTrue((self.root / 'run/virtio-dxg/bin' / app).is_file())
        self.assertTrue((self.root / 'run/virtio-dxg/omarchy-bin/omarchy-launch-webapp').is_file())
        self.assertTrue((self.root / 'run/virtio-dxg/omarchy-orig/omarchy-launch-webapp').is_file())
        self.assertFalse((self.root / 'run/virtio-dxg/omarchy-bin/omarchy-launch-nautilus').exists())
        self.assertFalse((self.root / 'run/virtio-dxg/bin/blender').exists())
        self.assertFalse((self.root / 'run/virtio-dxg/applications/uwsm-app.desktop').exists())
        unit = (self.root / 'run/systemd/system/virtio-dxg-bin.service').read_text()
        self.assertIn('/usr/share/omarchy/bin', unit)
        self.assertEqual(
            (self.root / 'run/virtio-dxg/applications/foot.desktop').read_text(),
            '[Desktop Entry]\nExec=/run/virtio-dxg/bin/foot\n',
        )
        self.assertEqual(
            (self.root / 'run/virtio-dxg/applications/omacalc.desktop').read_text(),
            '[Desktop Entry]\nExec=/run/virtio-dxg/bin/omacalc\n',
        )

    def _write_omarchy_fixtures(self, include_dock_group=True):
        files = {
            omarchy_ui.APP_LIBRARY_REL: (
                'Item {\n'
                '  property var iconIndex: ({})\n'
                '  property var pendingIconIndex: ({})\n'
                '  function refreshIcons() {\n'
                '    if (!iconIndexScan.running) iconIndexScan.running = true\n'
                '  }\n'
                '  Process {\n'
                '    onExited: root.iconIndex = root.pendingIconIndex\n'
                '  }\n'
                '}\n'
            ),
            omarchy_ui.MENU_QML_REL: (
                'Item {\n'
                '  function openExistingMenu(initialMenu) {\n'
                '    root.disarmPointer()\n'
                '    root.evaluateGuards()\n'
                '    opened = true\n'
                '    rebuildDisplay()\n'
                '  }\n'
                '  property var disabledResults: ({})   // id \u2192 true|false (dim, skip cursor)\n'
                '  property bool guardsPending: false\n'
                '  Process {\n'
                '    onExited: {\n'
                '      root.whenResults = nextWhen\n'
                '      root.checkedResults = nextChecked\n'
                '      root.disabledResults = nextDisabled\n'
                '    }\n'
                '  }\n'
                '}\n'
            ),
            omarchy_ui.OVERLAY_WINDOW_REL: (
                'PanelWindow {\n'
                '  onContentReadyChanged: {\n'
                '    contentRevealed = false\n'
                '    // Resize and scale notifications arrive while Qt is still updating the\n'
                '    // window. Reveal on the next turn, once its content layout has caught up.\n'
                '    if (contentReady) Qt.callLater(function() {\n'
                '      if (window.contentReady) window.contentRevealed = true\n'
                '    })\n'
                '  }\n'
                '  onShownChanged: {\n'
                '    targetScreen = shown ? focusedScreen() : null\n'
                '    if (shown) Hyprland.refreshMonitors()\n'
                '  }\n'
                '  Connections {\n'
                '    function onScreensChanged() {\n'
                '      if (window.shown && Quickshell.screens.indexOf(window.targetScreen) < 0) {\n'
                '        window.targetScreen = null\n'
                '        // Let the removed output finish closing its old window before mapping\n'
                '        // a replacement, rather than recreating it during Qt\'s teardown.\n'
                '        Qt.callLater(function() {\n'
                '          if (window.shown && !window.targetScreen) window.targetScreen = window.focusedScreen()\n'
                '        })\n'
                '      }\n'
                '    }\n'
                '  }\n'
                '}\n'
            ),
        }
        if include_dock_group:
            files.update({
                omarchy_ui.SHORTCUTS_REL: 'ipc notifications.act\nipc notifications.showHistory\n',
                omarchy_ui.UTILITIES_LUA_REL: 'o.bind("SUPER + ALT + SPACE", "Apps", "omarchy-shell omarchy.dock apps")\n',
                omarchy_ui.SHELL_QML_REL: (
                    'readonly property var ipcShortcutServices: '
                    '({ media: "omarchy.media", notifications: "omarchy.notifications" })\n'
                ),
                omarchy_ui.DOCK_QML_REL: '  function openApps() {\n    appGrid.open()\n  }\n',
            })
        for rel, content in files.items():
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content, encoding='utf-8')

    def test_omarchy_ui_boost_stages_all_overlays_and_unit(self):
        self._write_omarchy_fixtures(include_dock_group=True)
        staged = omarchy_ui.prepare_omarchy_ui(self.root)
        self.assertEqual(len(staged), 7)
        app_lib = (self.root / 'run/virtio-dxg/omarchy-ui' / omarchy_ui.APP_LIBRARY_REL).read_text(encoding='utf-8')
        self.assertIn('property bool iconIndexReady: false', app_lib)
        self.assertIn('if (root.iconIndexReady && !force) return', app_lib)
        menu = (self.root / 'run/virtio-dxg/omarchy-ui' / omarchy_ui.MENU_QML_REL).read_text(encoding='utf-8')
        self.assertIn('deferredGuardTimer.restart()', menu)
        self.assertNotIn('    root.evaluateGuards()\n    opened = true', menu)
        overlay = (self.root / 'run/virtio-dxg/omarchy-ui' / omarchy_ui.OVERLAY_WINDOW_REL).read_text(encoding='utf-8')
        self.assertIn('contentRevealed = contentReady', overlay)
        shortcuts = (self.root / 'run/virtio-dxg/omarchy-ui' / omarchy_ui.SHORTCUTS_REL).read_text(encoding='utf-8')
        self.assertIn('ipc dock.apps\n', shortcuts)
        unit = (self.root / 'run/systemd/system/virtio-dxg-omarchy-ui.service').read_text(encoding='utf-8')
        self.assertIn('remount,bind,ro /usr/share/omarchy/shell/plugins/menu/Menu.qml', unit)
        self.assertTrue((self.root / 'run/systemd/system/sysinit.target.wants/virtio-dxg-omarchy-ui.service').is_symlink())

    def test_omarchy_ui_boost_skips_partial_dock_group_or_absent_files(self):
        self.assertEqual(omarchy_ui.prepare_omarchy_ui(self.root), [])
        self._write_omarchy_fixtures(include_dock_group=False)
        staged = omarchy_ui.prepare_omarchy_ui(self.root)
        self.assertEqual(staged, sorted([
            omarchy_ui.OVERLAY_WINDOW_REL,
            omarchy_ui.MENU_QML_REL,
            omarchy_ui.APP_LIBRARY_REL,
        ]))

    def test_desktop_priority_avoids_rt_on_quickshell_and_silences_proc_reads(self):
        text = (HERE / 'initcpio/desktop-priority').read_text(encoding='utf-8')
        self.assertIn('rttime_unlimited()', text)
        self.assertIn('Max realtime timeout"*"unlimited"', text)
        self.assertIn('2>/dev/null < "/proc/$1/limits"', text)
        self.assertIn('rttime_unlimited "$p"', text)
        qs_section = text.split('quickshell)', 1)[1].split(';;\n        swayosd-server', 1)[0]
        self.assertIn('cls=other; prio=0; target=-20', qs_section)
        self.assertNotIn('cls=rr', qs_section)
        self.assertIn('Xwayland)', text)
        self.assertRegex(text, r'2>/dev/null < "\$pdir/comm"')
        self.assertNotRegex(text, r'< "\$[^"]+" 2>/dev/null')

    def test_app_fastlaunch_execs_without_post_close_relaunch(self):
        import shutil
        fastlaunch_sh = (HERE / 'initcpio/app-fastlaunch').read_text(encoding='utf-8')
        self.assertIn('exec "$fast" "$prog" "$@"', fastlaunch_sh)
        self.assertNotIn('rc=$?', fastlaunch_sh)
        gcc = shutil.which('gcc')
        if gcc:
            src = HERE / 'initcpio/vdxg_boost_helpers.c'
            targets = [
                ('-DBUILD_GAPP_HOLD', ['-shared', '-fPIC'], 'libvdxg-gapp-hold.so', ['-ldl', '-lpthread']),
                ('-DBUILD_WL_GATE', ['-shared', '-fPIC'], 'libvdxg-wl-gate.so', ['-ldl', '-lpthread']),
                ('-DBUILD_FASTLAUNCH', [], 'vdxg-fastlaunch', []),
            ]
            for define, extra_flags, out_name, libs in targets:
                out_path = self.root / out_name
                cmd = [gcc, '-O2', '-Wall', '-Wextra', '-Werror', define, *extra_flags,
                       str(src), '-o', str(out_path), *libs]
                subprocess.run(cmd, check=True, timeout=15)

    def test_browser_boost_stage_timeout_and_client_poll(self):
        text = (HERE / 'initcpio/browser-boost').read_text(encoding='utf-8')
        self.assertIn('vdxg-pso/.ready', text)
        self.assertIn('deadline = time.monotonic() + 25.0', text)
        self.assertIn('existing_addrs = ', text)
        self.assertIn('next_poll = 0.0', text)


if __name__ == '__main__':
    unittest.main()
