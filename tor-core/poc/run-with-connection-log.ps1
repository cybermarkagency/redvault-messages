# Starts a program and records every TCP connection and UDP endpoint the OS
# reports for its process (sampled every 200 ms until it exits). Used by the
# Windows leak test: afterwards every remote address must be loopback or a
# Tor relay listed in the consensus the gateway downloaded.
#
# Sampling can miss a connection that opens and closes within one interval;
# the transport's own socket guard covers that gap for libcurl.
param(
    [Parameter(Mandatory)] [string] $Exe,
    [string] $ArgsLine = "",
    [Parameter(Mandatory)] [string] $StdOut,
    [Parameter(Mandatory)] [string] $StdErr,
    [Parameter(Mandatory)] [string] $ConnLog
)
$startArgs = @{ FilePath = $Exe; NoNewWindow = $true; PassThru = $true; RedirectStandardOutput = $StdOut; RedirectStandardError = $StdErr }
if ($ArgsLine.Length -gt 0) { $startArgs.ArgumentList = $ArgsLine }
$proc = Start-Process @startArgs
$seen = @{}
Set-Content -Path $ConnLog -Value "kind|local|remote|port|state" -Encoding utf8
while (-not $proc.HasExited) {
    Get-NetTCPConnection -OwningProcess $proc.Id -ErrorAction SilentlyContinue | ForEach-Object {
        $key = "TCP|$($_.LocalAddress)|$($_.RemoteAddress)|$($_.RemotePort)|$($_.State)"
        if (-not $seen.ContainsKey($key)) { $seen[$key] = 1; Add-Content -Path $ConnLog -Value $key -Encoding utf8 }
    }
    Get-NetUDPEndpoint -OwningProcess $proc.Id -ErrorAction SilentlyContinue | ForEach-Object {
        $key = "UDP|$($_.LocalAddress)|$($_.LocalPort)||"
        if (-not $seen.ContainsKey($key)) { $seen[$key] = 1; Add-Content -Path $ConnLog -Value $key -Encoding utf8 }
    }
    Start-Sleep -Milliseconds 200
}
$proc.WaitForExit()
"exit=$($proc.ExitCode)"
