# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Tor by default (docs/tor-by-default.md), for package.ps1 and package-msix.ps1.
#
#   & tor-release-args.ps1
#       Returns the CMake arguments every published build is configured with: the Tor gateway,
#       Tor-only, the official onion service (read from cmake\tor_settings.cmake, its one home),
#       and REDVAULT_RELEASE_BUILD, which makes the configure step refuse anything else.
#       Always passed, so a value left in the build cache can't stick.
#   & tor-release-args.ps1 -Verify build\release\CMakeCache.txt
#       Throws unless that configured build really is Tor-only at the official onion service.
#       A setting such as REDVAULT_TOR_ONLY=OFF (for a developer's local test build) never ships.

param([string]$Verify)

$ErrorActionPreference = "Stop"
$settings = Join-Path (Split-Path -Parent $PSScriptRoot) "cmake\tor_settings.cmake"
$match = Select-String -Path $settings -Pattern 'set\(REDVAULT_OFFICIAL_ONION_API_BASE "(http://[a-z2-7]{56}\.onion/[^"]*)"\)'
if (-not $match) { throw "No official onion service in $settings" }
$onion = $match.Matches[0].Groups[1].Value

if (-not $Verify) {
    return @("-DREDVAULT_TOR=ON", "-DREDVAULT_TOR_ONLY=ON", "-DREDVAULT_ONION_API_BASE=$onion", "-DREDVAULT_RELEASE_BUILD=ON")
}

if (-not (Test-Path $Verify)) { throw "$Verify is missing: configure the release build first" }
$cache = Get-Content $Verify
$expected = @("REDVAULT_TOR:BOOL=ON", "REDVAULT_TOR_ONLY:BOOL=ON", "REDVAULT_ONION_API_BASE:STRING=$onion", "REDVAULT_RELEASE_BUILD:BOOL=ON")
foreach ($line in $expected) {
    if (-not ($cache -contains $line)) {
        throw "This build isn't Tor-only at the official onion service ($line missing from $Verify). A published build never is anything else."
    }
}
Write-Host "Tor-only release build: $onion" -ForegroundColor Green
