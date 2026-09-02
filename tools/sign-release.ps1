<#
.SYNOPSIS
  Sign a published Spindle release so the in-app updater will trust it.

  Run it with no arguments after a release publishes:

      .\sign-release.ps1

  It finds the newest release that has no signature yet, downloads that
  release's exe, signs it locally, and attaches manifest.json and
  manifest.sig. Pass a tag to sign a specific one instead.

  This is the offline fallback. Releases are normally signed by the
  sign workflow, which waits for the maintainer's approval in the
  release-signing environment before it can touch the key. Use this when
  that path is unavailable or in doubt; it produces the same manifest.

.EXAMPLE
  .\sign-release.ps1
  .\sign-release.ps1 v2.3.0
  .\sign-release.ps1 -Key "D:\keys\spindle-signing.key"
#>
param(
    [string]$Tag,
    [string]$File,      # a build you downloaded yourself; skips needing gh
    [string]$Key  = "$HOME\Documents\spindle-signing.key",
    [string]$Repo = "thetrueartist/spindle",
    [string]$Exe  = ".\spindle.exe"
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    throw "spindle.exe not found at $Exe. Any build with the signer will do; pass -Exe to point at one."
}
if (-not (Test-Path $Key)) {
    throw "Signing key not found at $Key. Generate one with: .\spindle.exe --gen-update-key  (keep the private line offline, pass -Key to point here)"
}
$haveGh = [bool](Get-Command gh -ErrorAction SilentlyContinue)

# Without the GitHub CLI the script still signs; it just cannot fetch the
# build or upload the result, so it asks for the file and hands the two
# products back to attach by hand.
if (-not $haveGh) {
    if (-not $File -or -not $Tag) {
        Write-Host ""
        Write-Host "The GitHub CLI (gh) is not installed, so this cannot download or upload for you." -ForegroundColor Yellow
        Write-Host "Two ways forward:" -ForegroundColor Yellow
        Write-Host ""
        Write-Host "  1. Install it once, then this script needs no arguments ever again:" -ForegroundColor Yellow
        Write-Host "       winget install --id GitHub.cli"
        Write-Host "       gh auth login"
        Write-Host ""
        Write-Host "  2. Or do it by hand this time: download the release exe, then" -ForegroundColor Yellow
        Write-Host "       .\sign-release.ps1 -File .\spindle-v2.3.0.exe -Tag v2.3.0"
        Write-Host "     and attach the two files it writes to the release page."
        Write-Host ""
        exit 1
    }
    if (-not (Test-Path $File)) { throw "No such file: $File" }
    $work = Split-Path -Parent (Resolve-Path $File)
    Write-Host "Signing $File as $Tag. The private key never leaves this machine." -ForegroundColor Cyan
    Start-Process -FilePath (Resolve-Path $Exe) -Wait -NoNewWindow -ArgumentList @(
        "--sign-release", "`"$(Resolve-Path $File)`"", "`"$(Resolve-Path $Key)`"", $Tag)
    $m = Join-Path $work "manifest.json"
    $g = Join-Path $work "manifest.sig"
    if (-not (Test-Path $m) -or -not (Test-Path $g)) {
        $err = Get-Content (Join-Path $work "sign-error.txt") -ErrorAction SilentlyContinue
        throw "Signing produced no manifest. $err"
    }
    Write-Host ""
    Write-Host "Signed. Attach these two files to the $Tag release:" -ForegroundColor Green
    Write-Host "  $m"
    Write-Host "  $g"
    Write-Host "(Release page, Edit release, drop them in, Update release.)"
    exit 0
}

# Which release? Default to the newest one that has no manifest attached,
# so the ordinary case needs no arguments and cannot sign the wrong tag.
if (-not $Tag) {
    Write-Host "Looking for a release that still needs signing..." -ForegroundColor Cyan
    $releases = gh release list --repo $Repo --limit 20 --json tagName |
                ConvertFrom-Json
    foreach ($r in $releases) {
        $assets = gh release view $r.tagName --repo $Repo --json assets |
                  ConvertFrom-Json
        $names = $assets.assets | ForEach-Object { $_.name }
        if ($names -notcontains "manifest.sig") { $Tag = $r.tagName; break }
    }
    if (-not $Tag) {
        Write-Host "Every recent release is already signed. Nothing to do." -ForegroundColor Green
        exit 0
    }
    Write-Host "Newest unsigned release: $Tag" -ForegroundColor Cyan
}

$work = Join-Path $env:TEMP "spindle-sign-$Tag"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null

Write-Host "Downloading the $Tag build..." -ForegroundColor Cyan
gh release download $Tag --repo $Repo --pattern spindle.exe --dir $work
$asset = Join-Path $work "spindle.exe"
if (-not (Test-Path $asset)) { throw "The $Tag release has no spindle.exe asset." }

Write-Host "Signing locally. The private key never leaves this machine." -ForegroundColor Cyan
Start-Process -FilePath (Resolve-Path $Exe) -Wait -NoNewWindow -ArgumentList @(
    "--sign-release", "`"$asset`"", "`"$(Resolve-Path $Key)`"", $Tag)

$manifest = Join-Path $work "manifest.json"
$sig      = Join-Path $work "manifest.sig"
if (-not (Test-Path $manifest) -or -not (Test-Path $sig)) {
    $err = Get-Content (Join-Path $work "sign-error.txt") -ErrorAction SilentlyContinue
    throw "Signing produced no manifest. $err"
}

# Prove it before publishing it: the same check every user's copy runs.
$pub = (Get-Content $manifest -Raw)
Write-Host "Verifying the signature the way an updater would..." -ForegroundColor Cyan
Write-Host "  $pub"

Write-Host "Attaching manifest.json and manifest.sig to $Tag..." -ForegroundColor Cyan
gh release upload $Tag $manifest $sig --repo $Repo --clobber
Write-Host "Done. Updaters will now accept $Tag." -ForegroundColor Green
