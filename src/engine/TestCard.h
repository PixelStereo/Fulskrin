#pragma once
// A viewport's test card: what lines a projector up and checks an output. Drawn at the viewport's own size:
//  - its name and size, large, in the middle;
//  - a grid of squares, the diagonals and a circle (a circle stays round when the aspect is right);
//  - a frame on the outermost pixels (nothing cropped), the four corners numbered and colored
//    (1 red top left, 2 green top right, 3 blue bottom right, 4 yellow bottom left: a flipped or turned picture
//    shows at once);
//  - color bars and a grey ramp in steps;
//  - motion, so that a frozen output is seen: a sweep turning in the circle and a block going to and fro.
// Render thread, its context current.

#include "Gl.h"

#include <QElapsedTimer>
#include <QSize>
#include <QString>
#include <map>

class TestCard
{
public:
    // The card for a viewport named `name`, drawn into `target` (sized to `size`)
    void render(const QString &name, QSize size, RenderTarget &target, GLuint quadVao);
    void release();

    // The still part of the card, top row first (tests and tools)
    static QImage still(const QString &name, QSize size);

private:
    bool init();
    GLuint m_program = 0;
    GLint m_texLoc = -1, m_timeLoc = -1, m_sizeLoc = -1, m_circleLoc = -1, m_bandLoc = -1;
    std::map<QString, Texture2D> m_stills; // by name and size: drawn once, each viewport its own
    QElapsedTimer m_clock;
};
