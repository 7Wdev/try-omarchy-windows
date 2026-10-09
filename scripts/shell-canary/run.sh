#!/usr/bin/env bash
set -euo pipefail
guest=/guest-source/guest
mkdir -p /results
cp "$guest/pacman-x86_64.conf" /etc/pacman.conf
pacman-key --init
pacman-key --populate archlinux
test "$(gpg --batch --with-colons --show-keys "$guest/keys/omarchy-pkgs.asc" | awk -F: '/^fpr:/ {print $10; exit}')" = 40DFB630FF42BCFFB047046CF0134EE680CAC571
pacman-key --add "$guest/keys/omarchy-pkgs.asc"
pacman-key --lsign-key 40DFB630FF42BCFFB047046CF0134EE680CAC571
pacman -Syu --noconfirm
pacman -S --needed --noconfirm quickshell qt6-base qt6-declarative qt6-wayland \
  qt6-svg qt6-multimedia sway dbus mesa git python jq fontconfig \
  ttf-jetbrains-mono-nerd pipewire wireplumber
pacman -Q > /results/packages.txt
cp /etc/pacman.conf /results/pacman.conf
cp "$guest/spec.json" /results/build-spec.json
"$guest/scripts/fetch-source.sh" --destination /tmp/omarchy-source
# materialize requires the package-owned Neovim skeleton; it is unrelated to
# the shell. Seed its directory without installing the editor/toolchain.
mkdir -p /tmp/root/etc/skel/.config/nvim
"$guest/scripts/materialize-omarchy.sh" --root /tmp/root --source /tmp/omarchy-source
python3 "$guest/scripts/apply-omarchy-backports.py" --root /tmp/root --spec "$guest/spec.json"
cp -a /tmp/root/usr/bin/. /usr/bin/
if [[ ${UNFIXED_PALETTE:-false} == true ]]; then
  # The real tree is produced above. This proof reverses only the palette fix,
  # verifying every restored preimage against the guest's backport registry.
  git -C /tmp/root/usr/share/omarchy apply --no-index --reverse "$guest/patches/omarchy/qt612-palette.patch"
  python3 - "$guest/spec.json" <<'PY'
import hashlib, json, pathlib, sys
spec = json.loads(pathlib.Path(sys.argv[1]).read_text())
backport = next(b for b in spec['authenticity']['backports'] if b['id'] == 'qt612-palette')
for target in backport['targets']:
    path = pathlib.Path('/tmp/root/usr/share/omarchy') / target['path']
    assert hashlib.sha256(path.read_bytes()).hexdigest() == target['beforeSha256'], path
print('Verified unfixed palette preimages')
PY
fi
find /tmp/root/usr/share/omarchy/shell -type f -print0 | sort -z | \
  xargs -0 sha256sum > /results/shell-sha256.txt
useradd --create-home canary
cp -a /tmp/root/etc/skel/. /home/canary/
mkdir -p /home/canary/.local/state/omarchy/current /tmp/canary-runtime
ln -s /tmp/root/usr/share/omarchy/themes/tokyo-night /home/canary/.local/state/omarchy/current/theme
chown -R canary:canary /home/canary /tmp/canary-runtime /results
chmod 700 /tmp/canary-runtime
runuser -u canary -- env HOME=/home/canary XDG_RUNTIME_DIR=/tmp/canary-runtime \
  OMARCHY_PATH=/tmp/root/usr/share/omarchy QT_QPA_PLATFORM=wayland \
  QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 NO_AT_BRIDGE=1 \
  dbus-run-session -- python3 /repo/scripts/shell-canary/probe.py
