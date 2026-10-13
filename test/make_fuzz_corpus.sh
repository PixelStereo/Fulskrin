#!/bin/sh
# Seeds for the fuzzers: every HAP frame of the test media, one file each (test/fuzz_hap.cpp), and a few OSC
# packets (test/fuzz_osc.cpp, in <folder>/osc).
# Usage: test/make_fuzz_corpus.sh <folder>      (after test/make_media.sh)
set -e
cd "$(dirname "$0")"
out=${1:-fuzz_corpus}
mkdir -p "$out"
for f in media/hap*.mov; do
    [ -f "$f" ] || continue
    ffmpeg -loglevel error -y -i "$f" -map 0:v -c copy -f image2 "$out/$(basename "$f" .mov)_%03d.hap"
done
# OSC: a message of every argument type, a bundle holding two, an address pattern
mkdir -p "$out/osc"
printf '/layer/a/opacity\0\0\0\0,f\0\0\077\0\0' > "$out/osc/float"
printf '/composition/x\0\0,ihdsSbcrmtTFNI\0\0\0\0\0\0\0\001\0\0\0\0\0\0\0\002\077\360\0\0\0\0\0\0hi\0\0S\0\0\0\0\0\0\002ab\0\0\0\0\0A\377\0\0\377\0\220\074\100\0\0\0\0\0\0\0\001' > "$out/osc/all_types"
printf '#bundle\0\0\0\0\0\0\0\0\001\0\0\0\024/a\0\0,i\0\0\0\0\0\007\0\0\0\0\0\0\0\020/b/*\0\0\0\0,T\0\0' > "$out/osc/bundle"
ls "$out" | wc -l
