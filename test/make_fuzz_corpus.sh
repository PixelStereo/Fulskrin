#!/bin/sh
# Seeds for the fuzzer (test/fuzz_hap.cpp): every HAP frame of the test media, one file each.
# Usage: test/make_fuzz_corpus.sh <folder>      (after test/make_media.sh)
set -e
cd "$(dirname "$0")"
out=${1:-fuzz_corpus}
mkdir -p "$out"
for f in media/hap*.mov; do
    [ -f "$f" ] || continue
    ffmpeg -loglevel error -y -i "$f" -map 0:v -c copy -f image2 "$out/$(basename "$f" .mov)_%03d.hap"
done
ls "$out" | wc -l
