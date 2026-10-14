# GPU sharing: acceptance checks for Syphon and Spout

This change makes one-frame-delayed GPU sharing the default. Viewports allocate up to three render targets while the delayed path is actively sharing. The third target is **not** allocated if no Syphon/Spout receiver is being published or if immediate sharing is selected.

## What the unit test proves

The buffer-index test in the `rendering` area verifies that the next destination differs from both the frame just rendered and the previous frame handed to the GPU publisher. It checks repeated rotations and transitions back to immediate sharing.

It does **not** prove that a vendor publisher has completed its internal copy. `SyphonOpenGLServer.publishFrameTexture` is a publication API; `SpoutSender.SendTexture` sends/copies an OpenGL texture. If a specific SDK backend retains source data beyond method return, **neither a third buffer nor a fence solely in the render context is sufficient**. Use SDK-specific completion or copy into a lifetime-owned intermediate target in that case.

## Physical acceptance test required before merge

1. On macOS, connect an actual Syphon receiver; on Windows, connect an actual Spout receiver. Test on the drivers and GPU used for live work, not only llvmpipe.
2. Run 1080p and 4K at 30/60 fps, with two viewports and substantial GPU load.
3. Render a test pattern with a per-frame 24-bit ID, redundantly encoded in multiple distant patches, plus a border alternating colors on every frame. Capture decoded pixels **at the receiver**. For each captured frame, require identical IDs and border phase everywhere; enforce increasing IDs, allowing skipped IDs but not repetitions or backwards IDs (except when the receiver itself explicitly duplicates a frame).
4. Toggle immediate/delayed sharing, disconnect and reconnect receivers, resize viewports, and stress VRAM pressure. Compare render p95/p99, shares_skipped and visible artifacts.
5. Check Syphon/Spout SDK versions. If a publisher does not guarantee source ownership or completed submission after publish returns, add a publisher-owned staging copy with a verifiable completion strategy before releasing this PR.
6. Run a sustained live project on the actual show machine. Keep the PR draft if any receiver shows mixed, torn or stale frames.

## VRAM cost

A 3840x2160 RGBA16 texture is 63.3 MiB. Triple buffering is about 189.8 MiB per 4K viewport versus 126.6 MiB for double buffering, before multisample or other targets.

The input project's serialization is not changed. The machine-level setting defaults to delayed only when no explicit previous setting exists.
