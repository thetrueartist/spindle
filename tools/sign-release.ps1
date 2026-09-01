<#
.SYNOPSIS
  Sign a published Spindle release so the in-app updater will trust it.

  The private signing key stays on this machine and is never uploaded.
  This downloads the CI-built exe for the tag, signs it locally with your
  own spindle.exe, and attaches manifest.json + manifest.sig to the
  release. Run it once after each release you want delivered by
  auto-update.

.EXAMPLE
  .\sign-release.ps1 v2.1.0
  .\sign-release.ps1 v2.1.0 -Key "D:\keys\spindle-signing.key"
#>
param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$Key  = "$HOME\Documents\spindle-signing.key",
    [string]$Repo = "thetrueartist/spindle",
    [string]$Exe  = ".\spindle.exe"
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) { throw "spindle.exe not found at $Exe (any build with the signer works)" }
if (-not (Test-Path $Key)) { throw "signing key not found: $Key (generate with: spindle.exe --gen-update-key, keep the private line offline)" }

$work = Join-Path $env:TEMP "spindle-sign-$Tag"
New-Item -ItemType Directory -Force $work | Out-Null

Write-Host "Downloading the $Tag exe..." -ForegroundColor Cyan
$asset = Join-Path $work "spindle.exe"
Invoke-WebRequest "https://github.com/$Repo/releases/download/$Tag/spindle.exe" -OutFile $asset

Write-Host "Signing locally (the private key never leaves this machine)..." -ForegroundColor Cyan
Start-Process -FilePath (Resolve-Path $Exe) -ArgumentList @("--sign-release", "`"$asset`"", "`"$Key`"", $Tag) -Wait -NoNewWindow
if (-not (Test-Path (Join-Path $work "manifest.json"))) {
    throw "signing produced no manifest: $(Get-Content (Join-Path $work 'sign-error.txt') -ErrorAction SilentlyContinue)"
}

if (Get-Command gh -ErrorAction SilentlyContinue) {
    Write-Host "Attaching manifest.json + manifest.sig to $Tag..." -ForegroundColor Cyan
    gh release upload $Tag (Join-Path $work "manifest.json") (Join-Path $work "manifest.sig") --repo $Repo --clobber
    Write-Host "Done. Updaters will now accept $Tag." -ForegroundColor Green
} else {
    Write-Host "gh not found. Attach these to the $Tag release yourself:" -ForegroundColor Yellow
    Write-Host "  $(Join-Path $work 'manifest.json')"
    Write-Host "  $(Join-Path $work 'manifest.sig')"
}
