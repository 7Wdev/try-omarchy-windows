#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo 'usage: build_qemu.sh OUTPUT_DIRECTORY' >&2
    exit 2
fi
recipe=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$recipe/../.." && pwd)
output=$(mkdir -p "$1" && cd "$1" && pwd)
if [[ -e "$output/source" || -e "$output/build" ]]; then
    echo 'Use a fresh output directory' >&2
    exit 1
fi
mapfile -t locked < <(python - "$root/runtime-build/sources.lock.json" <<'PY'
import json, sys
qemu = json.load(open(sys.argv[1]))['qemu']
print(qemu['repository'])
print(qemu['commit'])
PY
)
repository=${locked[0]%$'\r'}
commit=${locked[1]%$'\r'}
git init --quiet "$output/source"
git -C "$output/source" remote add origin "$repository"
git -C "$output/source" fetch --quiet --depth=1 origin "$commit"
git -C "$output/source" -c core.autocrlf=false checkout --quiet --detach "$commit"
python "$recipe/prepare_qemu.py" "$output/source"
mkdir "$output/build"
cd "$output/build"
../source/configure --target-list=x86_64-softmmu --enable-whpx --enable-sdl \
    --disable-gtk --disable-vnc --disable-opengl --disable-virglrenderer \
    --disable-tools --disable-guest-agent --disable-docs --disable-slirp --disable-libusb
ninja -j4 qemu-system-x86_64.exe
mkdir -p "$output/bin" "$output/share"
cp qemu-system-x86_64.exe "$output/bin/"
cp -r "$output/source/pc-bios" "$output/share/qemu"
bash "$root/runtime-build/collect-dlls.sh" "$output/bin" "$output/dll-sources.txt"
pacman -Q > "$output/build-packages.txt"
cp config-host.mak "$output/"
tar --exclude=.git -czf "$output/qemu-corresponding-source.tar.gz" -C "$output" source
echo 'Built engineering QEMU: WHPX, SDL and shared Windows section memory; no guest GPU backend'
