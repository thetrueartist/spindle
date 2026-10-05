# Where a right-click opens, checked on a real Windows desktop.
#
# "Open in a new tab" promises the folder that was right-clicked, and
# getting there can depend on what the program is doing at that moment.
# Each case is the gesture a person makes, made in one of those moments:
# with nothing else going on, inside All drives (from the map and from
# the Largest list), on switching back to a tab while another drive is
# still being read, and while a cache older than five minutes is being
# revalidated behind the map. A drive clicked while All drives is still
# being gathered is checked too: the stop it asks for used to leave the
# window waiting for good. Where a tab landed is read from what the
# program writes back on close ("Remember where I was" records the
# active tab), not from pixels. "Show in Explorer" is checked the same
# way against the Explorer window it opens: a folder opens as itself, a
# file opens its folder (what Explorer then selects is reported).
#
# A click only means something when the layout is known, so the script
# builds its own volume the way the README captures do: a VHDX attached
# through diskpart and formatted NTFS, where Alpha is the largest folder
# at the root and Beta the largest inside Alpha, so each sits top-left
# under the click. A small tree on C: does the same for the revalidation
# case, which needs a drive whose walk takes long enough to act during.
# Written for the hosted Windows runner (ci.yml); runs anywhere with a
# built spindle.exe and the right to attach a virtual disk. A capture of
# each case lands in -Out.
param(
    [string]$Exe = ".\spindle.exe",
    [string]$Out = ".\newtab-shots",
    [string]$Letter = "R"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
// PowerShell cannot take a struct back from an out or ref parameter, so
// every call that fills one lives here and hands back plain numbers.
public static class Native {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public static int[] WindowRect(IntPtr h) { RECT r; GetWindowRect(h, out r); return new int[] { r.L, r.T, r.R, r.B }; }
    public static int[] ClientOrigin(IntPtr h) { POINT p; p.X = 0; p.Y = 0; ClientToScreen(h, ref p); return new int[] { p.X, p.Y }; }
}
"@

function Log($m) { Write-Host ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $m) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$exePath = (Resolve-Path $Exe).Path
$dir = Join-Path $env:LOCALAPPDATA "Spindle"
$pass = 0; $fail = 0; $moot = 0
function Ok($m)   { Write-Host "  PASS: $m"; $script:pass++ }
function Bad($m)  { Write-Host "  FAIL: $m"; $script:fail++ }
function Moot($m) { Write-Host "  NOT JUDGED: $m"; $script:moot++ }
function Info($m) { Write-Host "  SEEN: $m" }
function Setting($name) {
    $line = Get-Content (Join-Path $dir "settings.txt") -ErrorAction SilentlyContinue |
        Where-Object { $_ -like "$name=*" } | Select-Object -First 1
    if ($line) { return $line.Substring($name.Length + 1) } else { return "" }
}
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
Log ("screen {0}x{1}" -f $screen.Width, $screen.Height)

# ----------------------------------------------------------------- volume
$R = "${Letter}:\"
if (-not (Test-Path $R)) {
    $vhd = Join-Path $env:TEMP "spindle-newtab.vhdx"
    $dp = Join-Path $env:TEMP "spindle-newtab.dp"
    @(
        "create vdisk file=`"$vhd`" maximum=65536 type=expandable",
        "attach vdisk",
        "create partition primary",
        "format fs=ntfs quick label=NewTab",
        "assign letter=$Letter"
    ) | Set-Content -Path $dp -Encoding ASCII
    Log "creating the test volume"
    & diskpart /s $dp | Out-String | Write-Host
    for ($i = 0; $i -lt 30 -and -not (Test-Path $R); $i++) { Start-Sleep -Seconds 1 }
    if (-not (Test-Path $R)) { throw "the test volume never appeared at $R" }
}
# fsutil sets the length without writing the bytes, so gigabytes cost
# nothing and the expandable disk stays small. One file is made larger
# than anything the runner ships with, so it heads All drives' Largest
# list and the list cases know which row they are clicking.
function Blob($path, [long]$bytes) {
    $d = Split-Path $path
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
    if (-not (Test-Path $path)) { & fsutil file createnew $path $bytes | Out-Null }
}
$MB = 1MB
Blob "${R}Alpha\Beta\one.bin" (40000 * $MB)   # the largest file on the machine
Blob "${R}Alpha\Beta\two.bin" (2000 * $MB)
Blob "${R}Alpha\Gamma\three.bin" (1000 * $MB)
Blob "${R}Other\four.bin" (800 * $MB)
Blob "${R}Small\five.bin" (200 * $MB)
$cTree = "C:\spindle-newtab"
Blob "$cTree\Alpha\one.bin" (300 * $MB)
Blob "$cTree\Alpha\two.bin" (150 * $MB)
Blob "$cTree\Other\three.bin" (50 * $MB)

# Sidebar geometry, from src/ui.cpp: the first drive card at 144, 74 per
# card, the panel tabs 10 below the last card and 59 wide from x=16, the
# list 30 below them in rows 34 apart. The map starts at x=268, under a
# 40-high breadcrumb, and a 30-high tab strip once there are two tabs; a
# top-level folder's header strip is the first 16 rows inside a 3 px inset.
$drives = @(Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -eq 3 } |
    Sort-Object DeviceID | ForEach-Object { $_.DeviceID.Substring(0, 1) })
Log ("fixed drives: {0}" -f ($drives -join " "))
$panelY = 144 + 74 * $drives.Count + 10
$rowY = $panelY + 30 + 16
function HeaderY($tabs) { if ($tabs -ge 2) { return 78 } else { return 48 } }

# ----------------------------------------------------------------- window
$script:proc = $null
$script:hwnd = [IntPtr]::Zero
$script:origin = $null
# Prefetch off, so nothing walks a drive unless a case asks it to and the
# cache times below mean what each case says they mean.
function Seed($extra) {
    Get-Process spindle -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir "settings.txt"),
        "remember_view=1`ncheck_updates=0`nprefetch_all=0`n$extra")
}
function Launch($target) {
    if ($target) {
        $script:proc = Start-Process -FilePath $exePath -ArgumentList $target -PassThru
    } else {
        $script:proc = Start-Process -FilePath $exePath -PassThru
    }
    $script:hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 80 -and $script:hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
        $script:proc.Refresh()
        $script:hwnd = $script:proc.MainWindowHandle
    }
    if ($script:hwnd -eq [IntPtr]::Zero) { throw "the window never appeared" }
    # Small enough for the runner's desktop, at its top left, so every
    # click below lands on the window and not off the screen.
    [void][Native]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 0, 0, 1000, 740, 0x0040)
    Start-Sleep -Milliseconds 400
    [void][Native]::SetForegroundWindow($script:hwnd)
    $o = [Native]::ClientOrigin($script:hwnd)
    $script:origin = @{ X = $o[0]; Y = $o[1] }
}
function Click($x, $y, [switch]$Right) {
    [void][Native]::SetCursorPos($script:origin.X + $x, $script:origin.Y + $y)
    Start-Sleep -Milliseconds 150
    if ($Right) {
        [Native]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)
        [Native]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
    } else {
        [Native]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        [Native]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 300
}
function Keys($k) {
    [void][Native]::SetForegroundWindow($script:hwnd)
    [System.Windows.Forms.SendKeys]::SendWait($k)
    Start-Sleep -Milliseconds 250
}
# Right-click, then the item `downs` places down the menu. A folder on
# the map offers Show in Explorer, Copy path, Open in a new tab; a row
# in the Largest list offers Open in a new tab, Show on the map, Show in
# Explorer, Copy path.
function Menu($x, $y, [int]$downs) {
    Click $x $y -Right
    Start-Sleep -Milliseconds 500
    Keys (("{DOWN}" * $downs) + "{ENTER}")
}
function Shot($name) {
    try {
        $r = [Native]::WindowRect($script:hwnd)
        $bmp = New-Object System.Drawing.Bitmap ($r[2] - $r[0]), ($r[3] - $r[1])
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r[0], $r[1], 0, 0, $bmp.Size); $g.Dispose()
        $bmp.Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    } catch { Log "no capture: $_" }
}
# The map is drawn once the point at (600, y) - inside the top-left
# folder's header strip, clear of its label - is no longer the bare
# background the window clears to (0x12161C) while a tree is loading.
function WaitDrawn($y, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $bmp = New-Object System.Drawing.Bitmap 1, 1
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($script:origin.X + 600, $script:origin.Y + $y, 0, 0, $bmp.Size); $g.Dispose()
        $c = $bmp.GetPixel(0, 0); $bmp.Dispose()
        if ([Math]::Abs($c.R - 0x12) + [Math]::Abs($c.G - 0x16) + [Math]::Abs($c.B - 0x1C) -gt 40) { return $true }
        Start-Sleep -Milliseconds 200
    }
    Log "the map was not drawn within $seconds s"
    return $false
}
function CloseAndRead() {
    Get-ExplorerWindows | ForEach-Object { try { $_.Quit() } catch { } }
    if (-not $script:proc.CloseMainWindow()) { Log "close request was not delivered" }
    if (-not $script:proc.WaitForExit(60000)) { Log "did not exit on close; killing"; $script:proc.Kill() }
    return @{ Flag = (Setting "last_all_drives"); Path = (Setting "last_path") }
}
function Expect($got, $flag, $path, $label) {
    if ($got.Flag -eq $flag -and $got.Path -eq $path) { Ok $label }
    else { Bad ("{0}: landed on last_all_drives={1} last_path={2}" -f $label, $got.Flag, $got.Path) }
}
function CacheTime($l) {
    $f = Join-Path $dir "$l.spincache"
    if (Test-Path $f) { return (Get-Item $f).LastWriteTimeUtc }
    return [DateTime]::MinValue
}
function WaitCacheAfter($l, [DateTime]$after, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ((CacheTime $l) -gt $after) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
function Get-ExplorerWindows {
    $list = @()
    foreach ($w in (New-Object -ComObject Shell.Application).Windows()) {
        try { if ($w.Document -and $w.Document.Folder) { $list += $w } } catch { }
    }
    return $list
}
# What Explorer shows after a "Show in Explorer": the folder it opened,
# and what it has selected there, which can arrive a moment after the
# window does, so it is given a few seconds to.
function ExplorerLanding() {
    $where = ""; $sel = ""; $settle = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        foreach ($w in Get-ExplorerWindows) {
            try {
                $p = $w.Document.Folder.Self.Path
                if ($p) {
                    $where = $p
                    $sel = @($w.Document.SelectedItems() | ForEach-Object { $_.Name }) -join ", "
                }
            } catch { }
        }
        if ($where) {
            if ($sel) { break }
            if (-not $settle) { $settle = (Get-Date).AddSeconds(6) }
            elseif ((Get-Date) -gt $settle) { break }
        }
    }
    return @{ Where = $where; Selected = $sel }
}
function Flatten($p) { return ($p -replace '(?<=.)\\{2,}', '\') }

try {
Get-ExplorerWindows | ForEach-Object { try { $_.Quit() } catch { } }
Get-ChildItem $dir -Filter "*.spincache" -ErrorAction SilentlyContinue | Remove-Item -Force

Write-Host "1) a folder opened in a new tab, with nothing else going on, opens on that folder"
Seed ""
Launch $R
$drawn = WaitDrawn 44 120
Start-Sleep -Seconds 1
Menu 330 (HeaderY 1) 3
Start-Sleep -Seconds 3
Shot "1_at_rest"
$got = CloseAndRead
if (-not $drawn) { Moot "the map never came up" }
else { Expect $got "0" "${R}Alpha" "the new tab is on ${R}Alpha" }

Write-Host "2) a drive opened in a new tab from All drives opens on that drive, inside All drives"
Seed "last_all_drives=1`n"
Launch $null
$drawn = WaitDrawn 44 240
Start-Sleep -Seconds 2
Shot "2_all_drives_before"
Menu 330 (HeaderY 1) 3
Start-Sleep -Seconds 15      # time enough for a rebuild to land, if one is what happens
Shot "2_all_drives_after"
$got = CloseAndRead
if (-not $drawn) { Moot "All drives never came up" }
elseif ($got.Flag -eq "1" -and $got.Path -match '^[A-Z]:\\$') { Ok "the new tab is on $($got.Path), inside All drives" }
else { Bad ("the new tab landed on last_all_drives={0} last_path={1}" -f $got.Flag, $got.Path) }

Write-Host "3) a file's row in All drives' Largest list opens a new tab on its folder"
Seed "last_all_drives=1`n"
Launch $null
[void](WaitDrawn 44 120)
Start-Sleep -Seconds 1
Click (16 + 59 + 29) ($panelY + 12)          # the Largest tab
Start-Sleep -Seconds 3
Set-Clipboard -Value " "
Menu 134 $rowY 4                             # Copy path
Start-Sleep -Milliseconds 500
$raw = [string](Get-Clipboard)
$file = Flatten $raw
Log "the first row is $raw"
if ($raw -match '^[A-Z]:\\[^\\]') { Ok "Copy path gave $raw, one separator after the drive" }
elseif ($raw -match '^[A-Z]:\\') { Bad "Copy path gave $raw" }
Menu 134 $rowY 1                             # Open in a new tab
Start-Sleep -Seconds 15
Shot "3_largest_row"
$got = CloseAndRead
$want = Split-Path -Parent $file
if (-not $want -or $file -notmatch '^[A-Z]:\\') { Moot "no path came off the first row ('$file')" }
else { Expect $got "1" $want "the new tab is on $want, inside All drives" }

Write-Host "4) Show in Explorer on a file's row in All drives' Largest list opens the file's folder"
Seed "last_all_drives=1`n"
Launch $null
[void](WaitDrawn 44 120)
Start-Sleep -Seconds 1
Click (16 + 59 + 29) ($panelY + 12)
Start-Sleep -Seconds 3
Menu 134 $rowY 3                             # Show in Explorer
$e = ExplorerLanding
[void](CloseAndRead)
$want = Split-Path -Parent $file
Info ("{0} -> Explorer on '{1}' with [{2}] selected" -f $file, $e.Where, $e.Selected)
if (-not $want -or $file -notmatch '^[A-Z]:\\') { Moot "no path came off the first row ('$file')" }
elseif ($e.Where -eq $want) { Ok "Explorer opened $want, the file's folder" }
else { Bad "Explorer opened '$($e.Where)', not $want" }

Write-Host "5) Show in Explorer on a folder on the map opens that folder, not its parent"
Seed ""
Launch $R
[void](WaitDrawn 44 60)
Start-Sleep -Seconds 1
Menu 330 (HeaderY 1) 1                       # Show in Explorer
$e = ExplorerLanding
[void](CloseAndRead)
Info ("${R}Alpha -> Explorer on '{0}' with [{1}] selected" -f $e.Where, $e.Selected)
if ($e.Where -eq "${R}Alpha") { Ok "Explorer opened ${R}Alpha itself" }
else { Bad "Explorer opened '$($e.Where)', not ${R}Alpha" }

Write-Host "6) a tab switched back to while another drive is being read reopens on its folder"
Seed ""
Launch $R                                    # minutes-old cache: no walk
[void](WaitDrawn 44 60)
Start-Sleep -Seconds 1
Click 330 (HeaderY 1)                        # into Alpha
Start-Sleep -Seconds 1
Menu 330 (HeaderY 1) 3                       # Beta in a second tab; the first keeps Alpha
Start-Sleep -Seconds 2
$cCard = 144 + 74 * [array]::IndexOf($drives, "C") + 33
Click 134 $cCard                             # the second tab looks at C:
[void](WaitDrawn (HeaderY 2) 60)
Start-Sleep -Seconds 1
$t = [DateTime]::UtcNow
Keys "{F5}"                                  # a real walk of C:, which takes a while
Start-Sleep -Milliseconds 800
Click 314 15                                 # the first tab, while C: is still being read
$inTime = (CacheTime "C") -le $t
Start-Sleep -Seconds 15
Shot "6_switch_back"
$got = CloseAndRead
if (-not $inTime) { Moot "C: finished its walk before the switch, so nothing was being read" }
else { Expect $got "0" "${R}Alpha" "the first tab came back on ${R}Alpha" }

# The rest needs caches older than the five minutes inside which one is
# served without a revalidating walk.
$newest = @((CacheTime $Letter), (CacheTime "C")) | Sort-Object | Select-Object -Last 1
$wait = ($newest.AddSeconds(320) - [DateTime]::UtcNow).TotalSeconds
if ($wait -gt 0) {
    Log ("waiting {0:n0} s for the caches to go stale" -f $wait)
    Start-Sleep -Seconds ([int][Math]::Ceiling($wait))
}

Write-Host "7) a folder opened in a new tab while its drive is revalidated stays open when the walk lands"
Seed "last_path=$cTree`n"
$t = [DateTime]::UtcNow
Launch $null                                 # reopens on $cTree from a stale cache, and walks C:
$drawn = WaitDrawn 44 60
Menu 330 (HeaderY 1) 3                       # Alpha, in a new tab
$opened = [DateTime]::UtcNow
$inTime = (CacheTime "C") -le $t
Shot "7_while_walking"
$landed = WaitCacheAfter "C" $t 240
Log ("tab opened {0:n1} s after launch; the walk of C: landed {1:n1} s after launch" -f `
    ($opened - $t).TotalSeconds, ((CacheTime "C") - $t).TotalSeconds)
Start-Sleep -Seconds 3
Shot "7_after_walk"
$got = CloseAndRead
if (-not $drawn) { Moot "the cached map never came up" }
elseif (-not $inTime) { Moot "the walk of C: had landed before the tab opened" }
elseif (-not $landed) { Moot "the walk of C: never landed" }
else { Expect $got "0" "$cTree\Alpha" "the new tab is still on $cTree\Alpha after the walk" }

Write-Host "8) a remembered folder reopened from a stale cache is still open when the walk lands"
Seed "last_path=${R}Alpha\Beta`n"
$t = [DateTime]::UtcNow
Launch $null
$landed = WaitCacheAfter $Letter $t 120
Start-Sleep -Seconds 3
Shot "8_remembered"
$got = CloseAndRead
if (-not $landed) { Moot "the walk of $R never landed" }
else { Expect $got "0" "${R}Alpha\Beta" "${R}Alpha\Beta is still open" }

Write-Host "9) a drive clicked while All drives is still being gathered opens that drive"
Seed "last_all_drives=1`n"
Remove-Item (Join-Path $dir "C.spincache") -Force -ErrorAction SilentlyContinue
Launch $null                                 # walks C: afresh, which takes a while
Start-Sleep -Milliseconds 1500
$inTime = -not (Test-Path (Join-Path $dir "C.spincache"))
Click 134 (144 + 74 * [array]::IndexOf($drives, "D") + 33)   # the D: card
Start-Sleep -Seconds 15
Shot "9_drive_while_gathering"
$got = CloseAndRead
if (-not $inTime) { Moot "All drives had finished gathering before the click" }
else { Expect $got "0" "D:\" "D: opened once the gathering stopped" }
} catch {
    Log "failed: $_"
    Bad "the script stopped: $_"
} finally {
    Get-Process spindle -ErrorAction SilentlyContinue | Stop-Process -Force
}
Write-Host "== $pass passed, $fail failed, $moot not judged =="
if ($fail -ne 0) { exit 1 }
