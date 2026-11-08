#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Collects the source code of every third-party library the Linux AppImage
# bundles, so it can be published next to the AppImage (the GPL, LGPL, MPL
# and AGPL require the source of what we ship to be available).
#
# Run it right after packaging/package-linux.sh, on the same machine (the
# GitHub Actions runner), so the versions match the AppImage exactly:
#
#   bash desktop/packaging/collect-linux-sources.sh            # download
#   bash desktop/packaging/collect-linux-sources.sh --dry-run  # only list
#
# - Libraries from Ubuntu packages: `apt-get source --download-only` for the
#   exact source package version (needs deb-src, which this enables on CI).
# - Qt from Qt's own installer (install-qt-action): the matching
#   qt*-everywhere-src tarballs from download.qt.io, plus the ICU and FFmpeg
#   that Qt's binaries bring along.
# - Anything it can't place is listed in UNRESOLVED.txt for a person to check.
#
# Output: desktop/dist/linux-sources/ and SOURCES.tsv (library, origin,
# source package, version, file).
set -euo pipefail
cd "$(dirname "$0")/.."

dry=0; [ "${1:-}" = "--dry-run" ] && dry=1
appdir="${APPDIR:-$PWD/build/AppDir}"
out="${OUT:-$PWD/dist/linux-sources}"
[ -d "$appdir" ] || { echo "No AppDir at $appdir (run package-linux.sh first, or set APPDIR)" >&2; exit 1; }
mkdir -p "$out"
list="$out/SOURCES.tsv"; unresolved="$out/UNRESOLVED.txt"
printf 'library\torigin\tsource\tversion\tfile\n' > "$list"; : > "$unresolved"

