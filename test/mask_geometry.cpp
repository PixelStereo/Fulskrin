#include "Masks.h"
#include <cmath>
#include <QLineF>
#include <cstdio>

static QPointF cubic(const MaskNode &a, const MaskNode &b, double t)
{
    const double u = 1-t;
    return a.p*u*u*u + a.out*3*u*u*t + b.in*3*u*t*t + b.p*t*t*t;
}

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char *why) { if (!ok) { ++failures; std::printf("FAIL mask_geometry: %s\n", why); } };
    for (Mask original : {Mask::rectangle(QRectF(0,0,1,1)), Mask::ellipse(QRectF(.1,.2,.7,.5))}) {
        for (int segment = 0; segment < int(original.nodes.size()); ++segment) {
            Mask split = original;
            constexpr double at = .37;
            check(masks::splitSegment(split, segment, at), "split accepted");
            check(split.nodes.size() == original.nodes.size()+1, "one node inserted");
            const auto &a = original.nodes[size_t(segment)];
            const auto &b = original.nodes[(size_t(segment)+1)%original.nodes.size()];
            for (int step=0; step<=100; ++step) {
                const double t=step/100.0;
                const auto actual = t <= at ? cubic(split.nodes[size_t(segment)], split.nodes[size_t(segment)+1], t/at)
                    : cubic(split.nodes[size_t(segment)+1], split.nodes[(size_t(segment)+2)%split.nodes.size()], (t-at)/(1-at));
                check(QLineF(actual, cubic(a,b,t)).length() < 1e-12, "split preserves cubic including closing segment");
            }
            check(Mask::fromJson(split.toJson()) == split, "edited geometry survives project serialization");
        }
        Mask invalid=original;
        check(!masks::splitSegment(invalid,-1,.5) && !masks::splitSegment(invalid,0,0) && !masks::splitSegment(invalid,0,1), "invalid split refused");
        check(invalid==original, "invalid split leaves shape intact");
    }
    std::printf("mask_geometry: %d failures\n", failures);
    return failures ? 1 : 0;
}
