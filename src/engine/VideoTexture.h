#pragma once
// A video layer's picture on the GPU (render thread).
// The decoder's frames arrive as they were decoded — YUV planes, RGB, grey, or HAP's compressed textures —
// in upload buffers (pixel buffer objects) the decoder wrote them into on its own thread. Their planes become
// textures, and one draw turns them into the RGBA picture the layer samples: YUV matrix and range, bit depth,
// chroma siting, scaled YCoCg, alpha, and the vertical flip (frames are stored top row first).
// On macOS, VideoToolbox's frames are not uploaded at all: their surfaces are bound to rectangle textures and
// converted from there (zero copy).

#include "Gl.h"
#include "VideoFrame.h"

#include <memory>
#include <vector>

class VideoDecoder;
struct AVFrame;

// The conversion program, shared by every video layer, and what the GPU can sample
class VideoConverter
{
public:
    bool init(GLuint quadVao, QString *err); // context current
    void release();
    bool s3tc() const { return m_s3tc; } // DXT1 / DXT5
    bool bptc() const { return m_bptc; } // BC7 / BC6H
    bool surfaces() const { return m_surfaces; } // hardware frames sampled in place (macOS)
    bool surfaceShader() const { return m_rect.id != 0; } // their program compiled (everywhere)

private:
    friend class VideoTexture;
    struct Program {
        GLuint id = 0;
        GLint tex[4] = {-1, -1, -1, -1}, sel[4] = {-1, -1, -1, -1}, off[4] = {-1, -1, -1, -1};
        GLint scale = -1, mode = -1, alpha = -1, matrix = -1, range = -1, crop = -1, clamp = -1;
        bool load(bool rect, QString *log);
    };
    Program m_plain, m_rect; // planes in 2D textures; hardware surfaces in rectangle textures
    GLuint m_quadVao = 0;
    bool m_s3tc = false, m_bptc = false, m_surfaces = false;
};

class VideoTexture
{
public:
    // Upload buffers of every video layer (any thread): bytes allocated, the budget they share, layers having some
    static size_t pboBytesInUse();
    static size_t pboBudgetBytes();
    static int pboStreams();
    VideoTexture() = default;
    ~VideoTexture() = default; // destroy() releases the GL objects, in the render thread
    VideoTexture(const VideoTexture &) = delete;
    VideoTexture &operator=(const VideoTexture &) = delete;

    // Each frame: the decoder gets as many upload buffers as it can use, of the size its frames need
    void feed(VideoDecoder &dec);
    // A frame fetched from the decoder becomes the picture. Its upload buffer goes back to the decoder.
    void upload(VideoFrame &f, VideoDecoder &dec, const VideoConverter &conv);

    GLuint texture() const { return m_out.tex; }
    int width() const { return m_out.w; }
    int height() const { return m_out.h; }
    uint64_t uploads() const { return m_uploads; }             // frames shown so far
    uint64_t stagedUploads() const { return m_staged; }        // of which through an upload buffer
    uint64_t surfaceUploads() const { return m_surfaced; }     // of which sampled in the decoder's surface
    // The picture's frame, unique among every video texture ever made (0: none yet): what was made from the
    // picture (its mipmaps) is still valid while this is unchanged
    uint64_t pictureSerial() const { return m_serial; }

    // Releases the textures and the upload buffers (render thread; the decoder must be closed already)
    void destroy();

private:
    struct Pbo {
        GLuint buffer = 0;
        size_t size = 0;
        bool mapped = false;
    };
    void createPbo(VideoDecoder &dec);
    void destroyPbo(int id);
    void ensurePlanes(const VideoLayout &L);
    void uploadPlane(int p, const VideoLayout::Plane &pl, const void *data, int rowPixels);
    void uploadFrom(const AVFrame *av, const VideoLayout &L); // planes read in place, rows repacked if need be
    bool bindSurface(const VideoFrame &fr, const VideoLayout &L); // false: refused by the GPU
    // False if allocation/format validation failed: keep the last complete GPU frame.
    bool convert(const VideoLayout &L, const VideoConverter &conv, bool surface);

    GLuint m_planes[4] = {0, 0, 0, 0};
    GLuint m_rect[4] = {0, 0, 0, 0}; // planes of a hardware surface (rectangle textures)
    AVFrame *m_held = nullptr;       // the frame whose surface is bound: kept until the next one
    VideoLayout::Plane m_planeShape[4]; // what each texture holds
    bool m_allocated[4] = {false, false, false, false};
    RenderTarget m_out;
    std::vector<Pbo> m_pbos; // by id; buffer 0: a free slot
    size_t m_pboSize = 0;
    int m_live = 0;
    int m_pboAllowed = 0; // its share of the upload buffers of every layer
    uint64_t m_uploads = 0, m_staged = 0, m_surfaced = 0, m_serial = 0;
    void shown(); // a new picture: counted, and a new serial
    std::vector<uint8_t> m_repack; // rows of a plane the GPU cannot read in place
};
