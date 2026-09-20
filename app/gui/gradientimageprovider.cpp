#include "gradientimageprovider.h"
#include "dither.h"

#include <QStringList>

// Width of the strip that gets tiled across the window. The noise doesn't
// repeat within it, but the strip itself does, so this is a compromise between
// the memory a full width texture would cost and how far apart the repeats sit.
// At the amplitude used here the noise is about one level, which is too faint
// for the repeat to register.
static const int k_StripWidth = 256;

// How much noise to mix into the color channels, in levels of the finished
// screen. A translucent gradient only contributes its own color in proportion
// to its alpha, so the noise it can inject into the blend underneath is scaled
// down by the same amount, and the amplitude is divided by alpha to compensate.
// This is what keeps the bottom of the backdrop dithered, where the darkening
// is strongest and the art underneath is contributing almost nothing.
static const float k_DitherAmplitude = 1.0f;
static const float k_MaxDitherAmplitude = 8.0f;

GradientImageProvider::GradientImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage GradientImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    const QStringList colors = id.split(QLatin1Char('-'));
    if (colors.count() != 2) {
        return QImage();
    }

    bool topParsed, bottomParsed;
    const uint top = colors.at(0).toUInt(&topParsed, 16);
    const uint bottom = colors.at(1).toUInt(&bottomParsed, 16);
    if (!topParsed || !bottomParsed) {
        return QImage();
    }

    // Channels in ARGB order, to match the hex the id is written in
    float from[4], to[4];
    for (int c = 0; c < 4; c++) {
        const int shift = (3 - c) * 8;
        from[c] = (top >> shift) & 0xFF;
        to[c] = (bottom >> shift) & 0xFF;
    }

    // Alpha is dithered only when it actually varies. Perturbing a gradient
    // that is meant to be fully opaque would leave it faintly see-through in
    // scattered pixels, which reads as dirt rather than as smoothness.
    const bool ditherAlpha = from[0] != to[0];

    const int height = qMax(1, requestedSize.height());
    const int width = requestedSize.width() > 0 ? requestedSize.width() : k_StripWidth;
    QImage image(width, height, QImage::Format_ARGB32);

    for (int y = 0; y < height; y++) {
        const float position = height > 1 ? float(y) / (height - 1) : 0.0f;

        float value[4];
        for (int c = 0; c < 4; c++) {
            value[c] = from[c] + (to[c] - from[c]) * position;
        }

        const float alpha = value[0] / 255.0f;
        const float colorAmplitude = qBound(k_DitherAmplitude,
                                            k_DitherAmplitude / qMax(alpha, 0.01f),
                                            k_MaxDitherAmplitude);

        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; x++) {
            line[x] = qRgba(Dither::quantize(value[1], x, y, colorAmplitude),
                            Dither::quantize(value[2], x, y, colorAmplitude),
                            Dither::quantize(value[3], x, y, colorAmplitude),
                            ditherAlpha ? Dither::quantize(value[0], x, y, k_DitherAmplitude)
                                        : qBound(0, int(value[0] + 0.5f), 255));
        }
    }

    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}
