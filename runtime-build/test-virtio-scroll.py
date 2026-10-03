#!/usr/bin/env python3
"""Compile the actual patched virtio wheel dispatch with transport stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched QEMU source directory')
root = parser.parse_args().source
source = (root / 'hw/input/virtio-input-hid.c').read_text()
capability = source[source.index('static bool virtio_input_has_rel('):
                    source.index('static void virtio_input_handle_event(')]
cases = source[source.index('    case INPUT_EVENT_KIND_BTN:'):
               source.index('    case INPUT_EVENT_KIND_ABS:')]
harness = Path(__file__).with_name('virtio-scroll-harness.c').read_text()
harness = harness.replace('/* REL_CAPABILITY */', capability).replace('/* WHEEL_DISPATCH */', cases)
with tempfile.TemporaryDirectory(prefix='tryomarchy-virtio-scroll-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('ok - virtio hi-res/legacy axes, fractional boundaries, mouse after touchpad, releases and missing capabilities')
