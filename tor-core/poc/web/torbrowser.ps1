# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Starts Tor Browser (the portable build, already extracted and signature-checked)
# with Marionette on, a fresh private profile, and a connection sampler that
# records every TCP/UDP endpoint of Tor Browser's processes, for the leak check
# (check_relays.py against the consensus Tor Browser's own tor downloaded).
#
#   torbrowser.ps1 -Root <extracted dir with Browser\> -Out <dir>
#
# Marionette runs with -remote-allow-system-access (test only), so the probe can read Tor Browser's
# own state and set its security level through its own code (torbrowser_probe.py security-level).
#
# It leaves Tor Browser running; create <Out>\stop to end the sampler, and stop
# Tor Browser itself by the PID it writes to <Out>\firefox.pid.
param(
    [Parameter(Mandatory)] [string] $Root,
    [Parameter(Mandatory)] [string] $Out
)
$ErrorActionPreference = "Stop"
$browser = Join-Path $Root "Browser"
$profile = Join-Path $browser "TorBrowser\Data\Browser\profile.default"
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item (Join-Path $Out "stop") -ErrorAction SilentlyContinue

@"
user_pref("torbrowser.settings.quickstart.enabled", true);
user_pref("app.update.auto", false);
user_pref("app.update.enabled", false);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage", "about:blank");
user_pref("startup.homepage_welcome_url", "about:blank");
user_pref("startup.homepage_welcome_url.additional", "");
user_pref("marionette.port", 2828);
"@ | Set-Content -Path (Join-Path $profile "user.js") -Encoding ascii

$firefox = Start-Process -FilePath (Join-Path $browser "firefox.exe") -ArgumentList @("-no-remote", "-profile", "`"$profile`"", "-marionette", "-remote-allow-system-access", "about:blank") `
    -WorkingDirectory $browser -PassThru
$firefox.Id | Set-Content (Join-Path $Out "firefox.pid")

# Sampler (its own process, so it outlives this script): every process whose image lives under Tor Browser.
Start-Process -FilePath "powershell.exe" -WindowStyle Hidden -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSScriptRoot\sampler.ps1`"", "-Root", "`"$Root`"", "-Out", "`"$Out`"")
"firefox pid $($firefox.Id); output in $Out"
