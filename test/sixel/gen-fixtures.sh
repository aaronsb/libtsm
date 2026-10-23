#!/bin/sh
# Regenerate the libsixel fixtures: the PPM sources from gen-fixtures.py,
# their sixel streams from libsixel's encoder through sixel-encode.c, with the
# img2sixel options listed per fixture, and libsixel's own decoding of each
# stream (sixel2png, then ImageMagick to PPM) as NAME.ref.ppm. Needs libsixel,
# ImageMagick and a C compiler on the host; the tests only read the committed
# files. libsixel 1.10.5's -I output differs from run to run, so highcolor.six
# changes on every regeneration.
#
#   test/sixel/gen-fixtures.sh
set -eu
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O2 -o "$tmp/sixel-encode" sixel-encode.c -lsixel
python3 gen-fixtures.py .

# name, then the img2sixel options it was encoded with
while read -r name opts; do
	# shellcheck disable=SC2086
	"$tmp/sixel-encode" "$name.ppm" $opts > "$name.six"
	sixel2png -i "$name.six" -o "$tmp/$name.png"
	magick "$tmp/$name.png" -depth 8 "$name.ref.ppm"
	echo "$name.six: img2sixel $opts ($(pkg-config --modversion libsixel 2>/dev/null || echo libsixel))"
done <<'LIST'
palette16 -d none -p 16
gradient -p 256
highcolor -I
LIST
