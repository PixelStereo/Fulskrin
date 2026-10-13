<div align="center">
  
# PINOL

## Pixels In New Optical Languages

A new language for live visuals.



<p>
  <a href="https://github.com/PixelStereo/Pinol/actions/workflows/build.yml"><img alt="Build" src="https://github.com/PixelStereo/Pinol/actions/workflows/build.yml/badge.svg?branch=main"></a>
  <img alt="macOS · Windows · Linux" src="https://img.shields.io/badge/macOS%20%C2%B7%20Windows%20%C2%B7%20Linux-24272C?style=flat-square">
  <img alt="Qt 6 · C++17 · FFmpeg" src="https://img.shields.io/badge/Qt%206%20%C2%B7%20C%2B%2B17%20%C2%B7%20FFmpeg-24272C?style=flat-square">
</p>

<p><strong>
<a href="#built-for-the-show">Explore the features</a> &nbsp;·&nbsp;
<a href="#get-started">Get started</a> &nbsp;·&nbsp;
<a href="#build-from-source">Build from source</a>
</strong></p>

</div>

---

> **Every surface is a canvas. Every cue matters.**
>
> PINOL brings video, generative visuals, spatial mapping, animation and live control together in one show environment. From a single projection to a multi-output installation, you can design the image and control what happens next—without changing tools.

## Built for the show

| | |
| :--- | :--- |
| **01 — PLAY**<br>**Your media, all together.**<br>Video, HAP, images, audio, text, live inputs and ISF generators. Layer, group, blend and combine them with effects. | **02 — MAP**<br>**Take the picture beyond the rectangle.**<br>Position, scale, rotate, corner-pin and warp with a mesh. Shape content to a surface, crop it with ROI and route it through viewports. |
| **03 — CUE**<br>**Build the moment. Then recall it.**<br>Snapshots, selective presets and sequences with waits, fades and auto-follow. Animate parameters with waveforms, Bézier keys and timelines. | **04 — CONTROL**<br>**Hands on. Eyes up.**<br>OSC, OSCQuery, MIDI, keyboard and internal interactions, with Learn, ranges, curves, filters and pick-up or jump takeover. |
| **05 — SYNC**<br>**Keep the show on time.**<br>Time-of-day and transport-based clocks, LTC, MTC and ATC, plus routing to MIDI, audio and supported video destinations. | **06 — OUTPUT**<br>**From preview to projection.**<br>Fullscreen output windows, Syphon, Spout, NDI, OMT and Blackmagic DeckLink / UltraStudio—depending on your platform and installed runtimes. |

### Made to be played live

**Build visually.** Combine videos, generators, images and effects in layers and groups. Correct color, animate parameters, and transform the composition to fit the space.

**Perform with confidence.** Store the state of a scene in a snapshot, recall only the values you want with a preset, or build a sequence of cues. Each value can have its own fade and easing.

**Connect everything.** Learn incoming OSC, MIDI or keyboard controls; shape the response with ranges, filtering and curves; or let one parameter drive another internally. OSCQuery exposes a browsable address tree for your show.

**Feed the room.** Route viewports to displays and supported video protocols. Use timecode where the show needs to stay in step with other equipment.

## Under the hood

PINOL is written in **C++17 / Qt 6**, with **FFmpeg** for media decoding, **OpenGL 3.3** for rendering and **Metal-based output presentation on macOS**.

- **HAP-aware playback** — HAP textures are decompressed and kept GPU-compressed where supported, avoiding an unnecessary full RGBA conversion. HAP Q is the preferred fast path on macOS.
- **Hardware-assisted decoding** — VideoToolbox on macOS, with D3D11VA / VAAPI paths on supported systems; CPU fallback when needed.
- **Multi-stream playback controls** — bounded frame queues, shared decoded-frame accounting, upload-buffer budgeting and a shared worker pool for HAP.
- **Performance visibility** — decoder timing (including p95/p99), missed and skipped frames, latency metrics and a multi-video benchmark.
- **Automation and diagnostics** — OSC/OSCQuery, a filterable event log, engine tests, fuzz testing and ThreadSanitizer checks in CI.

