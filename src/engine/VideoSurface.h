#pragma once
// Hardware frames sampled where they were decoded (macOS: VideoToolbox's IOSurfaces bound to rectangle
// textures). Render thread, its OpenGL context current. Elsewhere: not available.

#include "Gl.h"

struct AVFrame;

namespace videosurface {
// The current context can bind hardware surfaces to textures
bool available();
// Binds plane `plane` of the hardware frame `hw` to the rectangle texture `tex` (w × h texels of
// internal / format / type). False: the GPU refused it (the frame is then copied instead).
bool bindPlane(const AVFrame *hw, int plane, GLuint tex, GLenum internal, GLenum format, GLenum type, int w, int h);
} // namespace videosurface
