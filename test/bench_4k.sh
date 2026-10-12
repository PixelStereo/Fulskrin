#!/bin/sh
# How many 4K videos play together at their own rate, codec by codec, with and without zero copy.
#
#   sh test/bench_4k.sh [build dir] [max layers]
#
# Makes 4K test files once (test/media/4k, 10 s, 25 fps), then plays 1, 2, 3, 4, 6, 8… copies of each in one 4K
# composition at 60 fps with pinol_bench, and prints the largest count where every layer kept its rate.
# Requires the ffmpeg program and the bench (cmake --build build --target pinol_bench).
set -e
BUILD=${1:-build}
MAX=${2:-16}
ROOT=$(cd "$(dirname "$0")" && pwd)
BENCH="$BUILD/pinol_bench"
[ -x "$BENCH" ] || BENCH="$BUILD/pinol_bench.app/Contents/MacOS/pinol_bench"
[ -x "$BENCH" ] || { echo "No pinol_bench in $BUILD (cmake --build $BUILD --target pinol_bench)"; exit 1; }
MEDIA="$ROOT/media/4k"
mkdir -p "$MEDIA"
SRC="testsrc2=size=3840x2160:rate=25"
mk() { # name, ffmpeg output options
  [ -f "$MEDIA/$1" ] && return 0
  echo "Making $1…"
  ffmpeg -loglevel error -y -f lavfi -i "$SRC" -t 10 $2 "$MEDIA/$1" || { echo "  (skipped: this ffmpeg cannot make it)"; rm -f "$MEDIA/$1"; }
}
mk h264.mp4 "-c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p"
mk hevc10.mp4 "-c:v libx265 -preset fast -crf 20 -pix_fmt yuv420p10le -tag:v hvc1"
mk prores422hq.mov "-c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le"
mk hapq.mov "-c:v hap -format hap_q"

run() { # file, extra options: prints the largest count that kept its rate
  best=0
  for n in 1 2 3 4 6 8 10 12 16 20 24 32; do
    [ "$n" -le "$MAX" ] || break
    set --
    i=0
    while [ "$i" -lt "$n" ]; do set -- "$@" "$FILE"; i=$((i + 1)); done
    if "$BENCH" --seconds 6 --fps 60 --comp 3840x2160 $OPTS "$@" > /tmp/pinol_bench.log 2>&1; then
      best=$n
    else
      break
    fi
  done
  echo "$best"
}
printf "%-18s %12s %12s\n" "file" "zero copy" "copy"
for f in h264.mp4 hevc10.mp4 prores422hq.mov hapq.mov; do
  [ -f "$MEDIA/$f" ] || continue
  FILE="$MEDIA/$f"
  OPTS=""
  a=$(run)
  OPTS="--copy"
  b=$(run)
  printf "%-18s %12s %12s\n" "$f" "$a" "$b"
done
echo "Layers that kept 25 fps each (up to $MAX). Details of the last run: /tmp/pinol_bench.log"
