# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Builds the redvaultmessages installer: release build, tests, a self-contained
# dist\redvaultmessages folder, then dist\redvaultmessages-Setup-<version>.exe.
#
#   powershell -ExecutionPolicy Bypass -File packaging\package.ps1
#
# Needs MSYS2 UCRT64 (C:\msys64) with the packages in README.md, and
# Inno Setup 6.
#
# Code signing (Authenticode): if a code-signing certificate with its private
# key is in your certificate store (Current User > Personal) and the Windows
# SDK's signtool.exe is installed, every unsigned .exe/.dll in the app, the
# installer and its uninstaller are signed and timestamped. With more than one
# such certificate, pick one with $env:REDVAULT_SIGN_THUMBPRINT. With none, the
# build carries on unsigned (SmartScreen will warn). A key on a USB token or
# cloud key store may ask for its PIN while signing.
#
# Signing happens before the update manifest is signed: the manifest records
# the installer's SHA-256, and signing a file changes that hash.
#
#   powershell -ExecutionPolicy Bypass -File packaging\package.ps1 -SignOnly
#       Skips configure/build/test/stage and signs the dist\redvaultmessages
#       folder already staged by an earlier run, then rebuilds the installer.

param([switch]$SignOnly, [string]$Notes)

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

# --- Code signing setup -------------------------------------------------------
$timestampUrl = "http://timestamp.sectigo.com"
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Parent.Name -match '^\d+(\.\d+)+$' } |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
$thumbprint = $env:REDVAULT_SIGN_THUMBPRINT
if (-not $thumbprint) {
    $certs = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) })
    if ($certs.Count -gt 1) {
        $list = ($certs | ForEach-Object { "  $($_.Thumbprint)  $($_.Subject)" }) -join "`n"
        throw "More than one code-signing certificate. Pick one with `$env:REDVAULT_SIGN_THUMBPRINT:`n$list"
    }
    if ($certs.Count -eq 1) { $thumbprint = $certs[0].Thumbprint }
}
$signing = [bool]($thumbprint -and $signtool)
if ($signing) {
    $cert = Get-Item "Cert:\CurrentUser\My\$thumbprint" -ErrorAction SilentlyContinue
    if (-not $cert) { throw "No certificate $thumbprint in Current User > Personal" }
    Write-Host "Signing as: $($cert.Subject)" -ForegroundColor Cyan
} elseif ($thumbprint) {
    Write-Host "No signtool.exe found (install the Windows SDK), so this build is UNSIGNED." -ForegroundColor Yellow
} else {
    Write-Host "No code-signing certificate found, so this build is UNSIGNED." -ForegroundColor Yellow
}
if ($SignOnly -and -not $signing) { throw "-SignOnly needs a code-signing certificate and signtool.exe" }
$signArgs = @("sign", "/sha1", $thumbprint, "/fd", "sha256", "/tr", $timestampUrl, "/td", "sha256", "/d", "redvaultmessages")

function Invoke-Sign([string[]]$files) {
    # Batches keep the command line under Windows' length limit. A USB token or
    # the timestamp server sometimes fails one request when many files are
    # signed in a row, so a failed batch is retried (only its unsigned files).
    for ($i = 0; $i -lt $files.Count; $i += 20) {
        $batch = @($files[$i..([Math]::Min($i + 19, $files.Count - 1))])
        for ($try = 1; $true; $try++) {
            & $signtool @signArgs @batch
            if ($LASTEXITCODE -eq 0) { break }
            $batch = @($batch | Where-Object { (Get-AuthenticodeSignature $_).Status -ne "Valid" })
            if ($batch.Count -eq 0) { break }
            if ($try -ge 5) { throw "signtool failed (exit $LASTEXITCODE) after $try tries" }
            Write-Host "Signing failed (try $try of 5), retrying $($batch.Count) file(s) in 15 s..." -ForegroundColor Yellow
            Start-Sleep -Seconds 15
        }
    }
}

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
    if (-not $SignOnly) {
        Invoke-Step "Configure (release)" { cmake --preset release @sourceArgs }
    }
    & (Join-Path $PSScriptRoot "tor-release-args.ps1") -Verify "build\release\CMakeCache.txt"
    if ($SignOnly) {
        if (-not (Test-Path dist\redvaultmessages\bin\redvaultmessages.exe)) { throw "-SignOnly: nothing staged in dist\redvaultmessages" }
        Get-ChildItem dist -Filter *.exe | Remove-Item
        Remove-Item dist\latest.json -ErrorAction SilentlyContinue
    } else {
        Invoke-Step "Build" { cmake --build --preset release }
        Invoke-Step "Test" { ctest --preset release }

        if (Test-Path dist) { Remove-Item -Recurse -Force dist }
        Invoke-Step "Stage dist\redvaultmessages" { cmake --install build\release --prefix dist\redvaultmessages }
    }
    # The app's licence (AGPL-3.0-only) and the third-party notices must ship with every copy.
    foreach ($legal in @("LICENSE.txt", "THIRD_PARTY_NOTICES.txt")) {
        if (-not (Test-Path "dist\redvaultmessages\licenses\$legal")) { throw "dist\redvaultmessages\licenses\$legal is missing" }
    }
    if ($signing) {
        # Everything not already signed by its vendor: our exe, livekit_ffi.dll,
        # and the Qt/MSYS2 DLLs (Smart App Control can block unsigned DLLs).
        $unsigned = @(Get-ChildItem dist\redvaultmessages -Recurse -Include *.exe, *.dll |
            Where-Object { (Get-AuthenticodeSignature $_.FullName).Status -eq "NotSigned" } |
            Select-Object -ExpandProperty FullName)
        Invoke-Step "Sign app files ($($unsigned.Count))" { Invoke-Sign $unsigned }
    }

    # With signing on, Inno Setup signs the installer and its uninstaller itself
    # through the "rvsign" tool defined here ($q = quote, $f = quoted file name).
    $isccArgs = @("/DAppVersion=$version")
    if ($signing) {
        $signCmd = '$q' + $signtool + '$q ' + ($signArgs -join ' ') + ' $f'
        $isccArgs += @("/DSignInstaller", "/Srvsign=$signCmd")
    }
    Invoke-Step "Build installer" { & $iscc @isccArgs packaging\redvault.iss }

    $setup = Join-Path $desktop "dist\redvaultmessages-Setup-$version.exe"
    if ($signing) {
        Invoke-Step "Verify installer signature" { & $signtool verify /pa /q $setup }
        Write-Host "Signed and timestamped: $setup" -ForegroundColor Green
    }
    $size = "{0:N1} MB" -f ((Get-Item $setup).Length / 1MB)
    Write-Host "`nInstaller: $setup ($size)" -ForegroundColor Green
    Write-Host "SHA-256:   $((Get-FileHash $setup -Algorithm SHA256).Hash)"

    # Sign the update manifest so installed copies pick this release up.
    if (Test-Path $releaseKey) {
        # -Notes "..." skips the question (for unattended runs).
        $notes = if ($PSBoundParameters.ContainsKey("Notes")) { $Notes } else {
            Read-Host "Release notes for $version (one line, shown to users; Enter for none)" }
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
