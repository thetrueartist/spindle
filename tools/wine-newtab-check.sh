#!/usr/bin/env bash
# Where a right-click opens, under Wine: "Open in a new tab" from a folder
# on the map, from a drive inside All drives, and from a row of All
# drives' Largest list. Proved from what the program writes back on close
# ("Remember where I was" records the active tab), not from pixels. The
# cases that need a scan in flight, or a cache old enough to be
# revalidated, depend on a real disk's timing and live in
# tools/win-newtab-check.ps1, which CI runs on a real Windows desktop.
# Needs the prefix from tools/wine-prefix.sh (D: holds Games, the largest
# folder, and VMs\dev-box\dev-box.vmdk, the largest file) and a built
# build/spindle.exe. A screenshot of each case lands in build/uitest.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=wine-ui-test.sh
source "$HERE/wine-ui-test.sh"
[ -f build/spindle.exe ] || { echo "wine-newtab-check: build/spindle.exe is missing; run make first" >&2; exit 1; }
P="${WINEPREFIX:-$HOME/.wine}"
C="$P/drive_c/users/$(whoami)/AppData/Local/Spindle"
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
stop() { for pid in $(ps -eo pid,cmd | awk '$2 ~ /spindle\.exe$/ {print $1}'); do kill -TERM "$pid" 2>/dev/null; done; }
running() { ps -eo cmd | grep -qE 'spindle\.exe$'; }
setting() { tr -d '\r' < "$C/settings.txt" 2>/dev/null | grep "^$1=" | head -1 | cut -d= -f2-; }

# Layout from src/ui.cpp: a top-level folder's header strip at y=48 under
# the breadcrumb, the first drive card at 144 and 74 per card (this prefix
# shows four: C, D, E and Y), the panel tabs 10 below the last card and 59
# wide from x=16, the list's first row 30 below them.
PANEL_Y=$((144 + 74 * 4 + 10))
ROW_Y=$((PANEL_Y + 30 + 16))

# Seed, launch, wait for the caches the load writes, and settle.
open_view() {   # seed [path]
  stop; sleep 1
  mkdir -p "$C"
  printf 'remember_view=1\ncheck_updates=0\nprefetch_all=0\n%b' "$1" > "$C/settings.txt"
  ui_start "${2:-}" || { bad "no window"; return 1; }
  for _ in $(seq 1 60); do
    [ -f "$C/D.spincache" ] && { [ -n "${2:-}" ] || [ -f "$C/C.spincache" ]; } && break
    sleep 1
  done
  sleep 3
}
# Right-click, then the item that many places down the menu.
menu() { xdotool mousemove $((UI_X+$1)) $((UI_Y+$2)) click 3; sleep 1; for _ in $(seq 1 "$3"); do ui_key Down; done; ui_key Return; }
# Alt+F4, the way a person closes it: it reaches the program as WM_CLOSE,
# which is where the place is written back.
close_and_report() {   # shot want_flag want_path label
  sleep 4
  ui_shot "$1" "760x80+$((UI_X+268))+$((UI_Y))" >/dev/null
  ui_focus; ui_key alt+F4
  for _ in $(seq 1 30); do running || break; sleep 1; done
  running && { bad "did not exit on close"; stop; return; }
  local f p; f=$(setting last_all_drives); p=$(setting last_path)
  if [ "$f" = "$2" ] && [ "$p" = "$3" ]; then ok "$4"; else bad "$4: landed on last_all_drives=$f last_path=$p"; fi
}

rm -f "$C"/*.spincache
echo "1) a folder opened in a new tab opens on that folder"
open_view '' 'D:\' && { menu 330 48 3; close_and_report newtab_folder 0 'D:\Games' "the new tab is on D:\\Games"; }
echo "2) a drive opened in a new tab from All drives opens on that drive, inside All drives"
open_view 'last_all_drives=1\n' && { menu 330 48 3; close_and_report newtab_all_drives 1 'D:\' "the new tab is on D:\\ inside All drives"; }
echo "3) the largest file's row in All drives opens a new tab on its folder"
open_view 'last_all_drives=1\n' && {
  ui_click $((16 + 59 + 29)) $((PANEL_Y + 12)); sleep 2   # the Largest tab
  menu 134 "$ROW_Y" 1
  close_and_report newtab_largest_row 1 'D:\VMs\dev-box' "the new tab is on D:\\VMs\\dev-box inside All drives"
}
stop
echo "== $PASS passed, $FAIL failed =="
[ $FAIL -eq 0 ]
