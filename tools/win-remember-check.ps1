# Round trip for "Remember where I was" on a real Windows desktop: the
# view that was open at close is the view that opens at launch, All
# drives included.
#
# Proved from what the program writes back, not from pixels. Settings are
# seeded with a view, the program is launched, left to load, and closed
# the way a person would; then its settings are read. A launch that fell
# back to a single drive writes last_all_drives=0; one that never finished
# loading writes no last_path at all, since the place is captured only
# once a tree is on screen; and a folder seeded in the wrong case comes
# back in its real case only if the trail was really replayed. Written
# for the hosted Windows runner (ci.yml), whose fixed drives are C: and
# D:; runs anywhere with a built spindle.exe. A capture of each launch
# lands in -Out for the record.
param(
    [string]$Exe = ".\spindle.exe",
    [string]$Out = ".\remember-shots",
    [string]$Folder = "Windows",     # a folder that exists on C:
    [int]$LoadSeconds = 120
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    public static int[] WindowRect(IntPtr h) { RECT r; GetWindowRect(h, out r); return new int[] { r.L, r.T, r.R, r.B }; }
}
"@
function Log($m) { Write-Host ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $m) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$exePath = (Resolve-Path $Exe).Path
$dir = Join-Path $env:LOCALAPPDATA "Spindle"
$fixed = @(Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -eq 3 } | ForEach-Object { $_.DeviceID.Substring(0, 1) })
Log ("fixed drives: {0}" -f ($fixed -join " "))
$pass = 0; $fail = 0
function Ok($m)  { Write-Host "  PASS: $m"; $script:pass++ }
function Bad($m) { Write-Host "  FAIL: $m"; $script:fail++ }
function Setting($name) {
    $line = Get-Content (Join-Path $dir "settings.txt") -ErrorAction SilentlyContinue |
        Where-Object { $_ -like "$name=*" } | Select-Object -First 1
    if ($line) { return $line.Substring($name.Length + 1) } else { return "" }
}

# Seed, launch, wait for the caches the load writes (the aggregate walks
# each fixed drive and writes its cache as it goes; a single-drive launch
# prefetches the rest), capture, close, wait for the exit.
function RoundTrip($seed, $shot) {
    Get-Process spindle -ErrorAction SilentlyContinue | Stop-Process -Force
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Get-ChildItem $dir -Filter "*.spincache" -ErrorAction SilentlyContinue | Remove-Item -Force
    [IO.File]::WriteAllText((Join-Path $dir "settings.txt"), "remember_view=1`ncheck_updates=0`n$seed")
    $p = Start-Process -FilePath $exePath -PassThru
    $deadline = (Get-Date).AddSeconds($LoadSeconds)
    while ((Get-Date) -lt $deadline) {
        $missing = @($fixed | Where-Object { -not (Test-Path (Join-Path $dir "$_.spincache")) })
        if ($missing.Count -eq 0) { break }
        Start-Sleep -Seconds 2
    }
    Log ("caches present: {0}" -f ((Get-ChildItem $dir -Filter "*.spincache" | ForEach-Object { $_.Name }) -join " "))
    Start-Sleep -Seconds 5
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
        try {
            $r = [Win]::WindowRect($p.MainWindowHandle)
            $bmp = New-Object System.Drawing.Bitmap ($r[2] - $r[0]), ($r[3] - $r[1])
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($r[0], $r[1], 0, 0, $bmp.Size); $g.Dispose()
            $bmp.Save((Join-Path $Out "$shot.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        } catch { Log "no capture: $_" }
    } else { Log "no main window handle" }
    if (-not $p.CloseMainWindow()) { Log "close request was not delivered" }
    if (-not $p.WaitForExit(60000)) { Log "did not exit on close; killing"; $p.Kill() }
}
function Report($wantFlag, $wantPath, $label) {
    $f = Setting "last_all_drives"; $p = Setting "last_path"
    if ($f -eq $wantFlag -and $p -eq $wantPath) { Ok $label } else { Bad "${label}: got last_all_drives=$f last_path=$p" }
}

$real = "C:\" + (Get-Item "C:\$Folder").Name      # the folder's own casing
$wrong = "c:\" + $Folder.ToLower()

Write-Host "1) All drives at its root comes back as All drives"
RoundTrip "last_all_drives=1`n" "remember_root"
Report "1" "All drives" "last_all_drives=1, last_path=All drives"

Write-Host "2) a folder inside All drives comes back inside All drives, in its real case"
RoundTrip "last_all_drives=1`nlast_path=$wrong`n" "remember_folder"
Report "1" $real "last_all_drives=1, last_path=$real"

Write-Host "3) a folder on one drive still comes back on that drive alone"
RoundTrip "last_path=$wrong`n" "remember_drive"
Report "0" $real "last_all_drives=0, last_path=$real"

Get-Process spindle -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "== $pass passed, $fail failed =="
if ($fail -ne 0) { exit 1 }
