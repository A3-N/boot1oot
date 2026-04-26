param(
    [string]$Distro = "",
    [string]$BuildrootDir = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$repo = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($repo)
$distroArgs = @()

if ($Distro -ne "") {
    $distroArgs = @("-d", $Distro)
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw "wsl.exe was not found. Install WSL and a Linux distribution before building Boot1oot."
}

$linuxRepo = (& wsl.exe @distroArgs --exec wslpath -a "$repo").Trim()
if (-not $linuxRepo) {
    throw "Failed to translate repository path for WSL: $repo"
}

$linuxRepoQuoted = $linuxRepo.Replace("'", "'\''")
$cleanPath = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
$cleanEnv = ""

if ($Clean) {
    $cleanEnv = "CLEAN=1 "
}

if ($BuildrootDir -eq "") {
    & wsl.exe @distroArgs --exec bash -lc "export PATH='$cleanPath'; cd '$linuxRepoQuoted' && ${cleanEnv}bash scripts/build.sh"
    exit $LASTEXITCODE
}

if ($BuildrootDir -match "^~[\\/]") {
    $linuxBuildroot = "~/" + $BuildrootDir.Substring(2).Replace("\", "/")
} elseif ($BuildrootDir -match "^(~|/)") {
    $linuxBuildroot = $BuildrootDir
} else {
    $buildroot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildrootDir)
    $linuxBuildroot = (& wsl.exe @distroArgs --exec wslpath -a "$buildroot").Trim()
}

if (-not $linuxBuildroot) {
    throw "Failed to translate Buildroot path for WSL: $BuildrootDir"
}

$linuxBuildrootQuoted = $linuxBuildroot.Replace("'", "'\''")
& wsl.exe @distroArgs --exec bash -lc "export PATH='$cleanPath'; cd '$linuxRepoQuoted' && ${cleanEnv}BUILDROOT_DIR='$linuxBuildrootQuoted' bash scripts/build.sh"
