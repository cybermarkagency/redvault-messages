# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Puts the Linux AppImage next to the Windows installer: dist\redvaultmessages-<version>-x86_64.AppImage.
#
#   powershell -ExecutionPolicy Bypass -File packaging\fetch-linux.ps1
#
# The AppImage can't be built on Windows; GitHub Actions builds it
# (.github/workflows/desktop-linux.yml) whenever desktop/ changes are pushed.
# This asks GitHub for the newest such build on the release branch (main unless
# -Branch says otherwise), and only from a push, never a run that a pull request
# or a test branch could have produced. It waits for it if it's still running,
# and downloads it, but only if that commit is on the branch and it's the same
# version as desktop\CMakeLists.txt, so an old build is never passed off as new.
# package.ps1 runs this at the end; it also works on its own.
#
# Needs the GitHub CLI, signed in once:  gh auth login
param(
    # How long to wait for a build that's still running.
    [int]$WaitMinutes = 30,
    # Only builds of this branch are taken.
    [string]$Branch = "main"
)

$ErrorActionPreference = "Stop"
$desktop = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $desktop
$version = (Select-String -Path (Join-Path $desktop "CMakeLists.txt") -Pattern 'project\(RedvaultDesktop VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$artifact = "redvaultmessages-linux-x86_64"

$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh -and (Test-Path "$env:ProgramFiles\GitHub CLI\gh.exe")) { $gh = "$env:ProgramFiles\GitHub CLI\gh.exe" }
if (-not $gh) { throw "GitHub CLI not found. Install it once: winget install --id GitHub.cli" }

# gh and git print progress on stderr, which Windows PowerShell 5.1 turns into
# terminating errors under "Stop"; judge them by exit code instead.
function Invoke-Gh {
    $ErrorActionPreference = "Continue"
    & $gh @args 2>&1 | ForEach-Object { if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host $_ } else { $_ } }
}
function Invoke-Git {
    $ErrorActionPreference = "Continue"
    & git @args 2>$null
}

Invoke-Gh auth status | Out-Null
if ($LASTEXITCODE -ne 0) { throw "GitHub CLI isn't signed in. Run this once: gh auth login" }

Push-Location $repoRoot
try {
    Invoke-Git fetch origin $Branch | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Couldn't fetch origin/$Branch." }
    $ahead = [int](Invoke-Git rev-list --count "origin/$Branch..HEAD")
    if ($ahead -gt 0) {
        Write-Host "Note: $ahead commit(s) here aren't on origin/$Branch yet; GitHub can only build what's been pushed." -ForegroundColor Yellow
    }

    Write-Host "== Linux AppImage $version (GitHub Actions, $Branch)" -ForegroundColor Cyan
    $runs = (Invoke-Gh run list --workflow desktop-linux.yml --branch $Branch --event push --limit 1 --json "databaseId,status,conclusion,headSha,headBranch,event,url") -join "`n" | ConvertFrom-Json
    if (-not $runs) { throw "No Linux build of $Branch on GitHub yet. Push desktop changes to it." }
    $run = $runs[0]
    # Don't take the filter's word for it.
    if ($run.headBranch -ne $Branch -or $run.event -ne "push") { throw "GitHub returned a build of $($run.headBranch) ($($run.event)), not a push to $Branch." }
    Invoke-Git merge-base --is-ancestor $run.headSha "origin/$Branch" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "That build's commit ($($run.headSha.Substring(0, 7))) isn't on origin/$Branch`: $($run.url)" }

    if ($run.status -ne "completed") {
        Write-Host "The build is still running; waiting up to $WaitMinutes minutes ($($run.url))"
        $deadline = (Get-Date).AddMinutes($WaitMinutes)
        do {
            Start-Sleep -Seconds 20
            $run = (Invoke-Gh run view $run.databaseId --json "databaseId,status,conclusion,headSha,headBranch,event,url") -join "`n" | ConvertFrom-Json
        } while ($run.status -ne "completed" -and (Get-Date) -lt $deadline)
        if ($run.status -ne "completed") { throw "The Linux build is still running after $WaitMinutes minutes: $($run.url)" }
    }
    if ($run.conclusion -ne "success") { throw "The newest Linux build failed ($($run.conclusion)): $($run.url)" }

    $temp = Join-Path ([IO.Path]::GetTempPath()) ("redvault-linux-" + [guid]::NewGuid())
    try {
        Invoke-Gh run download $run.databaseId --name $artifact --dir $temp | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Couldn't download the Linux build: $($run.url)" }
        $appImage = Get-ChildItem $temp -Filter "*.AppImage" | Select-Object -First 1
        if (-not $appImage) { throw "The Linux build has no AppImage in it: $($run.url)" }
        if ($appImage.Name -notlike "*-$version-*") {
            throw "The newest Linux build is $($appImage.Name), not $version. Push the version change and run this again once GitHub has built it."
        }
        New-Item -ItemType Directory -Force (Join-Path $desktop "dist") | Out-Null
        $target = Join-Path $desktop "dist\$($appImage.Name)"
        Move-Item -Force $appImage.FullName $target
    } finally {
        Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
    }

    $size = "{0:N1} MB" -f ((Get-Item $target).Length / 1MB)
    Write-Host "`nLinux:     $target ($size)" -ForegroundColor Green
    Write-Host "SHA-256:   $((Get-FileHash $target -Algorithm SHA256).Hash)"
    Write-Host "Built by:  $($run.url) ($($run.headBranch), commit $($run.headSha.Substring(0, 7)))"
} finally {
    Pop-Location
}
