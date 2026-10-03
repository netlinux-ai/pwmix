#!/bin/sh
# Build the .deb from the committed tree, entirely inside scratch/deb.
# dpkg-buildpackage writes its output next to the source directory, so the
# source is exported into scratch/deb/src and the .deb lands in scratch/deb/.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/scratch/deb"
export TMPDIR="$root/scratch/tmp"
mkdir -p "$TMPDIR"
rm -rf "$out"
mkdir -p "$out/src"
git -C "$root" archive HEAD | tar -x -C "$out/src"
cd "$out/src"
dpkg-buildpackage -b -us -uc
ls -l "$out"/*.deb
