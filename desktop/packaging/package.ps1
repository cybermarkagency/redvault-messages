# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Builds the redvaultmessages installer: release build, tests, a self-contained
# dist\redvaultmessages folder, then dist\redvaultmessages-Setup-<version>.exe.
#
#   powershell -ExecutionPolicy Bypass -File packaging\package.ps1
#
# Needs MSYS2 UCRT64 (C:\msys64) with the packages in README.md, and
# Inno Setup 6. Unsigned: Windows SmartScreen will warn until the installer
# and redvaultmessages.exe are signed with a code-signing certificate.

$ErrorActionPreference = "Stop"
$desktop = Split-Path -Parent $PSScriptRoot
# One version for everything: project(RedvaultDesktop VERSION x.y.z ...) in CMakeLists.txt.
$version = (Select-String -Path (Join-Path $desktop "CMakeLists.txt") -Pattern 'project\(RedvaultDesktop VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$releaseKey = Join-Path $env:USERPROFILE ".redvault\release-signing-key.json"
$msys = "C:\msys64\ucrt64\bin"
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not (Test-Path "$msys\cmake.exe")) { throw "MSYS2 UCRT64 not found at $msys" }
if (-not $iscc) { throw "Inno Setup 6 not found (https://jrsoftware.org/isinfo.php)" }

# cargo builds the device-key engine (crypto-core).
$env:PATH = "$msys;" + (Join-Path $env:USERPROFILE ".cargo\bin") + ";" + $env:PATH
Push-Location $desktop
try {
    function Invoke-Step([string]$label, [scriptblock]$step) {
        Write-Host "== $label" -ForegroundColor Cyan
        & $step
        if ($LASTEXITCODE -ne 0) { throw "$label failed (exit $LASTEXITCODE)" }
    }

    # Settings > About shows this version's source link and contact from these (empty: "not published yet" and
    # the Terms of Service contact). Always passed, so a value left in the build cache from an earlier run can't stick.
    # Release builds carry device keys with Signal sessions: the release preset sets both, and they're passed for the same reason.
    # Tor by default (docs/tor-by-default.md): every published build is Tor-only at the official onion service.
    $sourceArgs = @("-DREDVAULT_SOURCE_CODE_URL=$env:REDVAULT_SOURCE_CODE_URL", "-DREDVAULT_SOURCE_CONTACT=$env:REDVAULT_SOURCE_CONTACT",
        "-DREDVAULT_E2E_V3=ON", "-DREDVAULT_E2E_SIGNAL=ON") + (& (Join-Path $PSScriptRoot "tor-release-args.ps1"))
    Invoke-Step "Configure (release)" { cmake --preset release @sourceArgs }
    & (Join-Path $PSScriptRoot "tor-release-args.ps1") -Verify "build\release\CMakeCache.txt"
    Invoke-Step "Build" { cmake --build --preset release }
    Invoke-Step "Test" { ctest --preset release }

    if (Test-Path dist) { Remove-Item -Recurse -Force dist }
    Invoke-Step "Stage dist\redvaultmessages" { cmake --install build\release --prefix dist\redvaultmessages }
    # The app's licence (AGPL-3.0-only) and the third-party notices must ship with every copy.
    foreach ($legal in @("LICENSE.txt", "THIRD_PARTY_NOTICES.txt")) {
        if (-not (Test-Path "dist\redvaultmessages\licenses\$legal")) { throw "dist\redvaultmessages\licenses\$legal is missing" }
    }
    Invoke-Step "Build installer" { & $iscc "/DAppVersion=$version" packaging\redvault.iss }

    $setup = Join-Path $desktop "dist\redvaultmessages-Setup-$version.exe"
    $size = "{0:N1} MB" -f ((Get-Item $setup).Length / 1MB)
    Write-Host "`nInstaller: $setup ($size)" -ForegroundColor Green
    Write-Host "SHA-256:   $((Get-FileHash $setup -Algorithm SHA256).Hash)"

    # Sign the update manifest so installed copies pick this release up.
    if (Test-Path $releaseKey) {
        $notes = Read-Host "Release notes for $version (one line, shown to users; Enter for none)"
        # Windows PowerShell drops an empty argument, so "--notes ''" would reach the tool as a bare "--notes".
        $notesArgs = if ($notes) { @("--notes", $notes) } else { @() }
        Invoke-Step "Sign update manifest" {
            & build\release\tools\release\redvault_release.exe sign --installer $setup --version $version @notesArgs --out dist\latest.json
        }
        Write-Host "`nTo publish, upload BOTH files to the droplet (installer first, then the manifest):" -ForegroundColor Green
        Write-Host "  scp `"$setup`" root@<droplet>:/var/www/redvaultmessages/desktop/"
        Write-Host "  scp dist\latest.json root@<droplet>:/var/www/redvaultmessages/desktop/"
    } else {
        Write-Host "`nNo release key at $releaseKey, so no update manifest was signed." -ForegroundColor Yellow
        Write-Host "Create one once with: build\release\tools\release\redvault_release.exe keygen --embed app\src\update_key.hpp"
    }

    # The Linux AppImage is built by GitHub Actions; fetch it into dist\ too.
    # A problem there never undoes the Windows release above.
    try {
        & (Join-Path $PSScriptRoot "fetch-linux.ps1")
    } catch {
        Write-Host "`nNo Linux AppImage this time: $($_.Exception.Message)" -ForegroundColor Yellow
        Write-Host "Get it later with: powershell -ExecutionPolicy Bypass -File packaging\fetch-linux.ps1"
    }
} finally {
    Pop-Location
}
