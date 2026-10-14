# PINOL golden-render regression fixtures

These fixtures are **analytical reference results**, written independently of the GPU renderer.
They are not snapshots silently regenerated from the implementation under test.

Run `ctest --test-dir build -R '^golden$' --output-on-failure` after building `pinol_tests`.
Each scene renders into an exact 128 × 64 viewport and compares **every RGB pixel inside each approved interior rectangle**, avoiding texture boundaries and anti-aliased edges. The current fixtures validate more than ten thousand pixels per run.

- `split`: opaque red and blue halves of an image.
- `flip`: same image with the stock ISF FlipCrop filter's horizontal flip enabled.
- `alpha`: normal source-over composition of a 50% red PNG over opaque blue, evaluated using straight-alpha GPU blending.
- `solid_isf`: a SolidColor ISF generator with input color (0.2, 0.6, 0.8, 1.0).

The `rect` fields are half-open `[left, top, right, bottom]` coordinates. RGB values are 8-bit SDR **display-coded** expectations. Per-scene tolerances only absorb GPU quantization, not large visual changes.

When an assertion fails, the test saves a PNG beside the test's temporary files. A developer must inspect the image difference, then approve any intentional change by updating the fixture in a reviewed PR. Do not auto-accept new captures.

Future fixtures should cover masks, mesh warps, viewport transforms, color correction, transitions, and video codecs. Those require separately validated reference results, not inferred snapshots.
