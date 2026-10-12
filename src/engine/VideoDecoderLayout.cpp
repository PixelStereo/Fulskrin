#include "VideoDecoder.h"
#include "VideoDecoderInternal.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/macros.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cmath>
#include <memory>

// Pixel-format layout construction and colorimetry are kept together. The delivery module
// calls videoDecoderYuvMatrix when its fallback needs to convert a frame to RGBA.
void videoDecoderYuvMatrix(AVColorSpace space, int height, float m[9], QString *name)
{
    double kr, kb;
    switch (space) {
    case AVCOL_SPC_BT709: kr = 0.2126, kb = 0.0722, *name = QStringLiteral("BT.709"); break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: kr = 0.299, kb = 0.114, *name = QStringLiteral("BT.601"); break;
    case AVCOL_SPC_FCC: kr = 0.30, kb = 0.11, *name = QStringLiteral("FCC"); break;
    case AVCOL_SPC_SMPTE240M: kr = 0.212, kb = 0.087, *name = QStringLiteral("SMPTE 240M"); break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: kr = 0.2627, kb = 0.0593, *name = QStringLiteral("BT.2020"); break;
    default: // unspecified: HD and above are BT.709, smaller pictures BT.601
        if (height >= 720) kr = 0.2126, kb = 0.0722, *name = QStringLiteral("BT.709");
        else kr = 0.299, kb = 0.114, *name = QStringLiteral("BT.601");
        break;
    }
    const double kg = 1.0 - kr - kb;
    const double r[9] = {1, 0, 2 * (1 - kr), 1, -2 * kb * (1 - kb) / kg, -2 * kr * (1 - kr) / kg, 1, 2 * (1 - kb), 0};
    for (int i = 0; i < 9; ++i) m[i] = float(r[i]);
}



std::shared_ptr<const VideoLayout> VideoDecoder::layoutFor(const AVFrame *fr)
{
    const LayoutKey key{fr->format, fr->width, fr->height, int(fr->colorspace), int(fr->color_range), int(fr->chroma_location)};
    const auto found = m_layouts.find(key);
    if (found != m_layouts.end()) return found->second;
    return m_layouts[key] = layoutOf(fr);
}



