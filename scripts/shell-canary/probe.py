#!/usr/bin/env python3
"""Load the unmodified guest QML in a private headless Wayland session."""
import json
import os
from pathlib import Path
import subprocess
import time

RESULTS = Path('/results')
SHELL = Path(os.environ['OMARCHY_PATH']) / 'shell'


def main():
    started = time.monotonic()
    processes = []
    evidence = {'stages': [], 'success': False}
    env = dict(os.environ, WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1',
               WLR_LIBINPUT_NO_DEVICES='1', WLR_RENDERER='pixman',
               SWAYSOCK=os.environ['XDG_RUNTIME_DIR'] + '/sway.sock')
    config = Path('/tmp/canary-sway.conf')
    config.write_text('output HEADLESS-1 mode 1920x1080\nseat seat0 fallback true\n')
    with (RESULTS / 'compositor.log').open('w') as compositor_log, (RESULTS / 'shell.log').open('w') as shell_log:
        try:
            compositor = subprocess.Popen(['sway', '--unsupported-gpu', '-c', str(config)],
                                          env=env, stdout=compositor_log, stderr=subprocess.STDOUT)
            processes.append(compositor)
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if compositor.poll() is not None:
                    raise RuntimeError(f'Compositor exited: {compositor.returncode}')
                displays = list(Path(env['XDG_RUNTIME_DIR']).glob('wayland-*'))
                sockets = [p for p in displays if not p.name.endswith('.lock')]
                if sockets:
                    env['WAYLAND_DISPLAY'] = sockets[0].name
                    break
                time.sleep(.2)
            else:
                raise RuntimeError('No Wayland display within 20 seconds')
            shell = subprocess.Popen(['qs', '-n', '-p', str(SHELL)], env=env,
                                     stdout=shell_log, stderr=subprocess.STDOUT)
            processes.append(shell)

            def ipc(*args):
                if shell.poll() is not None:
                    raise RuntimeError(f'Shell exited: {shell.returncode}')
                result = subprocess.run(['qs', 'ipc', '-n', '-p', str(SHELL), 'call', '--', *args],
                                        env=env, capture_output=True, text=True, timeout=3)
                result.check_returncode()
                return result.stdout.strip()

            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if ipc('shell', 'ping') == 'ok':
                    break
                time.sleep(.2)
            else:
                raise RuntimeError('Shell IPC did not become ready within 20 seconds')
            time.sleep(5)
            geometry = json.loads(ipc('shell', 'debugBarGeometry'))
            evidence['bar'] = geometry
            if not any(item['visible'] and item['itemVisible'] for item in geometry):
                raise RuntimeError('No visible bar widgets were instantiated')
            evidence['stages'].append('bar')
            if ipc('shell', 'summon', 'omarchy.menu', '{"menu":"root"}') != 'ok':
                raise RuntimeError('Omarchy menu component did not open')
            time.sleep(5)
            if ipc('omarchy.menu', 'ping') != 'ok':
                raise RuntimeError('Omarchy menu is not responding')
            evidence['stages'].append('Omarchy menu')
            ipc('shell', 'hide', 'omarchy.menu')
            # Win+K uses this same menu component in select mode. Sway cannot
            # supply Hyprland's live binds, so exercise the UI with sample rows.
            payload = {'mode': 'select', 'prompt': 'Keybindings',
                       'options': ['SUPER + K  Keybindings', 'SUPER + SPACE  Omarchy menu'],
                       'width': 800, 'maxHeight': 500,
                       'selectionFile': '/tmp/canary-selection', 'doneFile': '/tmp/canary-done'}
            if ipc('shell', 'summon', 'omarchy.menu', json.dumps(payload)) != 'ok':
                raise RuntimeError('Keybindings menu component did not open')
            time.sleep(5)
            if ipc('omarchy.menu', 'ping') != 'ok':
                raise RuntimeError('Keybindings menu is not responding')
            evidence['stages'].append('Keybindings select menu (sample rows)')
            time.sleep(10)
            if shell.poll() is not None or compositor.poll() is not None:
                raise RuntimeError('Shell or compositor exited during observation')
            evidence['success'] = True
        except Exception as error:
            evidence['error'] = str(error)
            print(f'Canary probe failed: {error}', flush=True)
        finally:
            for process in reversed(processes):
                running = process.poll() is None
                if running:
                    process.terminate()
                try:
                    code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    code = process.wait()
                # SIGTERM is our intentional shutdown, never an observed crash.
                if code != 0 and not (running and code == -15):
                    evidence['success'] = False
                    evidence['error'] = f'Process {process.args[0]} exited: {code}'
            evidence['seconds'] = round(time.monotonic() - started, 2)
            (RESULTS / 'probe.json').write_text(json.dumps(evidence, indent=2) + '\n')
    return 0 if evidence['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
