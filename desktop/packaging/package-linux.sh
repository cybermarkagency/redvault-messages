#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Builds dist/redvaultmessages-<version>-x86_64.AppImage: release build,
# tests, `cmake --install` into an AppDir, then linuxdeploy bundles Qt (its
# plugins and the app's QML imports) and every library the binary needs, and
# appimagetool packs it with the static type2 runtime, so the AppImage starts
# without libfuse2 (which Ubuntu 24.04, Kali and Fedora no longer install).
# Needs Qt 6.5+ (qmake on PATH, or QMAKE set) and the build deps in README.md.
set -euo pipefail
cd "$(dirname "$0")/.."

version=$(sed -n 's/^project(RedvaultDesktop VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
build=build/linux-release
appdir="$PWD/build/AppDir"
tools=build/linuxdeploy

# Settings > About shows this version's source link and contact from these (empty: "not published yet" and the
# Terms of Service contact). Always passed, so a value left in the build cache from an earlier run can't stick.
# Release builds carry device keys with Signal sessions (needs Rust/cargo); passed for the same reason.
# Tor by default (docs/tor-by-default.md): every published build embeds the Tor gateway and is Tor-only at the
# official onion service (cmake/tor_settings.cmake is its one home). REDVAULT_RELEASE_BUILD makes the configure
# step refuse anything else, and the cache is checked afterwards too.
onion=$(sed -n 's/^set(REDVAULT_OFFICIAL_ONION_API_BASE "\(http:\/\/[a-z2-7]*\.onion\/[^"]*\)")$/\1/p' cmake/tor_settings.cmake)
if [ -z "$onion" ]; then
    echo "No official onion service in cmake/tor_settings.cmake" >&2
    exit 1
fi
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DREDVAULT_BUILD_CLI=OFF -DREDVAULT_BUILD_TOOLS=OFF -DREDVAULT_E2E_V3=ON -DREDVAULT_E2E_SIGNAL=ON \
    -DREDVAULT_TOR=ON -DREDVAULT_TOR_ONLY=ON "-DREDVAULT_ONION_API_BASE=$onion" -DREDVAULT_RELEASE_BUILD=ON \
    "-DREDVAULT_SOURCE_CODE_URL=${REDVAULT_SOURCE_CODE_URL:-}" "-DREDVAULT_SOURCE_CONTACT=${REDVAULT_SOURCE_CONTACT:-}"
for setting in "REDVAULT_TOR:BOOL=ON" "REDVAULT_TOR_ONLY:BOOL=ON" "REDVAULT_ONION_API_BASE:STRING=$onion" "REDVAULT_RELEASE_BUILD:BOOL=ON"; do
    if ! grep -qxF "$setting" "$build/CMakeCache.txt"; then
        echo "This build isn't Tor-only at the official onion service ($setting missing). A published build never is anything else." >&2
        exit 1
    fi
done
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure

rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build"
# The app's licence (AGPL-3.0-only) and the third-party notices must ship in the AppImage (usr/licenses).
for legal in LICENSE.txt THIRD_PARTY_NOTICES.txt; do
    if [ ! -f "$appdir/usr/licenses/$legal" ]; then
        echo "$appdir/usr/licenses/$legal is missing" >&2
        exit 1
    fi
done

# These programs run during the build and shape what ships, so each is pinned by SHA-256. The hashes are the
# ones GitHub reports for the release assets (the `digest` field of
# https://api.github.com/repos/<repo>/releases/tags/<tag>), never ones worked out here.
#
# linuxdeploy-plugin-qt's last fixed tag (1-alpha-20250213-1) is older than those digests, so it has no published
# hash and this takes its "continuous" build, whose asset was uploaded 2026-08-22. That tag moves: when upstream
# rebuilds, the check below stops the build. Read the new digest from the API and review what changed, or better,
# move to a fixed tag once one has a digest.
fetch_tool() {  # repo tag name sha256
    local file="$tools/$3.AppImage"
    # A file already here is checked too: it could be left over from anything.
    if [ -f "$file" ] && ! echo "$4  $file" | sha256sum --check --status; then
        rm -f "$file"
    fi
    if [ ! -f "$file" ]; then
        curl -fsSL -o "$file.part" "https://github.com/$1/releases/download/$2/$3.AppImage"
        if ! echo "$4  $file.part" | sha256sum --check --status; then
            rm -f "$file.part"
            echo "$3.AppImage from $1 ($2) doesn't match its pinned SHA-256. Nothing was run." >&2
            exit 1
        fi
        mv "$file.part" "$file"
    fi
    chmod +x "$file"
}

mkdir -p "$tools"
fetch_tool linuxdeploy/linuxdeploy 1-alpha-20251107-1 linuxdeploy-x86_64 \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
fetch_tool linuxdeploy/linuxdeploy-plugin-qt continuous linuxdeploy-plugin-qt-x86_64 \
    cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617
fetch_tool AppImage/appimagetool 1.9.1 appimagetool-x86_64 \
    ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
export QML_SOURCES_PATHS="$PWD/app/qml"
# Containers and CI runners often have no FUSE to mount the tool AppImages.
export APPIMAGE_EXTRACT_AND_RUN=1
"$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" \
    --executable "$appdir/usr/bin/redvaultmessages" \
    --desktop-file "$appdir/usr/share/applications/redvaultmessages.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/redvaultmessages.png" \
    --plugin qt

output="dist/redvaultmessages-$version-x86_64.AppImage"
mkdir -p dist
ARCH=x86_64 VERSION="$version" "$tools/appimagetool-x86_64.AppImage" --no-appstream "$appdir" "$output"
echo "$output"
