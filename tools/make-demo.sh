#!/usr/bin/env bash
# Turn the walkthrough frames captured by tools/win-screenshots.ps1 into
# docs/demo.gif and docs/demo.mp4.
#
#   tools/make-demo.sh FRAMES-DIR [WIDTH]     default width 880
#
# Eight frames a second, which is the rate the frames were taken at, and
# scaled to the README's column. Needs ImageMagick and ffmpeg.
set -eu
dir=${1:?usage: make-demo.sh FRAMES-DIR [WIDTH]}
width=${2:-880}
for t in convert ffmpeg; do
  command -v "$t" >/dev/null 2>&1 || { echo "make-demo: $t not found" >&2; exit 1; }
done
frames=$(ls "$dir"/frame_*.png 2>/dev/null | wc -l)
[ "$frames" -gt 0 ] || { echo "make-demo: no frame_*.png in $dir" >&2; exit 1; }
here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
convert -delay 12 -loop 0 "$dir"/frame_*.png -resize "${width}x" -layers Optimize "$here/docs/demo.gif"
ffmpeg -v error -y -framerate 8 -pattern_type glob -i "$dir/frame_*.png" \
  -vf "scale=${width}:-2" -c:v libx264 -pix_fmt yuv420p -crf 23 -movflags +faststart \
  "$here/docs/demo.mp4"
echo "make-demo: $frames frames -> docs/demo.gif ($(du -h "$here/docs/demo.gif" | cut -f1)) and docs/demo.mp4 ($(du -h "$here/docs/demo.mp4" | cut -f1))"
