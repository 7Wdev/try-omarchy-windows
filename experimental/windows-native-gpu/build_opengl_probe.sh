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
exit_define=
if [ "${WDDM_BRIDGE_TEST_EXIT_LIFETIME:-0}" = 1 ]; then
    exit_define=-DWDDM_BRIDGE_EXIT_LIFETIME_TEST=1
    dxg=${WDDM_BRIDGE_DXG_HEADERS:?Supply libdxg source for the exit diagnostic}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC \
        -isystem "$dxg/include" -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        "$base/exit_lifetime_probe.cpp" -o "$output/exit-lifetime-probe.so"
fi
if [ "${WDDM_BRIDGE_TEST_MESA_FENCE_FDS:-0}" = 1 ]; then
    if [ "${WDDM_BRIDGE_TEST_SYNC_FDS:-0}" = 1 ]; then
        echo 'Select one explicit fence diagnostic at a time' >&2
        exit 2
    fi
    source=${WDDM_BRIDGE_MESA_SOURCE:?Supply the pinned, patched Mesa source}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
        -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        -isystem "$source/include" -isystem "$source/src/gallium/drivers/d3d12" \
        -c "$base/mesa_fence_fd_probe.cpp" -o "$output/mesa-fence-fd-probe.o"
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define -DWDDM_BRIDGE_MESA_FENCE_FD_TEST=1 \
        -isystem "$mesa/include" -c "$base/egl_gl_probe.c" -o "$output/egl-gl-probe.o"
    "${CXX:-c++}" -std=c++17 -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        "$output/egl-gl-probe.o" "$output/mesa-fence-fd-probe.o" "$directx/src/dxguids.cpp" \
        -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 -ldl -o "$output/egl-gl-probe"
elif [ "${WDDM_BRIDGE_TEST_SYNC_FDS:-0}" = 1 ]; then
    dxg=${WDDM_BRIDGE_DXG_HEADERS:?Supply libdxg source for shared-fence acceptance}
    directx=${WDDM_BRIDGE_DIRECTX_HEADERS:?Supply DirectX-Headers source}
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
        -isystem "$dxg/include" -isystem "$directx/include" -isystem "$directx/include/wsl/stubs" \
        -c "$base/sync_fd_probe.cpp" -o "$output/sync-fd-probe.o"
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define -DWDDM_BRIDGE_SYNC_FD_TEST=1 -isystem "$mesa/include" \
        -c "$base/egl_gl_probe.c" -o "$output/egl-gl-probe.o"
    "${CXX:-c++}" "$output/egl-gl-probe.o" "$output/sync-fd-probe.o" \
        -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 -ldl -o "$output/egl-gl-probe"
else
    "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror $exit_define -isystem "$mesa/include" \
        "$base/egl_gl_probe.c" -L"$mesa/lib" -Wl,-rpath-link,"$mesa/lib" -l:libEGL.so.1 \
        -ldl -o "$output/egl-gl-probe"
fi
"${CC:-cc}" -static -std=c11 -O2 -Wall -Wextra -Werror \
    "$base/runtime_guest_init.c" -o "$output/runtime-init"
