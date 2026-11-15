#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Turns the published AppImage into an Ubuntu/Debian package, and writes the
# one-line installer that the website serves next to it:
#
#   packaging/linux/make-deb.sh dist/redvaultmessages-<version>-x86_64.AppImage [out-dir]
#
# Out (default dist/):
#   redvaultmessages_<version>_amd64.deb  installs from App Center with a double click, or
#                                          `sudo apt install ./redvaultmessages_<version>_amd64.deb`
#   install-linux.sh                       `curl -fsSL <site>/install-linux.sh | bash`: downloads
#                                          the .deb, checks its SHA-256 and installs it with apt
#
# The package holds exactly what the AppImage holds (unpacked into
# /opt/redvaultmessages, so no FUSE is needed), plus a launcher in
# /usr/bin, the app-menu entry and the official icons. apt installs the
# system libraries the app uses from the Depends line (the ones the AppImage
# leaves to the system: OpenGL, X11, fontconfig, glibc 2.39+). Building it
# needs dpkg-deb (any Debian/Ubuntu); no root.
set -euo pipefail

appimage=$(readlink -f "${1:?usage: make-deb.sh <AppImage> [out-dir]}")
out=$(readlink -f "${2:-$(dirname "$appimage")}")
site="https://www.redvaultsystems.com/redvaultmessages/desktop"
version=$(basename "$appimage" | sed -n 's/^redvaultmessages-\([0-9.]*\)-x86_64\.AppImage$/\1/p')
[ -n "$version" ] || { echo "Can't read the version from $(basename "$appimage")" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$appimage" "$work/app.AppImage"
chmod +x "$work/app.AppImage"
(cd "$work" && ./app.AppImage --appimage-extract >/dev/null)

pkg="$work/pkg"
mkdir -p "$pkg/DEBIAN" "$pkg/opt" "$pkg/usr/bin" "$pkg/usr/share/applications" "$pkg/usr/share/icons"
mv "$work/squashfs-root" "$pkg/opt/redvaultmessages"
# The AppImage's own top-level links (AppRun, .DirIcon, .desktop) aren't used by the package.
rm -f "$pkg/opt/redvaultmessages/AppRun" "$pkg/opt/redvaultmessages/.DirIcon" \
      "$pkg/opt/redvaultmessages/redvaultmessages.desktop" "$pkg/opt/redvaultmessages/redvaultmessages.svg"
# The binary finds its libraries, Qt plugins and QML next to its real path ($ORIGIN, qt.conf).
ln -s /opt/redvaultmessages/usr/bin/redvaultmessages "$pkg/usr/bin/redvaultmessages"
cp -r "$pkg/opt/redvaultmessages/usr/share/icons/hicolor" "$pkg/usr/share/icons/"
sed -e '/^X-AppImage-/d' "$pkg/opt/redvaultmessages/usr/share/applications/redvaultmessages.desktop" \
    > "$pkg/usr/share/applications/redvaultmessages.desktop"
grep -q '^StartupWMClass=' "$pkg/usr/share/applications/redvaultmessages.desktop" ||
    echo 'StartupWMClass=redvaultmessages' >> "$pkg/usr/share/applications/redvaultmessages.desktop"
mkdir -p "$pkg/usr/share/doc/redvaultmessages"
cat > "$pkg/usr/share/doc/redvaultmessages/copyright" <<'EOF'
RedVaultMessages
Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems
License: AGPL-3.0-only
The licences of the bundled components are in /opt/redvaultmessages/usr/share/licenses,
/opt/redvaultmessages/usr/licenses and /opt/redvaultmessages/usr/share/doc.
EOF

size=$(du -sk --exclude=DEBIAN "$pkg" | cut -f1)
cat > "$pkg/DEBIAN/control" <<EOF
Package: redvaultmessages
Version: $version
Architecture: amd64
Maintainer: RedVault Systems <cybermarkagency@gmail.com>
Installed-Size: $size
Depends: libc6 (>= 2.39), libstdc++6, libgcc-s1, zlib1g, libgl1, libegl1, libopengl0, libglx0, libglvnd0, libdrm2, libx11-6, libx11-xcb1, libxcb1, libfontconfig1, libfreetype6, libexpat1, libgmp10, libgpg-error0, libcom-err2
Recommends: libpulse0
Section: net
Priority: optional
Homepage: https://www.redvaultsystems.com/
Description: RedVaultMessages, end-to-end encrypted messenger
 Private messages, calls, files and a password vault, encrypted on your
 own devices and reached through Tor.
EOF
# Refresh the menu and the icon cache straight away (dpkg triggers do it too, later).
cat > "$pkg/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
EOF
chmod 755 "$pkg/DEBIAN/postinst"
chmod -R u+rwX,go+rX,go-w "$pkg"

deb="$out/redvaultmessages_${version}_amd64.deb"
dpkg-deb --root-owner-group -Zxz --build "$pkg" "$deb" >/dev/null
sha=$(sha256sum "$deb" | cut -d' ' -f1)

# The one-line installer, pinned to this exact package by its SHA-256.
cat > "$out/install-linux.sh" <<EOF
#!/bin/sh
# RedVaultMessages $version for Ubuntu 24.04+ / Debian 13+ (64-bit PC).
#   curl -fsSL $site/install-linux.sh | sh
# Downloads the package, checks it is exactly the published one (SHA-256),
# then installs it with apt, which also adds the system libraries it needs.
set -eu
deb="redvaultmessages_${version}_amd64.deb"
sha="$sha"
url="$site/\$deb"
if [ "\$(uname -m)" != "x86_64" ]; then echo "RedVaultMessages for Linux needs a 64-bit PC (x86_64)." >&2; exit 1; fi
if ! command -v apt-get >/dev/null 2>&1; then
    echo "This installer is for Ubuntu and Debian. On other systems use the AppImage: $site/redvaultmessages-x86_64.AppImage" >&2; exit 1
fi
tmp=\$(mktemp -d)
trap 'rm -rf "\$tmp"' EXIT
chmod 755 "\$tmp"   # apt reads the package as its own user
echo "Downloading RedVaultMessages $version..."
if command -v curl >/dev/null 2>&1; then curl -fL --progress-bar -o "\$tmp/\$deb" "\$url"; else wget -q --show-progress -O "\$tmp/\$deb" "\$url"; fi
echo "\$sha  \$tmp/\$deb" | sha256sum -c --quiet - || { echo "The download doesn't match the published package. Nothing was installed." >&2; exit 1; }
chmod 644 "\$tmp/\$deb"
echo "Installing (your password may be asked for)..."
sudo apt-get install -y "\$tmp/\$deb"
echo "Done. Open Show Apps and choose RedVaultMessages."
EOF
chmod 755 "$out/install-linux.sh"

echo "$deb"
echo "SHA-256: $sha"
echo "$out/install-linux.sh"
