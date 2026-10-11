# Fulskrin

Video mapping for stage, installations and live shows: layers of video, images, sound, text and ISF shaders, warped onto
surfaces, sent to several outputs, recalled as snapshots and driven by OSC. Qt 6, FFmpeg, OpenGL 3.3 (Metal output on
macOS). macOS, Windows, Linux.

## Features

- **Layers and groups**: video (any FFmpeg codec, HAP, with sound), image, ISF generator, text, live input (camera, NDI, OMT, Syphon), another layer. ROI, color, ISF effect chain, blend modes.
- **Mapping**: position, scale, rotation, pivot, corner pin, warp mesh. Viewports send parts of the composition to screens.
- **Snapshots, presets and sequences**: every value fades to the snapshot's own way (time and easing per value or group); snapshots in nested groups, reordered by dragging; presets hold only the values put in them; cue lists with waits and auto-follows.
- **Animations**: any number animated by a waveform or Bézier keys, per layer or in timelines.
- **Outputs**: fullscreen windows, Syphon, Spout, NDI, OMT, Blackmagic DeckLink / UltraStudio (SDI, HDMI).
- **Timecode**: a generator layer per clock (time of day, a free run with its own transport — play, pause, seek, in / out,
  loop, ping-pong — held by snapshots and presets, or LTC / MTC / ATC received). Each layer drives one of the
  composition's named timecode outputs; the patch (Composition tab) sends an output to any number of destinations, and the
  destination gives the protocol: a named MIDI output (MTC, on a port or a virtual port), a named audio channel (LTC, a
  channel of any output device), a viewport (ATC in its NDI frames and DeckLink SDI: RP188, VITC in SD).
- **Control**: OSC and OSCQuery (UDP 1234, TCP 5678, Zeroconf). One address per value everywhere: OSC, project file, snapshots, timelines (`/layer/<name>/position/x`, `…/fx/<fx>/param/<input>`, `…/<param>/anim/play`).

## Build

The libraries Fulskrin builds from source are git submodules in `third_party/` (miniaudio, bcdec, Syphon, Spout,
RtMidi): clone with `git clone --recursive`, or run `git submodule update --init` in a clone.

```bash
# macOS
brew install qt ffmpeg cmake pkg-config
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" && cmake --build build -j

# Linux (Ubuntu)
sudo apt install build-essential cmake pkg-config qt6-base-dev libqt6opengl6-dev qt6-multimedia-dev libgl1-mesa-dev \
  libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev libasound2-dev
cmake -S . -B build && cmake --build build -j
```

Windows: Qt 6 (MSVC 2022, with Qt Multimedia), shared FFmpeg, then
`cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 -DFFMPEG_ROOT=C:\ffmpeg && cmake --build build --config Release`.

Requires Qt 6.2+, FFmpeg 4+, CMake 3.21+, C++17, OpenGL 3.3 core.

Tests: `sh test/make_media.sh && cmake --build build --target fulskrin_tests && ctest --test-dir build`
(one process per area; `fulskrin_tests --area snapshots` runs one; CI runs them on the three platforms).
Playback benchmark: `cmake --build build --target fulskrin_bench && sh test/bench_4k.sh build` (how many 4K files
play together, per codec, with and without zero copy).
Fuzzing of the HAP and Snappy readers (Clang, libFuzzer + ASan / UBSan; CI runs it two minutes):
`cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DFULSKRIN_FUZZ=ON && cmake --build build-fuzz --target fuzz_hap`,
then `sh test/make_fuzz_corpus.sh "$PWD/corpus" && ./build-fuzz/fuzz_hap -max_total_time=600 corpus`.
HAP is read by Fulskrin itself (sections, chunks, Snappy, GPU blocks), not by Vidvox's library: on 4K frames its
Snappy decompression is as fast as Google's or faster (same output), and it has no dependency.

## Limits

- Video: H.264, HEVC and ProRes decoded by the graphics hardware when it can (VideoToolbox on macOS, sampled in
  place with no copy; D3D11VA / VAAPI elsewhere, copied back then uploaded); other codecs on the CPU. HAP is
  decompressed on the CPU and sampled compressed, except Hap R and Hap HDR on macOS (no BPTC in its OpenGL: decoded
  on the CPU, slow in 4K; Hap Q is the fast one there). One stereo audio output.
- MIDI for the timecode only (MTC), no DMX; ISF audio inputs are black. Cameras need Qt Multimedia (without it, none are listed).
- NDI and OMT send a GPU readback (one frame of latency). DeckLink plays the same readback on the card's clock,
  three frames ahead (a new picture is shown at most four frames later; a missing one repeats the last). Its ports
  show up when Blackmagic Desktop Video is installed. A picture of another size than the mode is scaled (nearest):
  set the viewport to the mode's size. Video only, 8 bits, no keying.

Third party in the source tree: the Blackmagic DeckLink SDK's interfaces (`third_party/decklink`, SDK 12.0, Boost
Software License 1.0, see its LICENSE; a copy, as OBS, GStreamer and VLC keep: Blackmagic publishes no repository).

Links: [ISF](https://www.interactiveshaderformat.com/) · [OSCQuery](https://github.com/Vidvox/OSCQueryProposal)
