#!/bin/bash
# Build the ABI-identical virtio-dxg GTK 4 runtime library without touching
# installed /usr/lib libraries.
# Run in the guest: build-in-guest.sh <gtk-VERSION.tar.xz> <out.tar>
set -euo pipefail
[ "$#" -eq 2 ] || { echo "usage: $0 <gtk-VERSION.tar.xz> <out.tar>" >&2; exit 2; }
here=$(cd -- "$(dirname -- "$0")" && pwd)
tarball=$(realpath -- "$1")
out=$(realpath -m -- "$2")
[ "$tarball" != "$out" ] || { echo 'output must differ from source archive' >&2; exit 2; }
jobs=${JOBS:-4}
version=$(cat "$here/CURRENT")
recipe=$here/versions/$version
lib=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["libraryFilename"])' "$recipe/recipe.json")
factory="/usr/lib/$lib"
[ -f "$factory" ] || { echo "missing guest reference library: $factory" >&2; exit 1; }

work=${WORK:-$HOME/.cache/virtio-dxg-gtk}
mkdir -p -- "$work" "$(dirname -- "$out")"
build=$(mktemp -d "$work/build-$version.XXXXXXXX")
pending=$(mktemp "${out}.pending.XXXXXXXX")
trap 'rm -f -- "$pending"' EXIT

python3 "$here/stage_source.py" "$tarball" "$build/source" --recipe "$recipe"
src=$build/source/gtk-$version

export CFLAGS='-march=x86-64 -mtune=generic -O2 -pipe -fno-omit-frame-pointer -DG_DISABLE_CAST_CHECKS'
export CXXFLAGS='-march=x86-64 -mtune=generic -O2 -pipe -fno-omit-frame-pointer'
if command -v arch-meson >/dev/null 2>&1; then
    arch-meson "$src" "$build/compile" \
        --wrap-mode=nodownload -Ddocumentation=false -Dman-pages=false \
        -Dbroadway-backend=true -Dcloudproviders=enabled -Dcolord=enabled \
        -Dsysprof=enabled -Dtracker=enabled
else
    meson setup "$build/compile" "$src" \
        --prefix=/usr --libdir=lib --buildtype=plain \
        --wrap-mode=nodownload -Ddocumentation=false -Dman-pages=false \
        -Dbroadway-backend=true -Dcloudproviders=enabled -Dcolord=enabled \
        -Dsysprof=enabled -Dtracker=enabled
fi

ninja -j "$jobs" -C "$build/compile" "gtk/$lib"
mkdir -p "$build/dest/usr/lib"
strip --strip-unneeded -o "$build/dest/usr/lib/$lib" "$build/compile/gtk/$lib"
ln -sf "$lib" "$build/dest/usr/lib/libgtk-4.so.1"
ln -sf "libgtk-4.so.1" "$build/dest/usr/lib/libgtk-4.so"

abi() {
    nm -D --defined-only --format=posix "$1" | awk '{ print $1, $2 }' | LC_ALL=C sort >"$2.symbols"
    readelf -dW "$1" | awk '/\((NEEDED|SONAME)\)/ { print $2, $NF }' | LC_ALL=C sort >"$2.dynamic"
}
abi "$factory" "$build/factory"
abi "$build/dest/usr/lib/$lib" "$build/built"
diff -u "$build/factory.symbols" "$build/built.symbols"
diff -u "$build/factory.dynamic" "$build/built.dynamic"

python3 "$here/record_build.py" "$src" "$build/dest/usr" "$recipe"
tar --hard-dereference --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C "$build/dest/usr" -cf "$pending" lib
mv -f -- "$pending" "$out"
printf 'Packed ABI-matched GTK %s into %s; build retained in %s\n' "$version" "$out" "$build"