These are engineering capabilities, **not a guarantee of a particular frame rate**: performance depends on codecs, resolution, layer count, effects, outputs and hardware.

## Get started

**Want to try a build?** Successful [GitHub Actions runs](https://github.com/PixelStereo/Pinol/actions/workflows/build.yml) provide downloadable platform archives in their **Artifacts** section. At present, there are no published GitHub Releases.

| macOS | Windows | Linux |
| :--- | :--- | :--- |
| Apple Silicon build; a `.app` in a ZIP | MSVC x64 build; Qt and FFmpeg DLLs included | Ubuntu 24.04 x86_64 archive; runtime packages required |
| Ad-hoc signed; **not notarized** | Includes Visual C++ runtime installer | Not yet a self-contained AppImage |

Optional NDI, OMT and DeckLink features require compatible vendor runtimes/drivers. Consult the [macOS signing notes](docs/macos-release-signing.md) for distribution details.

## Build from source

**Requirements:** CMake 3.21+, Qt 6.2+, FFmpeg 4+, C++17 and OpenGL 3.3 core. Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/PixelStereo/Pinol.git
cd Pinol
```

<details>
<summary><strong>macOS — Homebrew</strong></summary>

```bash
brew install qt ffmpeg cmake pkg-config
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j
```

Run `build/Pinol.app`. The build is not Developer ID signed or notarized.

</details>

<details>
<summary><strong>Linux — Ubuntu</strong></summary>

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev libqt6opengl6-dev \
  qt6-multimedia-dev libgl1-mesa-dev libavformat-dev libavcodec-dev \
  libswscale-dev libswresample-dev libavutil-dev libasound2-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/Pinol
```

</details>

<details>
<summary><strong>Windows — MSVC 2022</strong></summary>

Install Qt 6 (MSVC 2022, including Qt Multimedia), shared FFmpeg and CMake. From an MSVC developer shell:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" -DFFMPEG_ROOT="C:/ffmpeg"
cmake --build build --config Release
```

See [the CI workflow](.github/workflows/build.yml) for the tested toolchain and deployment steps.

</details>

<details>
<summary><strong>Tests, benchmarking and command-line render</strong></summary>

Generate media fixtures and run the engine tests:

```bash
sh test/make_media.sh
cmake --build build --target pinol_tests
ctest --test-dir build --output-on-failure
```

Render a project from the command line:

```bash
Pinol --render out.png --frames 1 --strict project.pinol
```

Benchmark multiple 4K streams:

```bash
cmake --build build --target pinol_bench
sh test/bench_4k.sh build
```

CI runs platform tests, the HAP/OSC fuzzers and selected ThreadSanitizer suites. See the [performance work plan](docs/performance-work-plan.md).

</details>

## Before the doors open

A few important current boundaries:

- **Video:** HAP Q is optimized for compressed GPU textures. HAP R / HAP HDR may require CPU block decoding on macOS because its OpenGL path lacks BPTC support.
- **Audio and control:** one stereo program-audio output; MIDI/OSC/keyboard interactions and timecode are available. DMX input is not yet implemented, and ISF audio inputs are currently black.
- **Output protocols:** NDI / OMT use GPU readback and add latency; DeckLink also buffers frames to the card's clock. Hardware outputs require their vendor software. DeckLink is currently video-only, 8-bit, without keying.
- **Network:** the OSC/OSCQuery server has **no password authentication**. In **Settings → OSC → Listen on**, bind to localhost or a trusted network interface rather than exposing the server to an untrusted network.

For details on dependencies, supported workflows and implementation, browse the [source](src/engine), [tests](test) and [build workflow](.github/workflows/build.yml).

---

<div align="center">

<h3>Bring the image to life.</h3>

<p><strong>PINOL</strong> — from the first pixel to the final cue.</p>

<p>
<a href="https://github.com/PixelStereo/Pinol/issues">Report an issue</a> &nbsp;·&nbsp;
<a href="https://github.com/PixelStereo/Pinol">Explore the code</a> &nbsp;·&nbsp;
<a href="https://github.com/PixelStereo/Pinol/actions/workflows/build.yml">View builds</a>
</p>

<p><sub>Built for projection, installations and live performance.</sub></p>

</div>
