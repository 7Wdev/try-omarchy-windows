#!/bin/sh
# Compile the real EGL/OpenGL guest probe against an explicitly supplied Mesa
# prefix. Building does not install drivers or run against the host WSL GPU.
set -eu
if [ "$#" -ne 2 ]; then
    echo 'Usage: build_opengl_probe.sh non-GLVND-Mesa-prefix output-directory' >&2
    exit 2
fi
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mesa=$(CDPATH= cd -- "$1" && pwd)
mkdir -p "$2"
output=$(CDPATH= cd -- "$2" && pwd)
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -isystem "$mesa/include" \
    "$base/egl_gl_probe.c" -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 \
    -ldl -o "$output/egl-gl-probe"
"${CC:-cc}" -static -std=c11 -O2 -Wall -Wextra -Werror \
    "$base/runtime_guest_init.c" -o "$output/runtime-init"
