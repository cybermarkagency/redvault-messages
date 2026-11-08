# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Samples every TCP connection and UDP endpoint of every process whose image is under -Root
# (Tor Browser's firefox, tor and lyrebird), every 0.3 s, until <Out>\stop exists.
param([Parameter(Mandatory)] [string] $Root, [Parameter(Mandatory)] [string] $Out)
$seen = @{}
$log = Join-Path $Out "connections.txt"
Set-Content -Path $log -Value "kind|local|remote|port|state" -Encoding utf8
while (-not (Test-Path (Join-Path $Out "stop"))) {
    $ids = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { $_.Id })
    foreach ($id in $ids) {
        Get-NetTCPConnection -OwningProcess $id -ErrorAction SilentlyContinue | ForEach-Object {
            $key = "TCP|$($_.LocalAddress)|$($_.RemoteAddress)|$($_.RemotePort)|$($_.State)"
            if (-not $seen.ContainsKey($key)) { $seen[$key] = 1; Add-Content -Path $log -Value $key -Encoding utf8 }
        }
        Get-NetUDPEndpoint -OwningProcess $id -ErrorAction SilentlyContinue | ForEach-Object {
            $key = "UDP|$($_.LocalAddress)|$($_.LocalPort)||"
            if (-not $seen.ContainsKey($key)) { $seen[$key] = 1; Add-Content -Path $log -Value $key -Encoding utf8 }
        }
    }
    Start-Sleep -Milliseconds 300
}
