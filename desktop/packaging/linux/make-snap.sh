#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Turns the published AppImage into the snap for the Snap Store (Ubuntu App Center):
#
#   packaging/linux/make-snap.sh dist/redvaultmessages-<version>-x86_64.AppImage [snapcraft pack options]
#
# Out: dist/redvaultmessages_<version>_amd64.snap
#
# Like make-deb.sh, the snap holds exactly what the AppImage holds, unpacked.
# Needs snapcraft (`sudo snap install snapcraft --classic`) and either LXD
# (snapcraft's default build container) or, on Ubuntu 24.04 itself,
# `--destructive-mode` passed through as an option.
# Then, with a Snapcraft account that owns the name:
#   snapcraft upload --release=edge dist/redvaultmessages_<version>_amd64.snap
set -euo pipefail

appimage=$(readlink -f "${1:?usage: make-snap.sh <AppImage> [snapcraft pack options]}")
shift
here=$(cd "$(dirname "$0")" && pwd)
project="$here/../snap"
dist=$(dirname "$appimage")
id=com.redvaultsystems.redvaultmessages
version=$(basename "$appimage" | sed -n 's/^redvaultmessages-\([0-9.]*\)-x86_64\.AppImage$/\1/p')
[ -n "$version" ] || { echo "Can't read the version from $(basename "$appimage")" >&2; exit 1; }
# The snap's version is the newest (first) <release> in the metainfo.
newest=$(sed -n 's/.*<release version="\([^"]*\)".*/\1/p' "$here/redvaultmessages.metainfo.xml" | head -1)
[ "$newest" = "$version" ] || {
    echo "redvaultmessages.metainfo.xml's newest <release> is '$newest', not $version; add the release notes first" >&2; exit 1
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$appimage" "$work/app.AppImage"
chmod +x "$work/app.AppImage"
(cd "$work" && ./app.AppImage --appimage-extract >/dev/null)

input="$project/input"
rm -rf "$input"
mkdir -p "$input"
root="$input/root"
mv "$work/squashfs-root" "$root"
# The AppImage's own top-level links (AppRun, .DirIcon, .desktop) aren't used by the snap.
rm -f "$root/AppRun" "$root/.DirIcon" "$root/redvaultmessages.desktop" "$root/redvaultmessages.svg"

# The menu entry: the snap's icon by its path inside the snap, AppImage keys dropped.
desktop="$root/usr/share/applications/redvaultmessages.desktop"
sed -i -e '/^X-AppImage-/d' \
       -e 's|^Icon=.*|Icon=${SNAP}/usr/share/icons/hicolor/256x256/apps/redvaultmessages.png|' "$desktop"
grep -q '^StartupWMClass=' "$desktop" || echo 'StartupWMClass=redvaultmessages' >> "$desktop"

# The gnome extension's launcher points Qt at Wayland, but the AppImage carries
# only Qt's X11 (xcb) platform plugin, so the app runs through XWayland there.
# REDVAULT_QPA overrides it (e.g. offscreen, for tests).
cat > "$root/usr/bin/redvaultmessages-snap" <<'EOF'
#!/bin/sh
export QT_QPA_PLATFORM="${REDVAULT_QPA:-xcb}"
exec "$SNAP/usr/bin/redvaultmessages" "$@"
EOF
chmod 755 "$root/usr/bin/redvaultmessages-snap"

mkdir -p "$root/usr/share/metainfo"
cp "$here/redvaultmessages.metainfo.xml" "$root/usr/share/metainfo/$id.metainfo.xml"
cp "$root/usr/share/icons/hicolor/256x256/apps/redvaultmessages.png" "$input/redvaultmessages.png"
# The AppImage unpacks with owner-only folders; snaps must be world-readable.
chmod -R u+rwX,go+rX,go-w "$input"

out="$dist/redvaultmessages_${version}_amd64.snap"
# Start from a clean build each time: the parts are just the unpacked AppImage.
(cd "$project" && snapcraft clean "$@" >/dev/null 2>&1 || true)
(cd "$project" && snapcraft pack --output "input/out.snap" "$@")   # snapcraft only writes inside its project
mv -f "$input/out.snap" "$out"
rm -rf "$input"
echo "$out"
sha256sum "$out"
