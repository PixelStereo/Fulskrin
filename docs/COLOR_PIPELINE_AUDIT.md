# PINOL color-pipeline audit: preserving gradients (October 2026)

## Objective

Retain at least 10-bit precision **inside the render engine** when a GLSL/ISF
shader generates a gradient. Standard render targets now default to
**GL_RGBA16 normalized** (Settings ▸ Rendering ▸ Internal precision; 8 bits stay
available as a lighter choice for heavy shows) (16 bits *per channel*, i.e. four unsigned normalized
channels). The fractional output of the shader is now quantized to 65,536
nominal levels rather than the 256 levels of GL_RGBA8. Shader calculations
remain floating-point. ISF persistent FLOAT targets remain RGBA32F.

A real GL regression test draws a 1024-step grayscale ramp into an RGBA16
framebuffer and reads it back as unsigned 16-bit samples. More than 1000
monotonically distinct levels must survive, on each CI GPU/driver.

This is **not** a claim of end-to-end 10-bit output.

## Current pipeline, with actual limits

| Stage | Precision / limitation |
|---|---|
| ISF shader math | GLSL floating-point; output formerly RGBA8 by default |
| Composition, groups, viewports, transitions, effects | RGBA16 UNORM by default (8-bit on request); HDR values outside 0..1 cannot be represented |
| Ordinary FFmpeg YUV 10/12/16-bit input | Plane textures can be GL_R16/RG16 etc.; shader YUV→RGB conversion, then RGBA16 UNORM |
| Some FFmpeg formats | CPU fallback via swscale can become RGBA8; not an unconditional >8-bit guarantee |
| HAP video | DXT/BC compressed GPU textures, not a uniformly 10-bit source; compressed texture precision depends on format |
| HAP HDR BC6 fallback | A CPU fallback converts high-dynamic-range float values to 8-bit and clips; not HDR-safe |
| PNG/TIFF/still image | Currently converted to QImage::Format_RGBA8888 before GL upload; 16-bit files **lose depth on import** |
| Text and rasterized masks | QImage RGBA8 sources; smooth shaders may render into RGBA16, but 8-bit source detail is not restored |
| Preview, QImage grabs, thumbnails | GL_UNSIGNED_BYTE / RGBA8 readback; their 8-bit banding does not prove the actual GPU composition was 8-bit |
| CPU publishers / capture | glReadPixels GL_BGRA GL_UNSIGNED_BYTE; CPU network/output may be 8-bit |
| Syphon/Spout GPU shares / displays | The downstream app, shared-surface format, compositor and monitor/projector may quantize to 8-bit |
| GPU present / physical output | 10-bit output is **not negotiated or verified** in this PR; do not claim it |

## Color management is a distinct issue

This engine currently converts YUV according to declared/heuristic
coefficients and a full/limited range, but its conversion shader clamps RGB
to [0,1]. It does **not** implement a fully managed linear-light working
space, gamut conversion with ICC profiles, or PQ/HLG HDR tone mapping.
GL_RGBA16 UNORM preserves fine SDR shade spacing; it does not make blending
colorimetrically linear or provide HDR headroom.

The FFmpeg probe/inspector metadata feature (separate PR) labels **source
tags** (matrix, primaries, transfer, range, bit depth). A source tag must not
be confused with the color space actually driven by the projector.

## Performance and verification

RGBA16 consumes **8 bytes per pixel**, double RGBA8's **4 bytes per pixel**,
before mipmaps, antialiasing and multiple effect passes. Monitor GPU memory,
frame-time p95/p99 and maximum simultaneous layers at 1080p/4K/8K.

The cross-platform GPU test covers precision before readback. Visual 10-bit
output must be verified **on a display/output interface that truly supports
10-bit input** with its driver configuration and the destination application's
output path. A conventional 8-bit monitor will still display about 256
discrete shades per channel unless spatial/temporal dithering is applied.

Next work, separate reviewable PRs:
1. Preserve source images' 16-bit samples through GL upload when possible.
2. Negotiate/test true 10-bit hardware and network output pixel formats.
3. Optional final 8-bit output dithering, tested against reference gradients.
4. Full SDR working-space/color-transform calibration, then HDR only if needed.
