#!/usr/bin/env bash
# Compile comparable guest libraries on an isolated Linux runner.
set -euo pipefail
cd /tmp
printf 'Server = https://archive.archlinux.org/repos/2026/10/03/$repo/os/$arch\n' >/etc/pacman.d/mirrorlist
pacman-key --init
pacman-key --populate archlinux
pacman --disable-sandbox --noconfirm -Syyu --needed base-devel gtk4 meson git \
  glib2-devel gobject-introspection hicolor-icon-theme libsysprof-capture python-gobject \
  sassc shaderc vulkan-headers wayland-protocols curl
test "$(pacman -Q gtk4)" = 'gtk4 1:4.24.1-1'
lib=libgtk-4.so.1.2400.1
archive=gtk-4.24.1.tar.xz
sources=/repo/virtio-dxg/bench/desktop
curl --fail --location --proto '=https' --proto-redir '=https' --retry 2 --max-time 120 \
  "https://download.gnome.org/sources/gtk/4.24/$archive" -o "$archive"
echo "e98abe720e16129c8c0f50761dee0a2e9ae2478055e31018f6cf977fcf6513e9  $archive" | sha256sum -c -
tar xf "$archive"
cd gtk-4.24.1
sha256sum -c <<'SUMS'
afe45a2e916221509bfcd64db9daee3cae081a835be1a8952b8dd86f9456fb55  gtk/gtkgridview.c
d232ef28bd6fbf44208acf5311903e16598bd35a891cc44fbda3803ca35bce78  gtk/gtkcssnode.c
SUMS
export CFLAGS='-march=x86-64 -mtune=generic -O2 -pipe -fno-omit-frame-pointer -DG_DISABLE_CAST_CHECKS'
export CXXFLAGS='-march=x86-64 -mtune=generic -O2 -pipe -fno-omit-frame-pointer'
arch-meson . build --wrap-mode=nodownload -Ddocumentation=false -Dman-pages=false \
  -Dbroadway-backend=true -Dcloudproviders=enabled -Dcolord=enabled \
  -Dsysprof=enabled -Dtracker=enabled
abi() {
  nm -D --defined-only --format=posix "$1" | awk '{ print $1, $2 }' | LC_ALL=C sort >"$2.symbols"
  readelf -dW "$1" | awk '/\((NEEDED|SONAME)\)/ {print $2, $NF}' | LC_ALL=C sort >"$2.dynamic"
}
abi "/usr/lib/$lib" /tmp/factory
mkdir -p /out
for variant in control grid combined; do
  case "$variant" in
    grid) git apply "$sources/gtk-grid-split-spacing.patch" ;;
    combined) git apply "$sources/gtk-css-skip-valid-children.patch" ;;
  esac
  ninja -j2 -C build "gtk/$lib"
  mkdir "/out/$variant"
  strip --strip-unneeded -o "/out/$variant/$lib" "build/gtk/$lib"
  abi "/out/$variant/$lib" "/out/$variant/abi"
  diff -u /tmp/factory.symbols "/out/$variant/abi.symbols"
  diff -u /tmp/factory.dynamic "/out/$variant/abi.dynamic"
done
pacman -Q >/out/packages.txt
meson configure build >/out/configure.txt
cd /out
sha256sum {control,grid,combined}/"$lib" >SHA256SUMS
test "$(cut -d' ' -f1 SHA256SUMS | sort -u | wc -l)" -eq 3