std::shared_ptr<const VideoLayout> VideoDecoder::layoutOf(const AVFrame *fr)
{
    std::shared_ptr<VideoLayout> L;

    const AVPixelFormat fmt = AVPixelFormat(fr->format);
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
    const uint64_t refused = AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL
                             | AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_FLOAT;
    if (!d || (d->flags & refused) || fmt == AV_PIX_FMT_XYZ12LE || fmt == AV_PIX_FMT_XYZ12BE || d->nb_components < 1
        || fr->width <= 0 || fr->height <= 0)
        return nullptr;
    const bool rgb = d->flags & AV_PIX_FMT_FLAG_RGB;
    const bool alpha = d->flags & AV_PIX_FMT_FLAG_ALPHA;
    const int nc = d->nb_components;
    const bool gray = !rgb && nc <= 2;

    struct PlaneInfo {
        bool used = false, chroma = false;
        int step = 0, bytes = 0;
    } pi[4];
    int planes = 0;
    for (int c = 0; c < nc; ++c) {
        const AVComponentDescriptor &cd = d->comp[c];
        const int bits = cd.depth + cd.shift;
        if (cd.plane < 0 || cd.plane > 3 || cd.depth <= 0 || bits > 16) return nullptr;
        const int bytes = bits > 8 ? 2 : 1;
        PlaneInfo &p = pi[cd.plane];
        if (p.used && (p.step != cd.step || p.bytes != bytes)) return nullptr; // packed 4:2:2 and the like
        if (cd.step <= 0 || cd.step % bytes || cd.offset % bytes || cd.step / bytes > 4) return nullptr;
        for (int o = 0; o < c; ++o) // two components in the same bits (RGB565…)
            if (d->comp[o].plane == cd.plane && d->comp[o].offset == cd.offset) return nullptr;
        p.used = true;
        p.step = cd.step;
        p.bytes = bytes;
        if (!rgb && !gray && (c == 1 || c == 2)) p.chroma = true;
        planes = std::max(planes, cd.plane + 1);
    }

    L = std::make_shared<VideoLayout>();
    L->width = fr->width;
    L->height = fr->height;
    L->planeCount = planes;
    L->alpha = alpha;
    L->mode = rgb ? VideoLayout::Mode::Rgb : gray ? VideoLayout::Mode::Gray : VideoLayout::Mode::Yuv;
    size_t offset = 0;
    for (int p = 0; p < planes; ++p) {
        if (!pi[p].used) return nullptr;
        VideoLayout::Plane &pl = L->plane[p];
        pl.width = pi[p].chroma ? AV_CEIL_RSHIFT(fr->width, d->log2_chroma_w) : fr->width;
        pl.height = pi[p].chroma ? AV_CEIL_RSHIFT(fr->height, d->log2_chroma_h) : fr->height;
        pl.channels = pi[p].step / pi[p].bytes;
        pl.texels = pi[p].bytes == 2 ? VideoLayout::Texels::U16 : VideoLayout::Texels::U8;
        pl.rowBytes = size_t(pl.width) * size_t(pi[p].step);
        pl.offset = offset;
        pl.size = pl.rowBytes * size_t(pl.height);
        offset += (pl.size + 63) & ~size_t(63);
    }
    L->totalBytes = offset;

    // Output channels: Y U V A / R G B A / grey and A
    auto use = [&](int out, int comp) {
        const AVComponentDescriptor &cd = d->comp[comp];
        const int bytes = pi[cd.plane].bytes;
        L->srcPlane[out] = cd.plane;
        L->srcChannel[out] = cd.offset / bytes;
        const double full = bytes == 2 ? 65535.0 : 255.0;
        L->scale[out] = float(full / double(((1 << cd.depth) - 1) << cd.shift));
    };
    if (gray) {
        use(0, 0);
        if (alpha && nc == 2) use(3, 1);
    } else {
        for (int c = 0; c < 3; ++c) use(c, c);
        if (alpha && nc >= 4) use(3, 3);
    }

    QString space;
    if (L->mode == VideoLayout::Mode::Yuv) {
        videoDecoderYuvMatrix(fr->colorspace, fr->height, L->matrix, &space);
        const int depth = d->comp[0].depth;
        if (depth < 8) return nullptr;
        const double max = double((1 << depth) - 1), k = double(1 << (depth - 8));
        const bool full = fr->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUVJ422P
                          || fmt == AV_PIX_FMT_YUVJ444P || fmt == AV_PIX_FMT_YUVJ440P || fmt == AV_PIX_FMT_YUVJ411P;
        L->cOffset = float(128.0 * k / max);
        if (full) {
            L->yOffset = 0;
            L->yScale = 1;
            L->cScale = 1;
        } else {
            L->yOffset = float(16.0 * k / max);
            L->yScale = float(max / (219.0 * k));
            L->cScale = float(max / (224.0 * k));
        }
        // Chroma siting: MPEG-2 and later put it on the left (co-sited) unless the stream says otherwise
        const AVChromaLocation loc = fr->chroma_location;
        const double sx = d->log2_chroma_w ? (double(1 << d->log2_chroma_w) - 1) / double(2 << d->log2_chroma_w) : 0;
        const double sy = d->log2_chroma_h ? (double(1 << d->log2_chroma_h) - 1) / double(2 << d->log2_chroma_h) : 0;
        const bool left = loc == AVCHROMA_LOC_LEFT || loc == AVCHROMA_LOC_TOPLEFT || loc == AVCHROMA_LOC_BOTTOMLEFT
                          || loc == AVCHROMA_LOC_UNSPECIFIED;
        L->chromaShift[0] = left ? float(sx) : 0.0f;
        L->chromaShift[1] = loc == AVCHROMA_LOC_TOPLEFT || loc == AVCHROMA_LOC_TOP           ? float(sy)
                            : loc == AVCHROMA_LOC_BOTTOMLEFT || loc == AVCHROMA_LOC_BOTTOM ? -float(sy)
                                                                                           : 0.0f;
        if (full) space += QStringLiteral(" full range");
    }
    L->description = QString::fromUtf8(d->name) + (space.isEmpty() ? QString() : QStringLiteral(" · ") + space);
    return L;
}


