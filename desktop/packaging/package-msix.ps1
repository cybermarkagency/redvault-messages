# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Builds the Microsoft Store package: dist\redvaultmessages-<version>.msix.
# Same files as the Inno Setup installer, wrapped as an MSIX that the Store
# signs itself (no code-signing certificate needed). In a Store install the
# app's own updater is off: the Store updates it.
#
#   powershell -ExecutionPolicy Bypass -File packaging\package-msix.ps1
#       Release build, tests, then the .msix to upload in Partner Center.
#       Needs packaging\msix\store-identity.json filled in from Partner Center.
#
#   powershell -ExecutionPolicy Bypass -File packaging\package-msix.ps1 -Register
#       Installs the unpacked package on this PC to try it the way Store users
#       will get it (Start menu entry, no updater). Needs Windows Developer Mode
#       (Settings > System > For developers); works with the placeholder identity.
#       Remove it again under Settings > Apps.
#
# Needs MSYS2 UCRT64 (C:\msys64) with the packages in README.md, and the
# Windows SDK (makeappx.exe, makepri.exe): winget install Microsoft.WindowsSDK.10.0.26100
param(
    [switch]$Register,
    # Reuse build\release as it is (skips configure, build and tests).
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$desktop = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path (Join-Path $desktop "CMakeLists.txt") -Pattern 'project\(RedvaultDesktop VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$msys = "C:\msys64\ucrt64\bin"
$identity = Get-Content (Join-Path $PSScriptRoot "msix\store-identity.json") -Raw | ConvertFrom-Json
$placeholder = ($identity.name, $identity.publisher, $identity.publisherDisplayName) -match "REPLACE-ME"

if (-not (Test-Path "$msys\cmake.exe")) { throw "MSYS2 UCRT64 not found at $msys" }
if ($placeholder -and -not $Register) {
    throw "Fill in packaging\msix\store-identity.json from Partner Center (Product identity) first."
}
$sdkBin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\makeappx.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 | ForEach-Object { $_.DirectoryName }
if (-not $sdkBin -and -not $Register) { throw "Windows SDK not found. Install it: winget install Microsoft.WindowsSDK.10.0.26100" }

$env:PATH = "$msys;" + $env:PATH
Push-Location $desktop
try {
    function Invoke-Step([string]$label, [scriptblock]$step) {
        Write-Host "== $label" -ForegroundColor Cyan
        & $step
        if ($LASTEXITCODE -ne 0) { throw "$label failed (exit $LASTEXITCODE)" }
    }

    if (-not $SkipBuild) {
        Invoke-Step "Configure (release)" { cmake --preset release }
        Invoke-Step "Build" { cmake --build --preset release }
        Invoke-Step "Test" { ctest --preset release }
    }

    # The package's contents: the installed app, the manifest and the logos.
    $root = Join-Path $desktop "build\msix\root"
    if ($Register) {
        # A registered package runs from this folder; replace the previous copy.
        Get-AppxPackage -Name $identity.name -ErrorAction SilentlyContinue | Remove-AppxPackage
    }
    if (Test-Path $root) { Remove-Item -Recurse -Force $root }
    Invoke-Step "Stage the app" { cmake --install build\release --prefix $root }
    Copy-Item -Recurse (Join-Path $PSScriptRoot "msix\Assets") (Join-Path $root "Assets")
    # Store versions have four parts, and the last must be 0.
    $manifest = (Get-Content (Join-Path $PSScriptRoot "msix\AppxManifest.xml.in") -Raw).
        Replace("@IDENTITY_NAME@", $identity.name).
        Replace("@PUBLISHER@", $identity.publisher).
        Replace("@PUBLISHER_DISPLAY_NAME@", $identity.publisherDisplayName).
        Replace("@DISPLAY_NAME@", $identity.displayName).
        Replace("@VERSION@", "$version.0")
    [IO.File]::WriteAllText((Join-Path $root "AppxManifest.xml"), $manifest, (New-Object Text.UTF8Encoding $false))

    if ($sdkBin) {
        # resources.pri lets Windows pick the right logo size (scale-200, targetsize-*).
        $priConfig = Join-Path $desktop "build\msix\priconfig.xml"
        Invoke-Step "Index resources" { & "$sdkBin\makepri.exe" createconfig /cf $priConfig /dq en-US /o }
        Invoke-Step "Build resources.pri" {
            & "$sdkBin\makepri.exe" new /pr $root /cf $priConfig /mn (Join-Path $root "AppxManifest.xml") /of (Join-Path $root "resources.pri") /o
        }
    }

    if ($Register) {
        Write-Host "== Register on this PC" -ForegroundColor Cyan
        Add-AppxPackage -Register (Join-Path $root "AppxManifest.xml")
        Write-Host "`nInstalled: find $($identity.displayName) in the Start menu." -ForegroundColor Green
        Write-Host "Settings > About should say updates come from the Microsoft Store."
        return
    }

    New-Item -ItemType Directory -Force dist | Out-Null
    $msix = Join-Path $desktop "dist\redvaultmessages-$version.msix"
    Invoke-Step "Pack" { & "$sdkBin\makeappx.exe" pack /d $root /p $msix /o }
    $size = "{0:N1} MB" -f ((Get-Item $msix).Length / 1MB)
    Write-Host "`nStore package: $msix ($size)" -ForegroundColor Green
    Write-Host "Upload it in Partner Center: your app > new submission > Packages. The Store signs it."
} finally {
    Pop-Location
}
