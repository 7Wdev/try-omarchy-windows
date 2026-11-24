"""applaunch.py [RUNS] [--json PATH]: time from starting an app to its window
being mapped (Hyprland's openwindow event), cold launch after boot and warm
median/minimum launches across native, GTK4, browser, PWA, Electron, Vulkan and Wine apps.
Run as the desktop user, in the desktop session."""
import argparse
import json
import os
import shlex
import shutil
import signal
import socket
import statistics
import subprocess
import time

WINE_CMD = os.environ.get('VDXG_WINE_APP', os.path.expanduser('~/vdxg-test/run-vkpresent.sh'))

APPS = [  # label, command, window class (substring, lower case)
    ('ghostty', 'ghostty', 'ghostty'),
    ('alacritty', 'alacritty', 'alacritty'),
    ('foot', 'foot', 'foot'),
    ('nautilus', 'nautilus --new-window', 'nautilus'),
    ('gnome-calculator', 'gnome-calculator', 'calculator'),
    ('omacalc', 'omacalc', 'omacalc'),
    ('chromium', 'chromium --new-window about:blank', 'chromium'),
    ('chromium-pwa', 'chromium --app=about:blank', 'chrom'),
    ('code', 'code --new-window', 'code'),
    ('vkcube', 'vkcube', 'vkcube'),
    ('blender', 'blender --factory-startup', 'blender'),
    ('wine-vulkan', WINE_CMD, 'vkpresent'),
]

WEBAPP_APPS = [
    ('youtube-pwa', 'omarchy-launch-webapp https://youtube.com/', 'youtube'),
    ('discord-pwa', 'omarchy-launch-webapp https://discord.com/channels/@me', 'discord'),
]


def have(cmd):
    exe = shlex.split(cmd)[0]
    if os.path.isabs(exe):
        return os.path.isfile(exe) and os.access(exe, os.X_OK)
    return shutil.which(exe) is not None


def hypr_close(addr):
    expr = f'hl.dispatch(hl.dsp.window.close({{ window = "address:0x{addr}" }}))'
    res = subprocess.run(['hyprctl', 'eval', expr], capture_output=True, text=True)
    if res.returncode != 0 or not res.stdout.strip().startswith('ok'):
        subprocess.run(['hyprctl', 'dispatch', 'closewindow', 'address:0x' + addr],
                       capture_output=True)


def hypr_clients_via_sock(ctl_sock_path):
    cs = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    cs.settimeout(1.0)
    try:
        cs.connect(ctl_sock_path)
        cs.sendall(b'j/clients')
        chunks = []
        while True:
            d = cs.recv(65536)
            if not d:
                break
            chunks.append(d)
        return json.loads(b''.join(chunks).decode(errors='replace'))
    except Exception:
        return None
    finally:
        cs.close()


def launch(sock_path, cmd, cls, timeout=20.0):
    ctl_sock_path = os.path.join(os.path.dirname(sock_path), '.socket.sock')
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(0.05)
    t0 = time.monotonic()
    p = subprocess.Popen(shlex.split(cmd), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    buf, addr, t_hit = b'', None, None
    while time.monotonic() - t0 < timeout and addr is None:
        try:
            buf += s.recv(65536)
        except socket.timeout:
            continue
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            if line.startswith(b'openwindow>>'):
                fields = line[12:].decode(errors='replace').split(',', 3)
                if len(fields) >= 3 and not fields[1].lower().startswith('special') and (
                    cls in fields[2].lower() or
                    (len(fields) >= 4 and not fields[2] and cls in fields[3].lower())
                ):
                    addr = fields[0]
                    t_hit = time.monotonic()
                    break
            elif line.startswith(b'movewindow>>'):
                fields = line[12:].decode(errors='replace').split(',', 1)
                if len(fields) == 2 and not fields[1].lower().startswith('special'):
                    cand = fields[0].removeprefix('0x').lower()
                    hit_time = time.monotonic()
                    clients = hypr_clients_via_sock(ctl_sock_path) or []
                    for c in clients:
                        if c.get('address', '').removeprefix('0x').lower() == cand:
                            wcls = c.get('class', '').lower()
                            wtitle = c.get('title', '').lower()
                            if cls in wcls or (not wcls and cls in wtitle):
                                addr = cand
                                t_hit = hit_time
                                break
                    if addr is not None:
                        break
    ms = ((t_hit or time.monotonic()) - t0) * 1000 if addr else None
    s.close()
    if addr:
        time.sleep(0.15)
        addr = addr.removeprefix('0x')
        hypr_close(addr)
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(p.pid, signal.SIGTERM)
            p.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    time.sleep(0.5)
    return ms


def summarize(times):
    first = round(times[0], 1) if times[0] is not None else None
    warm = [round(t, 1) for t in times[1:] if t is not None]
    return {
        'first_ms': first,
        'warm_ms': [round(t, 1) if t is not None else None for t in times[1:]],
        'warm_median_ms': round(statistics.median(warm), 1) if warm else None,
        'warm_min_ms': round(min(warm), 1) if warm else None,
    }


def main():
    parser = argparse.ArgumentParser(description='Measure Hyprland window-map latency.')
    parser.add_argument('runs', nargs='?', type=int, default=3,
                        help='Warm runs after the initial cold launch (default: 3)')
    parser.add_argument('--json', type=str, default=None,
                        help='Optional path to write structured JSON latency results')
    parser.add_argument('--include-webapps', action='store_true',
                        help='Also measure YouTube and Discord PWA window launches')
    args = parser.parse_args()

    sock_path = os.path.join(os.environ['XDG_RUNTIME_DIR'], 'hypr',
                             os.environ['HYPRLAND_INSTANCE_SIGNATURE'], '.socket2.sock')
    results = {}
    fmt = lambda t: f'{t:7.0f}' if t is not None else '   fail'
    apps = APPS + (WEBAPP_APPS if args.include_webapps else [])
    for label, cmd, cls in apps:
        if not have(cmd):
            print(f'{label:18s} not installed')
            results[label] = {'status': 'skipped'}
            continue
        times = [launch(sock_path, cmd, cls) for _ in range(args.runs + 1)]
        summary = summarize(times)
        summary['status'] = 'ok' if summary['first_ms'] is not None else 'partial'
        results[label] = summary
        med = fmt(summary['warm_median_ms'])
        lo = fmt(summary['warm_min_ms'])
        print(f'{label:18s} first {fmt(times[0])} ms, warm {" ".join(fmt(t) for t in times[1:])} ms '
              f'(med {med.strip()} / min {lo.strip()} ms)')

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump({'runs': args.runs, 'apps': results}, f, indent=2)
            f.write('\n')


if __name__ == '__main__':
    main()
