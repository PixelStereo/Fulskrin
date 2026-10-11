# Fulskrin

Video mapping for stage, installations and live shows: layers of video, images, sound, text and ISF shaders, warped onto
surfaces, sent to several outputs, recalled as snapshots and driven by OSC. Qt 6, FFmpeg, OpenGL 3.3 (Metal output on
macOS). macOS, Windows, Linux.

## Features

- **Layers and groups**: video (any FFmpeg codec, HAP, with sound), image, ISF generator, text, another layer. ROI, color, ISF effect chain, blend modes.
- **Mapping**: position, scale, rotation, pivot, corner pin, warp mesh. Viewports send parts of the composition to screens.
- **Snapshots, presets and sequences**: every value fades to the snapshot's own way (time and easing per value or group); snapshots in nested groups, reordered by dragging; presets hold only the values put in them; cue lists with waits and auto-follows.
- **Animations**: any number animated by a waveform or Bézier keys, per layer or in timelines.
- **Outputs**: fullscreen windows, Syphon, Spout, NDI, OMT.
- **Control**: OSC and OSCQuery (UDP 1234, TCP 5678, Zeroconf). One address per value everywhere: OSC, project file, snapshots, timelines (`/layer/<name>/position/x`, `…/fx/<fx>/param/<input>`, `…/<param>/anim/play`).

## Build

```bash
# macOS
brew install qt ffmpeg cmake pkg-config
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" && cmake --build build -j

# Linux (Ubuntu)
sudo apt install build-essential cmake pkg-config qt6-base-dev libqt6opengl6-dev libgl1-mesa-dev \
  libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev
cmake -S . -B build && cmake --build build -j
```

Windows: Qt 6 (MSVC 2022), shared FFmpeg, then
`cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 -DFFMPEG_ROOT=C:\ffmpeg && cmake --build build --config Release`.

Requires Qt 6.2+, FFmpeg 4+, CMake 3.21+, C++17, OpenGL 3.3 core.

Tests: `sh test/make_media.sh && cmake --build build --target fulskrin_tests && ctest --test-dir build`
(one process per area; `fulskrin_tests --area snapshots` runs one; CI runs them on the three platforms).

## Limits

- Video decoded on the CPU (fine for a few HD streams); one stereo audio output.
- No MIDI/DMX, no camera or network inputs yet; ISF audio inputs are black.
- NDI and OMT send a GPU readback (one frame of latency).

Links: [ISF](https://www.interactiveshaderformat.com/) · [OSCQuery](https://github.com/Vidvox/OSCQueryProposal)
