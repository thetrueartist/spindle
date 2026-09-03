#!/usr/bin/env bash
# Drive the Windows build under Wine for interface testing on Linux.
#
# The two lessons that make UI automation here reliable, learned the hard
# way: a bare Xvfb gives child controls no keyboard focus, so a text box
# looks like it ignores typing (run a window manager, openbox, to fix it);
# and xdotool's getwindowgeometry returns the window FRAME, which a window
# manager offsets from the client by the title bar, so clicks land high
# (read the client origin from xwininfo's "Absolute upper-left" instead).
#
# Source this, then: ui_start ['D:\']  ->  ui_click X Y (client coords),
# ui_key <keys>, ui_type "text", ui_shot name [WxH+dx+dy]. Coordinates are
# client-relative; the helpers add the client origin.
set -u
: "${DISPLAY:=:99}"
export DISPLAY WINEDEBUG=-all
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UI_OUT="${UI_OUT:-$REPO/build/uitest}"
mkdir -p "$UI_OUT"

pgrep -x Xvfb    >/dev/null || { (setsid Xvfb "$DISPLAY" -screen 0 1280x800x24 >/dev/null 2>&1 &); sleep 2; }
pgrep -x openbox >/dev/null || { (setsid openbox >/dev/null 2>&1 &); sleep 1; }

ui_start() {
  for P in $(ps -eo pid,cmd | awk '$2 ~ /spindle\.exe$/ {print $1}'); do kill -TERM "$P" 2>/dev/null; done
  sleep 2
  ( cd "$REPO" && setsid wine build/spindle.exe ${1:+"$1"} >/dev/null 2>&1 & )
  local W=""
  for _ in $(seq 1 40); do W=$(xdotool search --name '^Spindle$' 2>/dev/null | head -1); [ -n "$W" ] && break; sleep 1; done
  [ -z "$W" ] && { echo "ui: window never appeared"; return 1; }
  UI_W=$W; sleep 4
  xdotool windowactivate --sync "$W" 2>/dev/null
  local info; info=$(xwininfo -id "$W" 2>/dev/null)
  UI_X=$(printf '%s' "$info" | awk '/Absolute upper-left X/{print $NF}')
  UI_Y=$(printf '%s' "$info" | awk '/Absolute upper-left Y/{print $NF}')
  echo "ui: window $W client origin $UI_X,$UI_Y"
}
ui_focus() { xdotool windowactivate --sync "$UI_W" 2>/dev/null; }
ui_click() { xdotool mousemove $((UI_X+$1)) $((UI_Y+$2)) click 1; }
ui_key()   { xdotool key --clearmodifiers "$@"; }
ui_type()  { xdotool type --clearmodifiers --delay "${UI_DELAY:-50}" "$1"; }
ui_shot()  {
  import -window root "$UI_OUT/$1.png"
  [ -n "${2:-}" ] && convert "$UI_OUT/$1.png" -crop "$2" +repage "$UI_OUT/${1}_c.png"
  echo "ui: shot $1 -> $UI_OUT/$1.png"
}
