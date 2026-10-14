# GPU sharing: acceptance checks for Syphon and Spout

This change makes one-frame-delayed GPU sharing the default. Viewports allocate up to three render targets while the delayed path is actively sharing. On a fresh viewport the third target is allocated on demand by the delayed path. Once allocated, it may remain cached after switching back to immediate sharing; the viewport teardown releases all three targets.

## What each SDK does with the texture (read in the submodules)

- **Syphon** (`third_party/syphon`, built with `SYPHON_CORE_SHARE`): `publishFrameTexture` makes a context *shared with* Pinol's render context current, draws the texture into the IOSurface there, calls `glFlushRenderAPPLE()` (a flush, not a finish) and restores Pinol's context. The copy therefore runs on another context whose commands are not ordered with Pinol's. Nothing in the render context can wait for it. This is the case the third buffer exists for: the shared picture is not drawn into by the next frame, only by the one after, which leaves the flushed copy one whole frame (16.7 ms at 60 Hz) to run. It is a margin, not a proof: the physical test below remains the merge gate on macOS.
- **Spout** (`third_party/spout2`, `spoutGL::WriteGLDXtexture`): the copy is made in the *calling* context (Pinol's render context), between `wglDXLockObjectsNV` and `wglDXUnlockObjectsNV`. Commands of one context run in order, and the unlock completes the GL work before DirectX touches the shared texture: Spout is safe by construction, with two buffers as with three. The physical test on Windows checks the driver, not the ordering.

## What the tests prove

- `pinol_tests --area rendering`: the buffer-index arithmetic (the next destination differs from the frame just rendered and from the previous one handed to the publisher), repeated rotations, and the return to immediate sharing.
- The publishing test with a stand-in GPU publisher (`Engine::setGpuPublisherFactoryForTests`, every platform, no SDK): the texture shared at the end of frame *f* is never the one shared at frame *f + 2*, i.e. the picture drawn at *f + 1*. With two buffers it would be.

Neither proves that Syphon's shared context has finished its copy within one frame on a given GPU and driver; only the receiver-side test does.

## Physical acceptance test required before merge

1. On macOS, connect an actual Syphon receiver; on Windows, connect an actual Spout receiver. Test on the drivers and GPU used for live work, not only llvmpipe.
2. Run 1080p and 4K at 30/60 fps, with two viewports and substantial GPU load.
3. Use the bundled `ShareFrameId.fs` ISF diagnostic generator, with no additional color effects or viewport transformations. Its interior encodes `FRAMEINDEX` as RGB bytes (red is least significant), and the border alternates black/white by frame. Capture decoded pixels **at the receiver**. Require identical IDs in distant interior regions, border phase consistent with ID parity, and monotonically increasing IDs (allow drops but investigate duplicates/repeats and backwards IDs).
4. Toggle immediate/delayed sharing, disconnect and reconnect receivers, resize viewports, and stress VRAM pressure. Compare render p95/p99, shares_skipped and visible artifacts.
5. Check Syphon/Spout SDK versions. If a publisher does not guarantee source ownership or completed submission after publish returns, add a publisher-owned staging copy with a verifiable completion strategy before releasing this PR.
6. Run a sustained live project on the actual show machine. Keep the PR draft if any receiver shows mixed, torn or stale frames.

## VRAM cost

A 3840x2160 RGBA16 texture is 63.3 MiB. Triple buffering is about 189.8 MiB per 4K viewport versus 126.6 MiB for double buffering, before multisample or other targets.

The input project's serialization is not changed. The machine-level setting defaults to delayed only when no explicit previous setting exists.
