#!/usr/bin/env bash
# Build the Wine prefix the acceptance run expects, from nothing.
#
#   tools/wine-prefix.sh [VOLUME-ROOT]     default $HOME/spindle-wine-volumes
#
# Creates the prefix if there is none, then lays out what the checks want:
# D:, an ordinary fixed drive with a tree worth mapping; E:, registered as
# a floppy; Y:, registered as a network drive; and \\nas\share, which Wine
# spells dosdevices/unc/nas/share. The files are sparse, so a volume of
# tens of gigabytes costs nothing and takes no time, and the scanner reads
# the same sizes it would from real files. Safe to run again.
set -eu
ROOT=${1:-$HOME/spindle-wine-volumes}
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine}
export WINEDEBUG=-all
# No Mono or Gecko prompts: nothing here needs either.
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-mscoree,mshtml=}
command -v wine >/dev/null 2>&1 || { echo "wine-prefix: wine not found" >&2; exit 1; }
# Creating a prefix without a display does not finish; on a headless box
# the same virtual display the interface harness uses serves.
: "${DISPLAY:=:99}"
export DISPLAY
if ! xdpyinfo >/dev/null 2>&1; then
  command -v Xvfb >/dev/null 2>&1 || { echo "wine-prefix: no display and no Xvfb" >&2; exit 1; }
  (setsid Xvfb "$DISPLAY" -screen 0 1280x800x24 >/dev/null 2>&1 &)
  sleep 2
fi

if [ ! -d "$WINEPREFIX/drive_c" ]; then
  echo "wine-prefix: creating $WINEPREFIX"
  # Wine creates the prefix directory but not its parents.
  mkdir -p "$WINEPREFIX"
  wineboot -i >/dev/null 2>&1 || { echo "wine-prefix: wineboot failed" >&2; exit 1; }
  wineserver -w
fi

mk() { mkdir -p "$(dirname "$1")"; truncate -s "$2" "$1"; }
D="$ROOT/d"; E="$ROOT/e"; Y="$ROOT/y"; U="$WINEPREFIX/dosdevices/unc/nas/share"

if [ ! -d "$D/Games" ]; then
  mk "$D/Games/Northwind Online/Content/Paks/pakchunk0-WindowsNoEditor.pak" 1500M
  mk "$D/Games/Northwind Online/Content/Paks/pakchunk1-WindowsNoEditor.pak" 760M
  mk "$D/Games/Northwind Online/Engine/Binaries/Win64/Northwind-Win64-Shipping.exe" 142M
  mk "$D/Games/Riverlands/data/world.big" 620M
  mk "$D/Games/Riverlands/data/textures.big" 510M
  for i in $(seq 1 8); do mk "$D/Videos/Holiday 2025/clip-0$i.mp4" $((120 + 40 * i))M; done
  for i in $(seq -w 1 40); do mk "$D/Photos/2024/Iceland/DSC_00$i.NEF" 24M; done
  mk "$D/VMs/dev-box/dev-box.vmdk" 2300M
  mk "$D/Downloads/ubuntu-24.04-desktop-amd64.iso" 690M
  mk "$D/Downloads/Setup-Blender-4.2.msi" 310M
  mk "$D/Backups/mail-archive.pst" 410M
  for a in Aurora "Brass Tacks" Coastline; do for t in $(seq 1 9); do mk "$D/Music/$a/0$t - track.flac" $((28 + t))M; done; done
  for i in $(seq -w 1 120); do mk "$D/Projects/web-app/node_modules/pkg-$i/index.js" 9K; done
  for f in core.cpp scan.cpp ui.cpp mft.cpp ntfs.cpp spindle.h; do mk "$D/Projects/spindle/src/$f" 40K; done
  mk "$D/Projects/data-pipeline/warehouse.sqlite" 860M
  for i in $(seq 1 60); do mk "$D/Docs/Reports 2025/report-$(printf '%02d' "$i").pdf" $((100 + i))K; done
fi
mkdir -p "$E/Photos"; mk "$E/Photos/IMG_0001.jpg" 4M; mk "$E/Photos/IMG_0002.jpg" 5M
mkdir -p "$Y/Finance" "$Y/Shared"; mk "$Y/Finance/ledger.xlsx" 2M; mk "$Y/Shared/handbook.pdf" 9M
mkdir -p "$U/Finance" "$U/deep/er"; mk "$U/Finance/ledger.xlsx" 2M; mk "$U/deep/er/notes.txt" 1K

# A fresh prefix maps Z: to the whole host filesystem, which the launch
# prefetch would then try to walk. Nothing here wants it.
rm -f "$WINEPREFIX/dosdevices/z:"
ln -sfn "$D" "$WINEPREFIX/dosdevices/d:"
ln -sfn "$E" "$WINEPREFIX/dosdevices/e:"
ln -sfn "$Y" "$WINEPREFIX/dosdevices/y:"
wine reg add 'HKLM\Software\Wine\Drives' /v 'E:' /d floppy /f >/dev/null 2>&1
wine reg add 'HKLM\Software\Wine\Drives' /v 'Y:' /d network /f >/dev/null 2>&1
wineserver -w
echo "wine-prefix: ready. D: $D (fixed), E: floppy, Y: network, \\\\nas\\share under $WINEPREFIX"
