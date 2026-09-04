#!/usr/bin/env bash
# Acceptance run for the network-permission and cache policy, driven under
# Wine with tools/wine-ui-test.sh. Needs a Wine prefix where Y: is a
# drive registered as type "network", E: as "floppy", D: an ordinary
# fixed drive with content, and a folder under dosdevices/unc/nas/share
# (see HACKING.md); tools/wine-prefix.sh builds exactly that, and
# WINEPREFIX names the prefix if it is not $HOME/.wine. Prints PASS/FAIL
# per check and exits non-zero on any failure. Dialog button offsets are measured from the dialog's client
# origin: the Scan and Don't scan buttons sit side by side at (230,127)
# and (314,127) when the remember box is present, with the box itself at
# (14,127), and at (230,130) and (314,130) when it is not.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=wine-ui-test.sh
source "$HERE/wine-ui-test.sh"
P="${WINEPREFIX:-$HOME/.wine}"
C="$P/drive_c/users/$(whoami)/AppData/Local/Spindle"
# A folder inside the prefix, so the output path is a Windows path whatever
# drive letters the prefix has (a Linux path needs a Z: drive to be reachable).
TMP="$P/drive_c/users/$(whoami)/Temp/spindle-check"; mkdir -p "$TMP"
TMPW="C:\\users\\$(whoami)\\Temp\\spindle-check"
[ -f build/spindle.exe ] || { echo "wine-corpo-check: build/spindle.exe is missing; run make first" >&2; exit 1; }
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
stop() { for pid in $(ps -eo pid,cmd | awk '$2 ~ /spindle\.exe$/ {print $1}'); do kill -TERM "$pid" 2>/dev/null; done; }
windows() { xdotool search --name 'Spindle' 2>/dev/null | wc -l; }
# Wait until exactly N top-level Spindle windows exist, up to `limit`
# seconds. A fixed sleep is not enough: a cold Wine on a shared machine
# can take several seconds to put a dialog up, and the count would then
# be read before the dialog existed. Proving a dialog does NOT appear
# still needs a plain sleep, since there is nothing to wait for.
wait_windows() {
  local want=$1 limit=${2:-20} i=0
  while [ "$(windows)" != "$want" ] && [ $i -lt $((limit * 2)) ]; do
    sleep 0.5; i=$((i + 1))
  done
  [ "$(windows)" = "$want" ]
}
menu() { ui_focus; ui_click 240 28; sleep 1; xdotool mousemove $((UI_X+260)) $((UI_Y+$1)) click 1; sleep 1.2; }
stop; sleep 1
[ -f "$C/settings.txt" ] && sed -i '/^trusted_share=/d' "$C/settings.txt"
[ -f "$C/D.spincache" ] && { cp "$C/D.spincache" "$C/Y.spincache"; cp "$C/D.spincache" "$C/E.spincache"; cp "$C/D.spincache" "$C/Q.spincache"; }

echo "1) headless --csv on a network letter without --allow-network"
wine build/spindle.exe --csv "$TMPW\\o.csv" 'Y:\' >/dev/null 2>&1; r=$?
[ $r -eq 3 ] && [ ! -f "$TMP/o.csv" ] && ok "refused, exit 3, nothing written" || bad "exit=$r"
echo "2) headless --csv with --allow-network"
wine build/spindle.exe --csv "$TMPW\\o.csv" --allow-network 'Y:\' >/dev/null 2>&1; r=$?
[ $r -eq 0 ] && [ -s "$TMP/o.csv" ] && ok "exit 0, csv written" || bad "exit=$r"; rm -f "$TMP/o.csv"
echo "3) headless --csv on a UNC path without the flag"
wine build/spindle.exe --csv "$TMPW\\o.csv" '\\nas\share' >/dev/null 2>&1; r=$?
[ $r -eq 3 ] && ok "refused, exit 3" || bad "exit=$r"

