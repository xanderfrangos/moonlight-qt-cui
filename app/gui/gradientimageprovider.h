#pragma once

#include <QQuickImageProvider>

// Provides dithered vertical gradients, as image://tvgradient/<top>-<bottom>,
// where each color is eight hex digits of ARGB. The height comes from the
// requested size; the result is a narrow strip meant to be tiled across.
//
// This exists because a QML Gradient bands badly at the sizes TV mode uses it.
// Its stops are only a dozen or two levels apart, so spread over a whole window
// height each level covers sixty or more rows, and the steps between them read
// as hard lines. Building the ramp here lets it be dithered as it is rounded to
// 8 bits, which is the only point at which the banding can actually be removed.
class GradientImageProvider : public QQuickImageProvider
{
public:
    GradientImageProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};
