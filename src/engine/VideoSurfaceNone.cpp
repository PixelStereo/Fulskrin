#include "VideoSurface.h"

namespace videosurface {
bool available() { return false; }
bool bindPlane(const AVFrame *, int, GLuint, GLenum, GLenum, GLenum, int, int) { return false; }
} // namespace videosurface