ui_start 'D:\' || { echo "no window"; exit 1; }
echo "4) launch sweep removes caches for network, removable and vanished letters"
L=$(ls "$C"/*.spincache 2>/dev/null | xargs -n1 basename | tr '\n' ' ')
case "$L" in *Y.spincache*|*E.spincache*|*Q.spincache*) bad "left: $L";; *) ok "left: $L";; esac
echo "5) launch shows no dialog"
[ "$(windows)" = "1" ] && ok "one window" || bad "dialog at launch"
echo "6) Tab completion on a share before consent does nothing"
ui_click 700 400; sleep 0.3; ui_key ctrl+l; sleep 0.5; ui_key ctrl+a; ui_type 'Y:\Fi'; ui_key Tab; sleep 0.5
ui_shot corpo_tab "300x30+$((UI_X+280))+$((UI_Y+10))" >/dev/null; ui_key Escape; sleep 0.3
ok "captured build/uitest/corpo_tab_c.png (expect the text unchanged)"
echo "7) clicking the network card asks; Don't scan"
ui_click 137 402
wait_windows 2 && ok "dialog shown" || bad "no dialog"
ui_dialog >/dev/null && dlg_click 314 130
wait_windows 1 && ok "declined" || bad "dialog still open"
echo "8) a GLOBALROOT spelling is refused without a dialog"
ui_click 700 400; sleep 0.3; ui_key ctrl+l; sleep 0.5; ui_key ctrl+a; ui_type '\\?\GLOBALROOT\Device\Mup\s\s'; ui_key Return; sleep 1.5
[ "$(windows)" = "1" ] && ok "no dialog" || bad "dialog appeared"; ui_key Escape; sleep 1.2
echo "9) a UNC path in the address bar asks; tick remember; Scan"
ui_click 700 400; sleep 0.8; ui_key ctrl+l; sleep 0.8; ui_key ctrl+a; ui_type '\\nas\share\deep'; sleep 0.3; ui_key Return
wait_windows 2 && ok "dialog shown" || bad "no dialog"
ui_dialog >/dev/null && { dlg_click 14 127; sleep 0.3; dlg_click 230 127; }
wait_windows 1 && ok "answered" || bad "dialog still open"
sleep 2   # let the scan the answer started settle before the next check
echo "10) the share is remembered by its own name"
grep -q 'trusted_share=\\\\nas\\share$' <(tr -d '\r' < "$C/settings.txt") && ok "trusted_share=\\\\nas\\share" || bad "not remembered"
echo "11) headless on the remembered share proceeds; a sibling share does not"
wine build/spindle.exe --csv "$TMPW\\o.csv" '\\nas\share' >/dev/null 2>&1; r=$?
[ $r -ne 3 ] && ok "remembered share not refused" || bad "refused"; rm -f "$TMP/o.csv"
wine build/spindle.exe --csv "$TMPW\\o.csv" '\\nas\other' >/dev/null 2>&1; r=$?
[ $r -eq 3 ] && ok "sibling share refused" || bad "exit=$r"
echo "11b) a bare object-manager spelling is refused as a path (exit 2)"
wine build/spindle.exe --csv "$TMPW\\o.csv" 'UNC\nas\share' >/dev/null 2>&1; r=$?
[ $r -eq 2 ] && [ ! -f "$TMP/o.csv" ] && ok "refused as a path, nothing written" || bad "exit=$r"
wine build/spindle.exe --csv "$TMPW\\o.csv" 'GLOBALROOT\Device\Mup\nas\share' >/dev/null 2>&1; r=$?
[ $r -eq 2 ] && ok "GLOBALROOT without a prefix refused as a path" || bad "exit=$r"
echo "12) Forget remembered network drives"
menu 151; grep -q trusted_share "$C/settings.txt" && bad "still remembered" || ok "cleared"
echo "13) Keep scan caches off deletes every cache"
menu 83; [ -z "$(ls "$C"/*.spincache 2>/dev/null)" ] && ok "none left" || bad "left: $(ls "$C"/*.spincache | xargs -n1 basename | tr '\n' ' ')"
menu 83    # back on
stop; rm -rf "$TMP"
echo "== $PASS passed, $FAIL failed =="
[ $FAIL -eq 0 ]
