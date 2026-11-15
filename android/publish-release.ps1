# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems
#
# Publishes an Android release on the website and tells installed apps about it (in-app updates,
# app/src/main/java/.../update/AppUpdates.kt). Run it yourself, after `gradlew assembleRelease`:
#
#   powershell -ExecutionPolicy Bypass -File android\publish-release.ps1 -Notes "One line users will see"
#
# 1. Names the signed APK RedVault-Messages-<version>.apk (version and versionCode read from the APK itself).
# 2. Signs android-latest.json with the release key (desktop's redvault_release; asks for its passphrase).
#    It's the same key and format as the desktop's latest.json; installed apps accept only a manifest it signed.
# 3. Uploads the APK first and the manifest last, so an app that sees the new version always finds the APK;
#    keeps a copy of what it replaces in /root/downloads-before-android-<version>/, and points the website's
#    download link (RedVault-Messages.apk) at the new APK.
param([string]$Notes = "", [string]$Apk = "", [switch]$SkipUpload)
$ErrorActionPreference = "Stop"

$android = $PSScriptRoot
$repo = Split-Path $android -Parent
if (-not $Apk) { $Apk = Join-Path $android "app\build\outputs\apk\release\app-release.apk" }
if (-not (Test-Path $Apk)) { throw "No APK at $Apk (run gradlew assembleRelease first)" }
$release = Join-Path $repo "desktop\build\release\tools\release\redvault_release.exe"
if (-not (Test-Path $release)) { throw "Build the desktop's release tool first (cmake --build desktop\build\release --target redvault_release)" }
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH

# Version and versionCode from the APK itself, so the manifest can't disagree with it.
$buildTools = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\build-tools" -Directory | Sort-Object Name | Select-Object -Last 1
$badging = & (Join-Path $buildTools.FullName "aapt2.exe") dump badging $Apk | Select-Object -First 1
if ($badging -notmatch "versionCode='(\d+)' versionName='([0-9.]+)'") { throw "Couldn't read the APK's version" }
$code = $Matches[1]; $version = $Matches[2]
$signer = & (Join-Path $buildTools.FullName "apksigner.bat") verify --print-certs $Apk | Select-String "Signer #1 certificate DN"
if (-not $signer) { throw "The APK isn't signed" }
Write-Host "RedVaultMessages $version (versionCode $code), $signer" -ForegroundColor Cyan

$out = Join-Path $android "dist"
New-Item -ItemType Directory -Force $out | Out-Null
$named = Join-Path $out "RedVault-Messages-$version.apk"
Copy-Item $Apk $named -Force
$manifest = Join-Path $out "android-latest.json"
& $release sign-android --apk $named --version $version --version-code $code --notes $Notes --out $manifest
if ($LASTEXITCODE -ne 0) { throw "Signing the manifest failed" }
if ($SkipUpload) { Write-Host "Signed, not uploaded: $out"; exit 0 }

$sha = (Get-FileHash $named -Algorithm SHA256).Hash.ToLower()
$dir = "/var/www/redvaultmessages/desktop"
Write-Host "Uploading the APK..." -ForegroundColor Cyan
scp $named "redvault-live:$dir/RedVault-Messages-$version.apk"
if ($LASTEXITCODE -ne 0) { throw "APK upload failed" }
scp $manifest "redvault-live:$dir/android-latest.json.new"
if ($LASTEXITCODE -ne 0) { throw "Manifest upload failed" }
$remote = @"
set -e
cd $dir
echo "$sha  RedVault-Messages-$version.apk" | sha256sum -c -
mkdir -p /root/downloads-before-android-$version
cp -p RedVault-Messages.apk /root/downloads-before-android-$version/ 2>/dev/null || true
cp -p android-latest.json /root/downloads-before-android-$version/ 2>/dev/null || true
chmod 644 RedVault-Messages-$version.apk android-latest.json.new
cp -p RedVault-Messages-$version.apk .swap.apk && mv -f .swap.apk RedVault-Messages.apk
mv -f android-latest.json.new android-latest.json
ls -la RedVault-Messages.apk RedVault-Messages-$version.apk android-latest.json
"@
($remote -replace "`r", "") | ssh redvault-live "tr -d '\r' | bash -s"
if ($LASTEXITCODE -ne 0) { throw "The step on the server failed" }
Write-Host "`nPublished $version. Installed apps from 0.6.2 on offer it within a day (or at once from Settings > About)." -ForegroundColor Green
