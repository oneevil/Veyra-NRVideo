# SPDX-License-Identifier: GPL-3.0-only
# Turns a Windows 10/11 PC into a GitHub Actions runner for voice-release.yml: installs the
# build tools, then registers a runner with the label "veyra" for this repository.
#
# Run in PowerShell **as administrator**:
#     powershell -ExecutionPolicy Bypass -File setup-windows-runner.ps1 -Token <registration token>
# The token is from the repository: Settings -> Actions -> Runners -> New self-hosted runner
# (or `gh api -X POST repos/<owner>/<repo>/actions/runners/registration-token`); it lasts an hour.
#
# The runner runs in your session (not as a service), so the tools installed for your user
# (Rust) are visible to it; it starts again at every logon. Everything lives in one folder,
# C:\Distr\veyra-ci (the runner in runner\, the dependencies and the build folder beside it,
# about 80 GB), kept between builds: that is what makes a rebuild take minutes instead of hours.
# The finished packages land in C:\Distr\veyra-ci\rel. Remove the runner: run with -Remove;
# then the whole folder can be deleted.
param(
    [string]$Token,
    [string]$Repo = "oneevil/Veyra-NRVideo",
    [string]$Folder = "C:\Distr\veyra-ci\runner",
    [switch]$Remove
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "Run this PowerShell as administrator." }

if ($Remove) {
    if (-not $Token) { throw "-Remove needs a removal token (Settings -> Actions -> Runners -> the runner -> Remove)." }
    Unregister-ScheduledTask -TaskName "GitHub runner (Veyra)" -Confirm:$false -ErrorAction SilentlyContinue
    Push-Location $Folder
    .\config.cmd remove --token $Token
    Pop-Location
    return
}
if (-not $Token) { throw "Pass -Token <registration token>." }

function Install([string]$id, [string]$extra = "") {
    Write-Host "== $id"
    $wargs = @("install", "--id", $id, "--exact", "--silent", "--accept-package-agreements", "--accept-source-agreements")
    if ($extra) { $wargs += @("--override", $extra) }
    & winget @wargs
    # 0: installed; -1978335189: already installed and up to date
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) { throw "winget $id failed ($LASTEXITCODE)" }
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget is missing: install 'App Installer' from the Microsoft Store, then run this again."
}

# Long paths (chiaki-ng submodules) and git's own long-path switch.
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force | Out-Null

Install "Microsoft.PowerShell"
Install "Git.Git"
Install "Kitware.CMake"
Install "Ninja-build.Ninja"
Install "Python.Python.3.12" "/quiet InstallAllUsers=1 PrependPath=1 Include_test=0"
Install "7zip.7zip"
Install "GitHub.cli"
Install "Rustlang.Rustup"
# MSVC, the Windows 11 SDK (D3D12, dxc, midl) and ATL: what windows-2022 runners carry.
Install "Microsoft.VisualStudio.2022.BuildTools" ("--quiet --wait --norestart --nocache " +
    "--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 " +
    "--add Microsoft.VisualStudio.Component.Windows11SDK.26100 --add Microsoft.VisualStudio.Component.VC.ATL " +
    "--add Microsoft.VisualStudio.Component.VC.CMake.Project")

# Tools on PATH for this session (winget changed the machine PATH).
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
$sevenZip = "C:\Program Files\7-Zip"
if ((Test-Path $sevenZip) -and ([Environment]::GetEnvironmentVariable("Path", "Machine") -notlike "*7-Zip*")) {
    [Environment]::SetEnvironmentVariable("Path", [Environment]::GetEnvironmentVariable("Path", "Machine") + ";$sevenZip", "Machine")
    $env:Path += ";$sevenZip"
}
git config --system core.longpaths true
rustup default stable
rustup target add x86_64-pc-windows-msvc

# The runner itself, checked against the digest GitHub publishes for the asset.
$release = Invoke-RestMethod https://api.github.com/repos/actions/runner/releases/latest
$asset = $release.assets | Where-Object { $_.name -match '^actions-runner-win-x64-[\d.]+\.zip$' } | Select-Object -First 1
if (-not $asset) { throw "runner download not found" }
New-Item -ItemType Directory -Force $Folder | Out-Null
$zip = Join-Path $Folder $asset.name
Invoke-WebRequest -UseBasicParsing $asset.browser_download_url -OutFile $zip
if ($asset.digest) {
    $want = ($asset.digest -replace '^sha256:', '').ToUpperInvariant()
    if ((Get-FileHash $zip).Hash -ne $want) { throw "runner archive hash mismatch" }
}
Expand-Archive $zip $Folder -Force
Remove-Item $zip

Push-Location $Folder
.\config.cmd --unattended --replace --url "https://github.com/$Repo" --token $Token `
    --name "$env:COMPUTERNAME-veyra" --labels veyra --work _work
Pop-Location

# Start at every logon of this user, in a window of its own.
$action = New-ScheduledTaskAction -Execute "$Folder\run.cmd" -WorkingDirectory $Folder
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName "GitHub runner (Veyra)" -Action $action -Trigger $trigger -Settings $settings -RunLevel Limited -Force | Out-Null
Start-ScheduledTask -TaskName "GitHub runner (Veyra)"

Write-Host ""
Write-Host "Runner '$env:COMPUTERNAME-veyra' registered and started (label: veyra)."
Write-Host "It starts at every logon; builds need this PC on and logged in."
