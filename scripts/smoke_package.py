#!/usr/bin/env python3
"""Smoke test of a packaged Pinol, run from the extracted archive on the CI runner (not the build tree).

Renders a small project headless (--render --strict): an ISF generator from the package's own isf folder, an image
and a video decoded by the package's FFmpeg. Fails if the program cannot start (a library missing), the project does
not load, a layer or an effect is in error, or no picture is written.

usage: smoke_package.py <Pinol executable> <isf folder> <test media folder> <work folder> [--allow-no-gl]
  --allow-no-gl: exit code 1 (OpenGL unavailable on this runner) is reported but not a failure — the program did start,
                 so its libraries were found.
"""
import json
import os
import subprocess
import sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    allow_no_gl = '--allow-no-gl' in sys.argv
    if len(args) != 4:
        print(__doc__)
        return 2
    exe, isf, media, work = (os.path.abspath(a) for a in args)
    os.makedirs(work, exist_ok=True)
    gen = os.path.join(isf, 'generators', 'TestPattern.fs')
    for path in (exe, gen, os.path.join(media, 'h264.mp4'), os.path.join(media, 'bars.png')):
        if not os.path.exists(path):
            print(f'missing: {path}')
            return 1
    full = {"corners": [[0, 0], [1, 0], [1, 1], [0, 1]], "cols": 4, "rows": 4, "offsets": [[0, 0]] * 16}
    layer = lambda name, source: {"name": name, "enable": True, "opacity": 1.0, "blend_mode": "normal",
                                  "source": source, "fx": [], "spatial": full}
    project = {"app": "Pinol", "format_version": 1, "composition": {"width": 320, "height": 180}, "layers": [
        layer("Pattern", {"type": "isf", "path": gen, "width": 320, "height": 180}),
        layer("Bars", {"type": "image", "path": os.path.join(media, 'bars.png')}),
        layer("Video", {"type": "video", "path": os.path.join(media, 'h264.mp4'), "loop": True, "speed": 1,
                        "playing": True}),
    ]}
    proj = os.path.join(work, 'smoke.pinol')
    with open(proj, 'w') as f:
        json.dump(project, f, indent=1)
    out = os.path.join(work, 'smoke.png')
    if os.path.exists(out):
        os.remove(out)
    cmd = [exe, '--render', out, '--frames', '10', '--strict', proj]
    print(' '.join(cmd), flush=True)
    try:
        rc = subprocess.run(cmd, timeout=180).returncode
    except subprocess.TimeoutExpired:
        print('FAIL: the packaged program did not finish within 3 minutes')
        return 1
    meaning = {0: 'rendered', 1: 'OpenGL unavailable', 2: 'output not written', 3: 'project not loaded',
               4: 'a layer or an effect in error'}
    print(f'exit code {rc}: {meaning.get(rc, "did not start or crashed (a library missing?)")}')
    if rc == 1 and allow_no_gl:
        print('::warning::no OpenGL on this runner: the packaged program started (its libraries were found), '
              'nothing rendered')
        return 0
    if rc != 0:
        return 1
    if not os.path.exists(out) or os.path.getsize(out) < 100:
        print('FAIL: no picture written')
        return 1
    print(f'OK: {out} ({os.path.getsize(out)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
