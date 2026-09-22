#!/usr/bin/env bash
# Round trip for "Remember where I was" under Wine: the view that was
# open at close is the view that opens at launch, All drives included.
#
# Proved from what the program writes back, not from pixels. Settings are
# seeded with a view, the program is launched, left to load, and closed
# the way a person would; then its settings are read. A launch that fell
# back to a single drive writes last_all_drives=0; one that never finished
# loading writes no last_path at all, since the place is captured only
# once a tree is on screen; and a folder seeded in the wrong case comes
# back in its real case only if the trail was really replayed. Needs the
# prefix from tools/wine-prefix.sh (C: and a D: with a Games folder) and a
# built build/spindle.exe. A screenshot of each launch lands in build/uitest.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=wine-ui-test.sh
source "$HERE/wine-ui-test.sh"
[ -f build/spindle.exe ] || { echo "wine-remember-check: build/spindle.exe is missing; run make first" >&2; exit 1; }
P="${WINEPREFIX:-$HOME/.wine}"
C="$P/drive_c/users/$(whoami)/AppData/Local/Spindle"
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
stop() { for pid in $(ps -eo pid,cmd | awk '$2 ~ /spindle\.exe$/ {print $1}'); do kill -TERM "$pid" 2>/dev/null; done; }
running() { ps -eo cmd | grep -qE 'spindle\.exe$'; }
setting() { tr -d '\r' < "$C/settings.txt" 2>/dev/null | grep "^$1=" | head -1 | cut -d= -f2-; }

# Seed, launch, wait for the caches the load writes (the aggregate walks
# each fixed drive and writes its cache as it goes; a single-drive launch
# prefetches the rest), capture, close, wait for the exit.
round_trip() {
  stop; sleep 1
  mkdir -p "$C"; rm -f "$C"/*.spincache
  printf 'remember_view=1\ncheck_updates=0\n%b' "$1" > "$C/settings.txt"
  ui_start || { bad "no window"; return; }
  for _ in $(seq 1 60); do
    [ -f "$C/C.spincache" ] && [ -f "$C/D.spincache" ] && break
    sleep 1
  done
  sleep 3
  ui_shot "$2" "760x44+$((UI_X+268))+$((UI_Y))" >/dev/null
  # Alt+F4, the way a person closes it: it reaches the program as WM_CLOSE,
  # which is where the place is written back. xdotool's windowclose would
  # destroy the X window underneath Wine instead, and nothing gets saved.
  ui_focus; ui_key alt+F4
  for _ in $(seq 1 30); do running || break; sleep 1; done
  running && { bad "did not exit on close"; stop; }
}
report() {   # want_flag want_path label
  local f p; f=$(setting last_all_drives); p=$(setting last_path)
  if [ "$f" = "$1" ] && [ "$p" = "$2" ]; then ok "$3"; else bad "$3: got last_all_drives=$f last_path=$p"; fi
}

echo "1) All drives at its root comes back as All drives"
round_trip 'last_all_drives=1\n' remember_root
report 1 'All drives' "last_all_drives=1, last_path=All drives"
echo "2) a folder inside All drives comes back inside All drives, in its real case"
round_trip 'last_all_drives=1\nlast_path=d:\\games\n' remember_folder
report 1 'D:\Games' "last_all_drives=1, last_path=D:\\Games"
echo "3) a folder on one drive still comes back on that drive alone"
round_trip 'last_path=d:\\games\n' remember_drive
report 0 'D:\Games' "last_all_drives=0, last_path=D:\\Games"
stop
echo "== $PASS passed, $FAIL failed =="
[ $FAIL -eq 0 ]
