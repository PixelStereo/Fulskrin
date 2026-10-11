#pragma once
// macOS privacy prompts, asked at startup so that a camera, an NDI or an OMT source works when it is dragged:
// the camera (Qt permission) and the local network (Bonjour browsing shows the prompt once). Nothing elsewhere.
void requestSystemPermissions();
