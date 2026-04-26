param(
    [string]$Distro = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$repo = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($repo)
$distroArgs = @()

if ($Distro -ne "") {
    $distroArgs = @("-d", $Distro)
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw "wsl.exe was not found. Install WSL and a Linux distribution before running Boot1oot setup."
}

$linuxRepo = (& wsl.exe @distroArgs --exec wslpath -a "$repo").Trim()
if (-not $linuxRepo) {
    throw "Failed to translate repository path for WSL: $repo"
}

$linuxRepoQuoted = $linuxRepo.Replace("'", "'\''")
$cleanPath = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
& wsl.exe @distroArgs --exec bash -lc "export PATH='$cleanPath'; cd '$linuxRepoQuoted' && bash scripts/setup.sh"
