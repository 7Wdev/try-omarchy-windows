"""Prepare boot-local graphics data. Never import or execute a graphics library."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import tempfile

LIMIT = 512 * 1024 ** 2
DESKTOP_REWRITE_APPS = (
    'chromium',
    'code',
    'foot',
    'alacritty',
    'ghostty',
    'nautilus',
    'omacalc',
    'gnome-calculator',
    'vkcube',
    'blender',
)
LAUNCHER_HELPER_APPS = (
    'uwsm-app',
    'gtk-launch',
    'xdg-terminal-exec',
    'omarchy-launch-webapp',
    'omarchy-launch-browser',
    'omarchy-cmd-browser-handoff',
    'omarchy-launch-nautilus',
    'omarchy-launch-terminal',
)
FASTLAUNCH_APPS = DESKTOP_REWRITE_APPS + LAUNCHER_HELPER_APPS


def canonical(name):
    path = PurePosixPath(name)
    if (not name or not path.parts or path.is_absolute() or '\\' in name or ':' in name or
            '..' in path.parts or path.as_posix() != name.rstrip('/') or
            path.parts[0] not in ('lib', 'share')):
        raise ValueError('Unsupported payload path: ' + name)
    return path.as_posix()


def _extract_cohort(archive, destination, recipe, config, post_extract=None):
    """Reject a partial/mixed cohort before it can select session libraries."""
    expected = json.loads(recipe.read_text(encoding='utf-8'))
    if destination.exists() or archive.stat().st_size > LIMIT:
        raise ValueError('Destination exists or archive exceeds limit')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, 'r:') as tar:
        entries = tar.getmembers()
        if len(entries) > 10000 or sum(m.size for m in entries) > LIMIT:
            raise ValueError('Expanded payload exceeds limit')
        members = {}
        for member in entries:
            name = canonical(member.name)
            top = name.split('/', 1)[0]
            if top not in config['allowed_roots']:
                raise ValueError('Unsupported payload path: ' + name)
            if name in members or not (member.isdir() or member.isfile() or member.issym()):
                raise ValueError('Duplicate or unsupported member: ' + name)
            if member.size < 0 or member.size > 256 * 1024 ** 2:
                raise ValueError('Member exceeds limit: ' + name)
            members[name] = member
        receipt_member = members.get(config['receipt_path'])
        if not receipt_member or not receipt_member.isfile() or receipt_member.size > 4 * 1024 ** 2:
            raise ValueError('Missing bounded build receipt')
        receipt = json.load(tar.extractfile(receipt_member))
        version_key = config['version_key']
        if (receipt.get('schema') != 1 or receipt.get('profile') != config['profile'] or
                receipt.get(version_key) != expected[version_key] or
                receipt.get('recipeSha256') != hashlib.sha256(recipe.read_bytes()).hexdigest()):
            raise ValueError('Payload and initramfs recipes differ; rebuild the matched pair')
        for key in config['match_keys']:
            if receipt.get(key) != expected[key]:
                raise ValueError('Payload and initramfs recipes differ; rebuild the matched pair')
        inventory = receipt['files']
        names = {n for n, m in members.items() if not m.isdir()}
        expected_names = set(inventory) | {config['receipt_path'], config['version_path']}
        if names != expected_names:
            raise ValueError('Payload inventory differs: missing=' +
                             repr(sorted(expected_names - names)[:12]) +
                             ', unlisted=' + repr(sorted(names - expected_names)[:12]))
        for name, record in inventory.items():
            canonical(name)
            member = members[name]
            if 'symlink' in record:
                if not member.issym() or member.linkname != record['symlink']:
                    raise ValueError('Changed library alias: ' + name)
            elif not member.isfile() or member.size != record['bytes']:
                raise ValueError('Changed component: ' + name)
        version_member = members[config['version_path']]
        if not version_member.isfile() or version_member.size > 64:
            raise ValueError('Invalid payload version label')
        version = tar.extractfile(version_member).read().decode().strip()
        if version != expected[version_key]:
            raise ValueError('Payload version label differs')
        with tempfile.TemporaryDirectory(prefix=config['stage_prefix'],
                                         dir=destination.parent) as temporary:
            stage = Path(temporary)
            tar.extractall(stage, members=entries, filter='data')
            for name, record in inventory.items():
                path = stage / name
                if not path.resolve(strict=True).is_relative_to(stage):
                    raise ValueError('Alias leaves the payload: ' + name)
                # Hash the bytes in tmpfs, so large libraries cross the 9p
                # share only during extraction and use bounded hash buffers.
                if 'symlink' not in record:
                    with path.open('rb') as component:
                        if hashlib.file_digest(component, 'sha256').hexdigest() != record['sha256']:
                            raise ValueError('Changed component: ' + name)
            for name in expected['runtimeRequiredPaths']:
                canonical(name)
                if not (stage / name).is_file():
                    raise ValueError(config['incomplete_error'] + ': ' + name)
            for name, target in expected['runtimeAliases'].items():
                path = stage / name
                if not path.is_symlink() or os.readlink(path) != target:
                    raise ValueError('Incorrect runtime alias: ' + name)
            if post_extract:
                post_extract(stage)
            # mkdtemp makes the stage 0700: the session's (unprivileged)
            # processes must be able to enter it to load these libraries.
            os.chmod(stage, 0o755)
            os.rename(stage, destination)
            # TemporaryDirectory tolerates its renamed source disappearing.
    return version


def extract(archive, destination, recipe):
    """Extract a hash-verified Mesa cohort into destination."""
    def patch_icd(stage):
        icd = stage / 'share/vulkan/icd.d/dzn_icd.x86_64.json'
        data = json.loads(icd.read_text(encoding='utf-8'))
        data['ICD']['library_path'] = str(destination / 'lib/libvulkan_dzn.so')
        icd.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')

    return _extract_cohort(archive, destination, recipe, {
        'allowed_roots': ('lib', 'share'),
        'receipt_path': 'lib/virtio-dxg-build.json',
        'version_path': 'lib/virtio-dxg-mesa-version',
        'version_key': 'mesaVersion',
        'profile': 'virtio-dxg',
        'match_keys': ('sourceArchiveSha256', 'directXHeadersCommit'),
        'stage_prefix': '.virtio-dxg-mesa-',
        'incomplete_error': 'Incomplete graphics cohort',
    }, post_extract=patch_icd)


def extract_gtk(archive, destination, recipe):
    """Extract a hash-verified GTK 4 library cohort into destination."""
    return _extract_cohort(archive, destination, recipe, {
        'allowed_roots': ('lib',),
        'receipt_path': 'lib/virtio-dxg-gtk-build.json',
        'version_path': 'lib/virtio-dxg-gtk-version',
        'version_key': 'gtkVersion',
        'profile': 'virtio-dxg-gtk',
        'match_keys': ('sourceArchiveSha256',),
        'stage_prefix': '.virtio-dxg-gtk4-',
        'incomplete_error': 'Incomplete GTK cohort',
    })


def desktop_entry(text, cmd='chromium'):
    """Rewrite Exec/TryExec for cmd while preserving URLs, field codes, and actions."""
    qcmd = re.escape(cmd)
    pattern = rf'(?m)^Exec=(?:/usr/bin/{qcmd}|{qcmd}|"/usr/bin/{qcmd}")(?=\s|$)'
    result, count = re.subn(pattern, f'Exec=/run/virtio-dxg/bin/{cmd}', text)
    if not count:
        raise ValueError(f'{cmd} desktop entry has no recognized launcher')
    return re.sub(rf'(?m)^TryExec=(?:/usr/bin/{qcmd}|{qcmd})\s*$',
                  f'TryExec=/run/virtio-dxg/bin/{cmd}', result)


def prepare_launch_paths(root, browser_wrapper=None, xdg_settings_fast=None, app_fastlaunch=None):
    """Create data only; systemd mounts the read-only boot-local overlays later."""
    boot = root / 'run/virtio-dxg'
    has_bin = False
    has_apps = False
    staged_desktop_cmds = []

    if browser_wrapper:
        desktop = root / 'usr/share/applications/chromium.desktop'
        if (root / 'usr/bin/chromium').is_file() and desktop.is_file():
            changed = desktop_entry(desktop.read_text(encoding='utf-8'), 'chromium')
            (boot / 'bin').mkdir(parents=True, exist_ok=True)
            (boot / 'applications').mkdir(parents=True, exist_ok=True)
            shutil.copyfile(browser_wrapper, boot / 'bin/chromium')
            (boot / 'bin/chromium').chmod(0o755)
            (boot / 'applications/chromium.desktop').write_text(changed, encoding='utf-8')
            has_bin = True
            has_apps = True

    if xdg_settings_fast and (root / 'usr/bin/xdg-settings').is_file():
        (boot / 'bin').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(xdg_settings_fast, boot / 'bin/xdg-settings')
        (boot / 'bin/xdg-settings').chmod(0o755)
        has_bin = True

    has_omarchy_bin = False
    if app_fastlaunch:
        for cmd in DESKTOP_REWRITE_APPS:
            dst = boot / 'bin' / cmd
            if dst.exists():
                continue  # browser_wrapper keeps precedence on winbuf=0
            if (root / 'usr/bin' / cmd).is_file():
                (boot / 'bin').mkdir(parents=True, exist_ok=True)
                shutil.copyfile(app_fastlaunch, dst)
                dst.chmod(0o755)
                has_bin = True
                staged_desktop_cmds.append(cmd)

        omarchy_src_dir = root / 'usr/share/omarchy/bin'
        omarchy_dst_dir = boot / 'omarchy-bin'
        omarchy_orig_dir = boot / 'omarchy-orig'
        for cmd in LAUNCHER_HELPER_APPS:
            src_exists = (root / 'usr/bin' / cmd).is_file() or (omarchy_src_dir / cmd).is_file()
            if src_exists:
                dst = boot / 'bin' / cmd
                if not dst.exists():
                    (boot / 'bin').mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(app_fastlaunch, dst)
                    dst.chmod(0o755)
                    has_bin = True
            if (omarchy_src_dir / cmd).is_file():
                omarchy_orig_dir.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(omarchy_src_dir / cmd, omarchy_orig_dir / cmd)
                (omarchy_orig_dir / cmd).chmod(0o755)
                omarchy_dst_dir.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(app_fastlaunch, omarchy_dst_dir / cmd)
                (omarchy_dst_dir / cmd).chmod(0o755)
                has_omarchy_bin = True

        apps_dir = root / 'usr/share/applications'
        if staged_desktop_cmds and apps_dir.is_dir():
            for entry_file in sorted(apps_dir.glob('*.desktop')):
                out_file = boot / 'applications' / entry_file.name
                if out_file.exists():
                    continue
                try:
                    raw = entry_file.read_text(encoding='utf-8')
                except OSError:
                    continue
                for cmd in staged_desktop_cmds:
                    try:
                        rewritten = desktop_entry(raw, cmd)
                    except ValueError:
                        continue
                    (boot / 'applications').mkdir(parents=True, exist_ok=True)
                    out_file.write_text(rewritten, encoding='utf-8')
                    has_apps = True
                    break

    if not has_bin and not has_apps and not has_omarchy_bin:
        return False

    starts = []
    stops = []
    if has_bin:
        (boot / 'bin-upper').mkdir(parents=True, exist_ok=True)
        (boot / 'bin-work').mkdir(parents=True, exist_ok=True)
        starts.append('ExecStart=/usr/bin/mount -t overlay overlay -o lowerdir=/run/virtio-dxg/bin:/usr/local/bin,upperdir=/run/virtio-dxg/bin-upper,workdir=/run/virtio-dxg/bin-work /usr/local/bin')
        stops.append('ExecStop=/usr/bin/umount /usr/local/bin')
    if has_omarchy_bin:
        (boot / 'omarchy-upper').mkdir(parents=True, exist_ok=True)
        (boot / 'omarchy-work').mkdir(parents=True, exist_ok=True)
        starts.append('ExecStart=/usr/bin/mount -t overlay overlay -o lowerdir=/run/virtio-dxg/omarchy-bin:/usr/share/omarchy/bin,upperdir=/run/virtio-dxg/omarchy-upper,workdir=/run/virtio-dxg/omarchy-work /usr/share/omarchy/bin')
        stops.insert(0, 'ExecStop=/usr/bin/umount /usr/share/omarchy/bin')
    if has_apps:
        (boot / 'app-upper').mkdir(parents=True, exist_ok=True)
        (boot / 'app-work').mkdir(parents=True, exist_ok=True)
        starts.append('ExecStart=/usr/bin/mount -t overlay overlay -o lowerdir=/run/virtio-dxg/applications:/usr/share/applications,upperdir=/run/virtio-dxg/app-upper,workdir=/run/virtio-dxg/app-work /usr/share/applications')
        stops.insert(0, 'ExecStop=/usr/bin/umount /usr/share/applications')

    unit = root / 'run/systemd/system/virtio-dxg-bin.service'
    unit.parent.mkdir(parents=True, exist_ok=True)
    unit.write_text(
        '[Unit]\n'
        'Description=virtio-dxg launch paths for this boot\n'
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
    return True


def prepare_browser(root, source):
    """Backward-compatible helper for staging the Chromium wrapper overlay."""
    return prepare_launch_paths(root, browser_wrapper=source)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--recipe', required=True, type=Path)
    parser.add_argument('--gtk-only', action='store_true')
    parser.add_argument('--browser-wrapper', type=Path)
    parser.add_argument('--xdg-settings-fast', type=Path)
    parser.add_argument('--app-fastlaunch', type=Path)
    args = parser.parse_args()
    if args.gtk_only:
        version = extract_gtk(args.archive, args.destination, args.recipe)
        print('virtio-dxg: staged matched GTK ' + version)
    else:
        version = extract(args.archive, args.destination, args.recipe)
        if args.browser_wrapper or args.xdg_settings_fast or args.app_fastlaunch:
            try:
                prepare_launch_paths(
                    Path('/'), args.browser_wrapper, args.xdg_settings_fast, args.app_fastlaunch
                )
            except (ValueError, OSError) as error:
                print('virtio-dxg: launch path overlay unavailable: ' + str(error))
        print('virtio-dxg: staged matched Mesa ' + version + '; hardware acceptance pending')
