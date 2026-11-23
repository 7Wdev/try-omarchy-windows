"""applaunch.py [RUNS] [--json PATH]: time from starting an app to its window
being mapped (Hyprland's openwindow event), cold launch after boot and warm
median/minimum launches across native, GTK4, browser, Electron and Vulkan apps.
Run as the desktop user, in the desktop session."""
import argparse
import json
import os
import shlex
import shutil
import socket
import statistics
import subprocess
import time

APPS = [  # label, command, window class (substring, lower case)
    ('ghostty', 'ghostty', 'ghostty'),
    ('alacritty', 'alacritty', 'alacritty'),
    ('foot', 'foot', 'foot'),
    ('nautilus', 'nautilus --new-window', 'nautilus'),
    ('gnome-calculator', 'gnome-calculator', 'calculator'),
    ('omacalc', 'omacalc', 'omacalc'),
    ('chromium', 'chromium --new-window about:blank', 'chromium'),
    ('code', 'code --new-window', 'code'),
    ('vkcube', 'vkcube', 'vkcube'),
    ('blender', 'blender --factory-startup', 'blender'),
]


def have(cmd):
    return shutil.which(shlex.split(cmd)[0]) is not None


def launch(sock_path, cmd, cls, timeout=20.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(0.05)
    t0 = time.monotonic()
    p = subprocess.Popen(shlex.split(cmd), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    buf, addr = b'', None
    while time.monotonic() - t0 < timeout and addr is None:
        try:
            buf += s.recv(65536)
        except socket.timeout:
            continue
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            if line.startswith(b'openwindow>>'):
                fields = line[12:].decode(errors='replace').split(',', 3)
                if len(fields) >= 3 and cls in fields[2].lower():
                    addr = fields[0]
                    break
    ms = (time.monotonic() - t0) * 1000 if addr else None
    s.close()
    if addr:
        time.sleep(0.15)
        addr = addr.removeprefix('0x')
        subprocess.run(['hyprctl', 'dispatch', 'closewindow', 'address:0x' + addr],
                       capture_output=True)
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, 9)
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
    args = parser.parse_args()

    sock_path = os.path.join(os.environ['XDG_RUNTIME_DIR'], 'hypr',
                             os.environ['HYPRLAND_INSTANCE_SIGNATURE'], '.socket2.sock')
    results = {}
    fmt = lambda t: f'{t:7.0f}' if t is not None else '   fail'
    for label, cmd, cls in APPS:
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
        print(f'{label:18s} first {fmt(times[0])} ms, warm {' '.join(fmt(t) for t in times[1:])} ms '
              f'(med {med.strip()} / min {lo.strip()} ms)')

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump({'runs': args.runs, 'apps': results}, f, indent=2)
            f.write('\n')


if __name__ == '__main__':
    main()