if [ "$dry" = 0 ] && [ -n "${CI:-}" ] && ! grep -rqs '^Types:.*deb-src' /etc/apt/sources.list.d/; then
    sudo sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/*.sources
    sudo apt-get update -q
fi

declare -A debsrc=()   # "srcpkg=version" -> 1
qt_libs=(); ffmpeg_libs=(); icu_libs=()

# Libraries that come from Qt's own installer (install-qt-action sets QT_ROOT_DIR). linuxdeploy
# patches and strips what it bundles, so bundled files can't be compared byte for byte with the
# originals: the file name is what tells Qt's copies apart from Ubuntu's.
declare -A from_qt=()
if [ -n "${QT_ROOT_DIR:-}" ] && [ -d "$QT_ROOT_DIR" ]; then
    while IFS= read -r -d '' q; do from_qt["$(basename "$q")"]=1; done < <(find "$QT_ROOT_DIR" -name '*.so*' -print0)
fi

while IFS= read -r -d '' f; do
    b=$(basename "$f")
    case "$b" in
        redvault*|libredvault*|liblivekit_ffi.so) continue ;;   # our own code, and LiveKit (Apache-2.0, see THIRD_PARTY.md)
    esac
    if [[ "$b" =~ ^lib(av|sw)(codec|format|util|scale|resample|device|filter) ]]; then ffmpeg_libs+=("$f"); continue; fi
    if [[ "$b" == libicu* ]]; then icu_libs+=("$f"); continue; fi
    if [ -n "${from_qt[$b]:-}" ]; then qt_libs+=("$b"); continue; fi
    # The system library folders first: other packages (e.g. the runner's Firefox) carry private
    # copies with the same file names, and linuxdeploy bundles from the system folders.
    owner=""
    for dir in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib /lib; do
        owner=$(dpkg -S "$dir/$b" 2>/dev/null | grep -v '^diversion' | head -1 || true)
        [ -n "$owner" ] && break
    done
    [ -n "$owner" ] || owner=$(dpkg -S "$b" 2>/dev/null | grep -E "/$b\$" | head -1 || true)
    if [ -n "$owner" ]; then
        pkg=${owner%%:*}; pkg=${pkg%%,*}
        src=$(dpkg-query -W -f '${source:Package}' "$pkg")
        ver=$(dpkg-query -W -f '${source:Version}' "$pkg")
        debsrc["$src=$ver"]=1
        printf '%s\tubuntu:%s\t%s\t%s\t\n' "$b" "$pkg" "$src" "$ver" >> "$list"
    elif [ -z "${QT_ROOT_DIR:-}" ] && [[ "$b" == libQt6* || "$b" == libq* || "$b" == lib*plugin.so ]]; then qt_libs+=("$b")
    else echo "$b: not from an Ubuntu package and not recognised as Qt, ICU or FFmpeg" >> "$unresolved"
    fi
done < <(find "$appdir" -name '*.so*' -type f -print0)

fetch() {  # url file
    printf '  %s\n' "$1"
    [ "$dry" = 1 ] || curl -fsSL --retry 3 -o "$out/$2" "$1" || echo "download failed: $1" >> "$unresolved"
}

echo "== Ubuntu source packages: ${#debsrc[@]}"
mkdir -p "$out/ubuntu"
# When apt's index no longer lists that exact version (a newer one replaced it in -updates or
# -security), fetch the same version's files from Launchpad, which keeps every published version.
launchpad_source() {  # name version
    local name=$1 version=$2 dsc pool="https://launchpad.net/ubuntu/+archive/primary/+files"
    dsc="${name}_${version#*:}.dsc"
    curl -fsSL --retry 3 -o "$dsc" "$pool/$dsc" || return 1
    # The .dsc's Files section lists the tarballs and patches that make up the source package.
    awk '/^Files:/{f=1;next} /^[^ ]/{f=0} f && NF==3 {print $3}' "$dsc" | while read -r part; do
        curl -fsSL --retry 3 -o "$part" "$pool/$part" || exit 1
    done
}
for sv in "${!debsrc[@]}"; do
    echo "  $sv"
    [ "$dry" = 1 ] && continue
    (cd "$out/ubuntu" && { apt-get source --download-only -q "$sv" >/dev/null 2>&1 || launchpad_source "${sv%%=*}" "${sv#*=}"; }) \
        || echo "source package not found (apt or Launchpad): $sv" >> "$unresolved"
done

if [ "${#qt_libs[@]}" -gt 0 ]; then
    qmake="${QMAKE:-$(command -v qmake6 || command -v qmake || true)}"
    qtver=$([ -n "$qmake" ] && "$qmake" -query QT_VERSION || true)
    if [ -z "$qtver" ]; then
        echo "Qt libraries found but no qmake to tell the Qt version" >> "$unresolved"
    else
        mm=${qtver%.*}
        echo "== Qt $qtver (${#qt_libs[@]} libraries and plugins)"
        # The modules whose libraries the AppImage bundles. qtbase carries the
        # image, platform and network plugins.
        for m in qtbase qtdeclarative qtmultimedia qtsvg; do
            file="$m-everywhere-src-$qtver.tar.xz"
            # Current releases live under official_releases; older ones move to archive/.
            current="https://download.qt.io/official_releases/qt/$mm/$qtver/submodules/$file"
            if [ "$dry" = 0 ] && curl -fsSL --retry 3 -o "$out/$file" "$current" 2>/dev/null; then
                printf '  %s\n' "$current"
            else
                fetch "https://download.qt.io/archive/qt/$mm/$qtver/submodules/$file" "$file"
            fi
            printf 'Qt %s\tqt.io\t%s\t%s\t%s\n' "$m" "$m" "$qtver" "$m-everywhere-src-$qtver.tar.xz" >> "$list"
        done
    fi
fi

if [ "${#icu_libs[@]}" -gt 0 ]; then
    # The full version string (e.g. 73.2) is only in libicuuc.
    icuuc=$(printf '%s\n' "${icu_libs[@]}" | grep -m1 libicuuc || echo "${icu_libs[0]}")
    icu=$(strings "$icuuc" | grep -m1 -E '^[0-9]{2}\.[0-9]+(\.[0-9]+)?$' || true)
    if [ -n "$icu" ]; then
        echo "== ICU $icu (bundled by Qt)"
        u=${icu//./_}; d=${icu//./-}
        fetch "https://github.com/unicode-org/icu/releases/download/release-$d/icu4c-$u-src.tgz" "icu4c-$u-src.tgz"
        printf 'ICU\tqt.io bundle\ticu4c\t%s\ticu4c-%s-src.tgz\n' "$icu" "$u" >> "$list"
    else
        echo "ICU: couldn't read the version from ${icu_libs[0]##*/}" >> "$unresolved"
    fi
fi

if [ "${#ffmpeg_libs[@]}" -gt 0 ]; then
    lavu=$(printf '%s\n' "${ffmpeg_libs[@]}" | grep -m1 libavutil || true)
    ff=$([ -n "$lavu" ] && strings "$lavu" | grep -m1 -oE 'FFmpeg version n?[0-9]+\.[0-9]+(\.[0-9]+)?' | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' || true)
    if [ -n "$ff" ]; then
        echo "== FFmpeg $ff (bundled by Qt)"
        fetch "https://ffmpeg.org/releases/ffmpeg-$ff.tar.xz" "ffmpeg-$ff.tar.xz"
        printf 'FFmpeg\tqt.io bundle\tffmpeg\t%s\tffmpeg-%s.tar.xz\n' "$ff" "$ff" >> "$list"
    else
        echo "FFmpeg libraries bundled by Qt (${ffmpeg_libs[*]##*/}): version not readable; take it from the Qt $qtver release notes and add ffmpeg-<version>.tar.xz by hand" >> "$unresolved"
    fi
fi

echo "Listed in $list"
if [ -s "$unresolved" ]; then echo "Needs a person to check ($unresolved):"; cat "$unresolved"; fi
