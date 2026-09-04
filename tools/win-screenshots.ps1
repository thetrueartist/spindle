# Capture the README images on a real Windows desktop.
#
# Runs the built spindle.exe against a demo volume it creates itself (a
# VHDX attached through diskpart, formatted NTFS, filled with a synthetic
# tree of games, videos, photos, projects and a few real duplicate sets),
# drives the window with the same clicks and keys a person would use, and
# saves each scene as a PNG under -Out. Nothing here beyond Windows and
# .NET: the GitHub-hosted Windows runner has both, and that runner is
# where this is meant to run (see .github/workflows/screenshots.yml).
# Wine renders the program with its own chrome and fonts; only a Windows
# desktop shows what a user sees.
#
# Positions are client coordinates, derived from the layout constants in
# src/ui.cpp (the sidebar is 268 wide, a drive card is 66 high with an
# 8 px gap, the panel tabs follow the cards) so they hold for any number
# of drives. Every scene also saves a full-window capture, which is how
# a position that drifted gets found.
param(
    [string]$Exe = ".\spindle.exe",
    [string]$Out = ".\shots",
    [string]$Letter = "S",
    [int]$ClientW = 1272,
    [int]$ClientH = 766
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
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields;
        public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2;
        public int dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettingsW(ref DEVMODE dm, int flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowW")] public static extern IntPtr FindWindowByTitle(IntPtr cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);

    // {width, height} of the current display mode.
    public static int[] CurrentMode() {
        DEVMODE dm = new DEVMODE();
        dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        if (!EnumDisplaySettingsW(null, -1, ref dm)) return new int[] { 0, 0 };
        return new int[] { dm.dmPelsWidth, dm.dmPelsHeight };
    }
    // Asks for a mode; 0 means the display took it.
    public static int SetMode(int width, int height) {
        DEVMODE dm = new DEVMODE();
        dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        EnumDisplaySettingsW(null, -1, ref dm);
        dm.dmPelsWidth = width;
        dm.dmPelsHeight = height;
        dm.dmFields = 0x80000 | 0x100000;   // DM_PELSWIDTH | DM_PELSHEIGHT
        return ChangeDisplaySettingsW(ref dm, 0);
    }
    // {left, top, right, bottom} of the window frame on screen.
    public static int[] WindowRect(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        return new int[] { r.L, r.T, r.R, r.B };
    }
    // {width, height} of the client area.
    public static int[] ClientSize(IntPtr h) {
        RECT r; GetClientRect(h, out r);
        return new int[] { r.R - r.L, r.B - r.T };
    }
    // {x, y} on screen of the client area's top-left corner.
    public static int[] ClientOrigin(IntPtr h) {
        POINT p; p.X = 0; p.Y = 0;
        ClientToScreen(h, ref p);
        return new int[] { p.X, p.Y };
    }
}
"@

function Log($m) { Write-Host ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $m) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

# ---------------------------------------------------------------- display
# The hosted runner boots at 1024x768, too small for the window the README
# shows. Ask for something larger; the synthetic display usually obliges,
# and if it does not the window is shrunk to fit further down.
$mode = [Native]::CurrentMode()
Log ("display now {0}x{1}" -f $mode[0], $mode[1])
foreach ($want in @(@(1600, 1000), @(1600, 900), @(1440, 900), @(1920, 1080))) {
    if ($mode[0] -ge 1300 -and $mode[1] -ge 830) { break }
    $r = [Native]::SetMode($want[0], $want[1])
    Log ("ChangeDisplaySettings {0}x{1} -> {2}" -f $want[0], $want[1], $r)
    if ($r -eq 0) { Start-Sleep -Seconds 2; $mode = [Native]::CurrentMode() }
}
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
Log ("screen {0}x{1}" -f $screen.Width, $screen.Height)

# ------------------------------------------------------------ demo volume
$vhd = Join-Path $env:TEMP "spindle-demo.vhdx"
$root = "${Letter}:\"
if (-not (Test-Path $root)) {
    $dp = Join-Path $env:TEMP "spindle-demo.dp"
    @(
        "create vdisk file=`"$vhd`" maximum=40960 type=expandable",
        "attach vdisk",
        "create partition primary",
        "format fs=ntfs quick label=Demo",
        "assign letter=$Letter"
    ) | Set-Content -Path $dp -Encoding ASCII
    Log "creating the demo volume"
    & diskpart /s $dp | Out-String | Write-Host
    for ($i = 0; $i -lt 30 -and -not (Test-Path $root); $i++) { Start-Sleep -Seconds 1 }
    if (-not (Test-Path $root)) { throw "demo volume never appeared at $root" }
}

# fsutil allocates the file without writing it, so a multi-gigabyte tree
# costs seconds and the expandable disk stays small. Every size is unique (a counter times a prime on top of
# the requested size) so that same-length zero-filled files never pose as
# duplicates; the only duplicates on the volume are the real ones below.
$script:n = 0
function Blob($rel, [long]$bytes) {
    $p = Join-Path $root $rel
    $d = Split-Path $p
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
    $script:n++
    $size = $bytes + ($script:n * 7919)
    & fsutil file createnew $p $size | Out-Null
}
function Random-File($rel, [int]$bytes) {
    $p = Join-Path $root $rel
    $d = Split-Path $p
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
    $buf = New-Object byte[] $bytes
    (New-Object System.Random 1234).NextBytes($buf)
    [IO.File]::WriteAllBytes($p, $buf)
}
$MB = 1MB; $GB = 1GB
if (-not (Test-Path (Join-Path $root "Games"))) {
    Log "filling the demo volume"
    # Games: the block a treemap is for.
    Blob "Games\Northwind Online\Content\Paks\pakchunk0-WindowsNoEditor.pak" (1.55 * $GB)
    Blob "Games\Northwind Online\Content\Paks\pakchunk1-WindowsNoEditor.pak" (760 * $MB)
    Blob "Games\Northwind Online\Content\Paks\pakchunk2-WindowsNoEditor.pak" (410 * $MB)
    Blob "Games\Northwind Online\Content\Paks\pakchunk3-WindowsNoEditor.pak" (230 * $MB)
    Blob "Games\Northwind Online\Content\Movies\intro.bk2" (180 * $MB)
    Blob "Games\Northwind Online\Content\Movies\credits.bk2" (95 * $MB)
    Blob "Games\Northwind Online\Engine\Binaries\Win64\Northwind-Win64-Shipping.exe" (142 * $MB)
    foreach ($d in "PhysX3_x64","APEX_x64","OpenAL32","steam_api64","D3DCompiler_47") { Blob "Games\Northwind Online\Engine\Binaries\Win64\$d.dll" (3 * $MB) }
    Blob "Games\Riverlands\data\world.big" (620 * $MB)
    Blob "Games\Riverlands\data\audio.big" (240 * $MB)
    Blob "Games\Riverlands\data\textures.big" (510 * $MB)
    Blob "Games\Riverlands\Riverlands.exe" (48 * $MB)
    foreach ($i in 1..12) { Blob ("Games\Riverlands\saves\slot{0:d2}.sav" -f $i) (2 * $MB) }
    # Videos and photos.
    foreach ($i in 1..8) { Blob ("Videos\Holiday 2025\clip-{0:d2}.mp4" -f $i) ((120 + 40 * $i) * $MB) }
    foreach ($i in 1..4) { Blob ("Videos\Screen recordings\recording-{0}.mkv" -f $i) ((300 + 90 * $i) * $MB) }
    foreach ($i in 1..30) { Blob ("Photos\2024\Iceland\DSC_{0:d4}.NEF" -f $i) (24 * $MB) }
    foreach ($i in 1..30) { Blob ("Photos\2024\Iceland\DSC_{0:d4}.jpg" -f $i) (4 * $MB) }
    foreach ($i in 1..18) { Blob ("Photos\2025\Garden\IMG_{0:d4}.heic" -f $i) (3 * $MB) }
    # Virtual machines and disk images.
    Blob "VMs\dev-box\dev-box.vmdk" (2.3 * $GB)
    Blob "VMs\dev-box\dev-box.nvram" (8 * 1KB)
    Blob "VMs\win11-test\win11-test.vhdx" (1.4 * $GB)
    Blob "Downloads\ubuntu-24.04-desktop-amd64.iso" (690 * $MB)
    # Downloads, backups, music, and a project with many small files.
    Blob "Downloads\node-v22.11.0-win-x64.zip" (46 * $MB)
    Blob "Downloads\Setup-Blender-4.2.msi" (310 * $MB)
    Blob "Downloads\fonts-2024.7z" (12 * $MB)
    foreach ($i in 1..3) { Blob ("Backups\laptop-2025-0{0}.7z" -f $i) ((150 + 30 * $i) * $MB) }
    Blob "Backups\mail-archive.pst" (410 * $MB)
    foreach ($a in "Aurora","Brass Tacks","Coastline") { foreach ($t in 1..9) { Blob ("Music\$a\{0:d2} - track.flac" -f $t) ((28 + $t) * $MB) } }
    foreach ($f in "core.cpp","scan.cpp","ui.cpp","mft.cpp","ntfs.cpp","spindle.h","sync.h","workqueue.h") { Blob "Projects\spindle\src\$f" (40 * 1KB) }
    foreach ($i in 1..120) { Blob ("Projects\web-app\node_modules\pkg-{0:d3}\index.js" -f $i) (9 * 1KB); Blob ("Projects\web-app\node_modules\pkg-{0:d3}\package.json" -f $i) (1 * 1KB) }
    foreach ($i in 1..40) { Blob ("Projects\web-app\src\components\Widget{0}.tsx" -f $i) (6 * 1KB) }
    Blob "Projects\web-app\dist\bundle.js" (5 * $MB)
    Blob "Projects\data-pipeline\warehouse.sqlite" (860 * $MB)
    foreach ($i in 1..6) { Blob ("Projects\data-pipeline\exports\run-{0}.csv" -f $i) ((40 + 15 * $i) * $MB) }
    # Real duplicates: the same bytes in several places.
    Random-File "Photos\2024\Iceland\DSC_0102-edit.jpg" (4200 * 1KB)
    New-Item -ItemType Directory -Force -Path (Join-Path $root "Backups\photos-2024") | Out-Null
    Copy-Item (Join-Path $root "Photos\2024\Iceland\DSC_0102-edit.jpg") (Join-Path $root "Backups\photos-2024\DSC_0102-edit.jpg") -Force
    New-Item -ItemType Directory -Force -Path (Join-Path $root "Downloads") | Out-Null
    Copy-Item (Join-Path $root "Photos\2024\Iceland\DSC_0102-edit.jpg") (Join-Path $root "Downloads\DSC_0102-edit (1).jpg") -Force
    Random-File "Videos\Holiday 2025\clip-09.mp4" (9 * $MB)
    New-Item -ItemType Directory -Force -Path (Join-Path $root "Backups\videos") | Out-Null
    Copy-Item (Join-Path $root "Videos\Holiday 2025\clip-09.mp4") (Join-Path $root "Backups\videos\clip-09.mp4") -Force
    Random-File "Downloads\driver-setup.zip" (6 * $MB)
    Copy-Item (Join-Path $root "Downloads\driver-setup.zip") (Join-Path $root "Downloads\driver-setup (1).zip") -Force
    Log ("demo volume filled: {0} files" -f (Get-ChildItem $root -Recurse -File).Count)
}

function ShotScreen($name) {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# ------------------------------------------------------------- the window
$exePath = (Resolve-Path $Exe).Path
Log "launching $exePath $root"
$proc = Start-Process -FilePath $exePath -ArgumentList $root -PassThru
try {
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 500
    $hwnd = [Native]::FindWindowByTitle([IntPtr]::Zero, "Spindle")
}
if ($hwnd -eq [IntPtr]::Zero) { throw "the window never appeared" }
Start-Sleep -Seconds 2

# Size the client area exactly, so every position below and every crop
# matches the layout the README was drawn for.
$wr = [Native]::WindowRect($hwnd); $cs = [Native]::ClientSize($hwnd)
$frameW = ($wr[2] - $wr[0]) - $cs[0]; $frameH = ($wr[3] - $wr[1]) - $cs[1]
Log ("window {0}x{1}, client {2}x{3}, frame {4}+{5}" -f ($wr[2] - $wr[0]), ($wr[3] - $wr[1]), $cs[0], $cs[1], $frameW, $frameH)
$W = [Math]::Min($ClientW, $screen.Width - $frameW - 16)
$H = [Math]::Min($ClientH, $screen.Height - $frameH - 16)
[void][Native]::SetWindowPos($hwnd, [IntPtr]::Zero, 8, 8, $W + $frameW, $H + $frameH, 0x0040)
Start-Sleep -Milliseconds 800
[void][Native]::SetForegroundWindow($hwnd)
$cs = [Native]::ClientSize($hwnd); $o = [Native]::ClientOrigin($hwnd)
$W = $cs[0]; $H = $cs[1]
$origin = New-Object PSObject -Property @{ X = $o[0]; Y = $o[1] }
if ($W -le 0 -or $H -le 0) { throw "client area is ${W}x${H}" }
Log ("client {0}x{1} at screen {2},{3}" -f $W, $H, $origin.X, $origin.Y)

# Variable names are case-insensitive in PowerShell, so these parameters
# must not be spelled like the client size they default to.
function Shot($name, $left = 0, $top = 0, $width = 0, $height = 0) {
    if ($width -le 0) { $width = $W - $left }
    if ($height -le 0) { $height = $H - $top }
    $bmp = New-Object System.Drawing.Bitmap $width, $height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($origin.X + $left, $origin.Y + $top, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
function ShotWindow($name) {
    # The whole window including its frame, for the record.
    $r = [Native]::WindowRect($hwnd)
    $bmp = New-Object System.Drawing.Bitmap ($r[2] - $r[0]), ($r[3] - $r[1])
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r[0], $r[1], 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
function Click($x, $y, $right = $false) {
    [void][Native]::SetCursorPos($origin.X + $x, $origin.Y + $y)
    Start-Sleep -Milliseconds 120
    if ($right) { [Native]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero); [Native]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero) }
    else        { [Native]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero); [Native]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) }
    Start-Sleep -Milliseconds 250
}
function Hover($x, $y) { [void][Native]::SetCursorPos($origin.X + $x, $origin.Y + $y); Start-Sleep -Milliseconds 300 }
function Keys($k) { [void][Native]::SetForegroundWindow($hwnd); [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 250 }
function TypeText($text) {
    foreach ($ch in $text.ToCharArray()) {
        $s = [string]$ch
        if ("+^%~(){}[]".Contains($s)) { $s = "{" + $s + "}" }
        [System.Windows.Forms.SendKeys]::SendWait($s)
        Start-Sleep -Milliseconds 70
    }
}
$script:frame = 0
function Frames($count, $everyMs = 125) {
    for ($i = 0; $i -lt $count; $i++) {
        Shot ("frame_{0:d3}" -f $script:frame)
        $script:frame++
        Start-Sleep -Milliseconds $everyMs
    }
}

# Sidebar geometry, from src/ui.cpp: title row at 16, All drives card at
# 102, the first drive card at 144, 74 per card, panel tabs 10 below the
# last card.
$drives = @(Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -eq 3 } | Sort-Object DeviceID)
$demoIndex = [array]::IndexOf(@($drives | ForEach-Object { $_.DeviceID }), "${Letter}:")
$cardY = 144 + 74 * $demoIndex + 33
$tabsY = 144 + 74 * $drives.Count + 10
Log ("{0} fixed drives, demo card at y={1}, panel tabs at y={2}" -f $drives.Count, $cardY, $tabsY)
function Tab($i) { Click (16 + 59 * $i + 29) ($tabsY + 12) }

# --- 1. the map, after the scan (the demo card is clicked so the sidebar
#        shows it selected even if the launch path already opened it)
Start-Sleep -Seconds 4
Click 134 $cardY
Start-Sleep -Seconds 4
Hover 700 500
Shot "screenshot"
ShotWindow "window_screenshot"

# --- 2. the walkthrough: rescan, drill in, come back, search
Keys "{F5}"
Frames 10 100
Start-Sleep -Milliseconds 800
Frames 6
Click 420 220           # the largest block, top-left of the map
Frames 10
Click 420 220           # one level further
Frames 8
Keys "{BACKSPACE}"; Frames 4
Keys "{BACKSPACE}"; Frames 6
Keys "^f"; Frames 3
TypeText "kind:media >100mb"; Frames 12
Keys "{ESC}"

# --- 3. the list. Map and List are the two 46-wide buttons at the right
#        end of the breadcrumb row (src/ui.cpp DrawBreadcrumb).
Keys "{ESC}"; Start-Sleep -Milliseconds 300
Click ($W - 10 - 23) 19
Start-Sleep -Seconds 1
Hover 700 200
Shot "browse" 262 0 1010 420
ShotWindow "window_browse"
Click ($W - 46 - 14 - 23) 19     # back to the map
Start-Sleep -Milliseconds 600

# --- 4. the address bar with Tab completion
Keys "^l"; Start-Sleep -Milliseconds 400
Keys "^a"; TypeText "${Letter}:\Ga"; Keys "{TAB}"; Start-Sleep -Milliseconds 600
Shot "addressbar" 262 0 1000 140
ShotWindow "window_addressbar"
Keys "{ESC}"

# --- 5. Find. The walkthrough left its query in the box; select it all
#        first so the new one replaces it.
Tab 2
Keys "^f"; Keys "^a"; TypeText "kind:media >300mb"; Start-Sleep -Seconds 1
Shot "find" 0 $tabsY 262 246
ShotWindow "window_find"
Keys "{ESC}"

# --- 6. duplicates
Tab 3; Start-Sleep -Milliseconds 500
ShotWindow "window_dupes_before"
Click 134 ($tabsY + 24 + 12 + 13)     # "Find duplicates here"
Start-Sleep -Seconds 6
Shot "duplicates" 0 $tabsY 262 300
ShotWindow "window_dupes"

# --- 7. tabs: open a folder in its own tab. The right-click lands on a
#        folder's header strip (the "Games" label row at the top left of
#        the map), because only a folder's menu carries "Open in a new
#        tab" as its third item; on a file the third item would be the
#        recycle prompt.
Tab 0
Keys "{ESC}"; Start-Sleep -Milliseconds 300
Click 330 48 $true; Start-Sleep -Milliseconds 600
Keys "{DOWN}{DOWN}{DOWN}{ENTER}"; Start-Sleep -Seconds 2
Shot "tabs" 262 0 780 62
ShotWindow "window_tabs"

# --- 8. the question a network path asks. Anything modal left over would
#        take the keys instead, so clear the decks first.
Keys "{ESC}"; Start-Sleep -Milliseconds 400
Keys "^l"; Start-Sleep -Milliseconds 400
Keys "^a"; TypeText "\\nas\share"; Keys "{ENTER}"
$dlg = [IntPtr]::Zero
for ($i = 0; $i -lt 20 -and $dlg -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 300; $dlg = [Native]::FindWindowW("#32770", "Spindle") }
if ($dlg -ne [IntPtr]::Zero) {
    $dr = [Native]::WindowRect($dlg)
    $bmp = New-Object System.Drawing.Bitmap ($dr[2] - $dr[0]), ($dr[3] - $dr[1])
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($dr[0], $dr[1], 0, 0, $bmp.Size); $g.Dispose()
    $bmp.Save((Join-Path $Out "network.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Keys "{ESC}"
} else { Log "no network dialog appeared" }
} catch {
    Log "failed: $_"
    try { ShotScreen "screen_failure" } catch { Log "no screen capture: $_" }
    throw
} finally {
    try { ShotScreen "screen_end" } catch { }
    Log "done: $((Get-ChildItem $Out).Count) files in $Out"
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}
