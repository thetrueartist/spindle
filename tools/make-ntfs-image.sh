#!/usr/bin/env bash
# Build a small NTFS image with a known tree, for tests/test_mft.cpp.
#
#   tools/make-ntfs-image.sh build/demo.ntfs [size-mb]
#
# mkntfs writes the image and ntfs-3g mounts it through FUSE to fill it,
# which needs root (or a setuid ntfs-3g), so CI runs this under sudo. The
# tree exercises what the assembler has to get right: names that are
# resident and non-resident, long, non-ASCII and spaced; a directory big
# enough to need an index allocation; a chain deeper than any sane tree;
# an empty file; and a hard-linked pair, which the table holds once.
# Beside the image goes a manifest of what was written, one entry per
# line: type (d or f), size, inode (the MFT record number), path.
set -eu
out=${1:?usage: make-ntfs-image.sh OUT [size-mb]}
size=${2:-64}
for t in mkfs.ntfs ntfs-3g; do
  command -v "$t" >/dev/null 2>&1 || { echo "make-ntfs-image: $t not found (install ntfs-3g)" >&2; exit 1; }
done
mnt=$(mktemp -d)
cleanup() {
  if mountpoint -q "$mnt" 2>/dev/null; then
    fusermount3 -u "$mnt" 2>/dev/null || fusermount -u "$mnt" 2>/dev/null || umount "$mnt"
  fi
  rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
mkdir -p "$(dirname "$out")"
rm -f "$out" "$out.manifest"
truncate -s "${size}M" "$out"
mkfs.ntfs -F -f -q -L SPINDLE -s 512 -c 4096 "$out" >/dev/null 2>&1
ntfs-3g "$out" "$mnt"

# fill PATH BYTES: a file of exactly that many bytes, under the mount.
fill() {
  mkdir -p "$mnt/$(dirname "$1")"
  head -c "$2" /dev/urandom > "$mnt/$1"
}
fill "Program Files/Northwind/bin/northwind.exe" 1500000
fill "Program Files/Northwind/bin/config.ini" 200            # resident data
fill "Program Files/Northwind/readme.txt" 640                # resident, near the limit
fill "Users/sam/Documents/Reports 2025/Q1 summary.pdf" 70000
fill "Users/sam/Documents/notes.md" 5000
fill "Users/sam/Pictures/café.jpg" 300000
fill "Users/sam/Pictures/日本語の写真.jpg" 123456
fill "Videos/holiday.mp4" 2500000
fill "src/spindle/core.cpp" 90000
mkdir -p "$mnt/src/spindle"; : > "$mnt/src/spindle/empty.h"     # zero bytes
mkdir -p "$mnt/Users/sam/Documents/empty folder"
long=$(printf 'n%.0s' $(seq 1 200))
fill "Users/sam/Documents/$long.txt" 1000
# A directory large enough to need an index allocation, not just the
# resident index root.
mkdir -p "$mnt/Users/sam/Pictures/Iceland"
for i in $(seq -w 1 600); do : > "$mnt/Users/sam/Pictures/Iceland/DSC_$i.NEF"; done
fill "Users/sam/Pictures/Iceland/DSC_0001.NEF" 24000000
# Deeper than any sane tree, though well inside the assembler's limit.
d="deep"; for i in $(seq 1 40); do d="$d/level$i"; done
fill "$d/bottom.txt" 10
# One record, two names.
fill "Users/sam/Documents/shared.dat" 4096
ln "$mnt/Users/sam/Documents/shared.dat" "$mnt/Videos/shared-link.dat"
sync
(cd "$mnt" && find . -mindepth 1 -printf '%y\t%s\t%i\t%P\n' | LC_ALL=C sort -t "$(printf '\t')" -k4) > "$out.manifest"
echo "make-ntfs-image: $out ($(wc -l < "$out.manifest") entries)"
