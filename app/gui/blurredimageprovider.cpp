#include "blurredimageprovider.h"
#include "dither.h"

#include <QImageReader>
#include <QStringList>
#include <QUrl>
#include <QVector>

// The source is decoded at this width before it is blurred, which removes
// detail cheaply
static const int k_ReadWidth = 72;

// The blur runs again at this many times the decoded width. Everything up to
// the final write stays in floating point, so this is only about how far the
// second blur reaches, not about hiding steps left by scaling up.
static const int k_BlurScale = 4;

// Longest edge of the image handed back to the scene graph. The GPU still
// stretches it the rest of the way, but from something already smooth and
// already dithered rather than from a thumbnail.
static const int k_MaxOutputEdge = 2560;

// Dither amplitude, in levels. The result is opaque and drawn as-is, so one
// level is the textbook value and nothing has to be scaled to compensate.
static const float k_DitherAmplitude = 1.0f;

// The id may carry the rest of the background after the art: the opacity the
// art is faded back to, the window gradient it sits on, and the gradient that
// darkens it. When it does, all of that is folded in here rather than being
// stacked as separate translucent layers.
//
// Stacking them is what kept the backdrop banding. Every layer the scene graph
// composites rounds to 8 bits, so three layers meant three roundings, and noise
// mixed into the bottom one arrives at the top scaled down by the opacities of
// everything above it. Folding them together leaves exactly one rounding, with
// the dither applied right where it happens.
struct Backdrop
{
    bool valid = false;
    float opacity = 1.0f;
    float windowTop[4], windowBottom[4];
    float dimTop[4], dimBottom[4];
};

static void unpackColor(uint packed, float* out)
{
    for (int c = 0; c < 4; c++) {
        out[c] = (packed >> ((3 - c) * 8)) & 0xFF;
    }
}

// An image held as float channels, so none of the intermediate steps round to
// 8 bits. Rounding every pass, as an 8-bit buffer does, leaves plateaus that
// the final upscale then widens into bands.
struct FloatImage
{
    int width = 0;
    int height = 0;
    QVector<float> data; // Four interleaved channels per pixel

    FloatImage() = default;
    FloatImage(int w, int h) : width(w), height(h), data(w * h * 4, 0.0f) {}

    float* pixel(int x, int y) { return data.data() + (y * width + x) * 4; }
    const float* pixel(int x, int y) const { return data.data() + (y * width + x) * 4; }
};

// One horizontal or vertical pass of a box blur with the given radius.
// Pixels past the edges repeat the nearest edge pixel.
static void boxBlurPass(FloatImage& image, int radius, bool horizontal)
{
    const int length = horizontal ? image.width : image.height;
    const int lines = horizontal ? image.height : image.width;
    const int window = radius * 2 + 1;
    const int step = (horizontal ? 1 : image.width) * 4;

    QVector<float> line(length * 4);

    for (int l = 0; l < lines; l++) {
        float* first = horizontal ? image.pixel(0, l) : image.pixel(l, 0);

        for (int i = 0; i < length; i++) {
            for (int c = 0; c < 4; c++) {
                line[i * 4 + c] = first[i * step + c];
            }
        }

        float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int i = -radius; i <= radius; i++) {
            const int clamped = qBound(0, i, length - 1);
            for (int c = 0; c < 4; c++) {
                sum[c] += line[clamped * 4 + c];
            }
        }

        for (int i = 0; i < length; i++) {
            const int removed = qMax(i - radius, 0);
            const int added = qMin(i + radius + 1, length - 1);

            for (int c = 0; c < 4; c++) {
                first[i * step + c] = sum[c] / window;
                sum[c] += line[added * 4 + c] - line[removed * 4 + c];
            }
        }
    }
}

// Three box blur passes in each direction closely approximate a Gaussian blur
static void blur(FloatImage& image, int radius)
{
    for (int i = 0; i < 3; i++) {
        boxBlurPass(image, radius, true);
        boxBlurPass(image, radius, false);
    }
}

static FloatImage toFloat(const QImage& source)
{
    const QImage argb = source.convertToFormat(QImage::Format_ARGB32);

    FloatImage result(argb.width(), argb.height());
    for (int y = 0; y < argb.height(); y++) {
        const QRgb* line = reinterpret_cast<const QRgb*>(argb.constScanLine(y));

        for (int x = 0; x < argb.width(); x++) {
            float* out = result.pixel(x, y);
            out[0] = qRed(line[x]);
            out[1] = qGreen(line[x]);
            out[2] = qBlue(line[x]);
            out[3] = qAlpha(line[x]);
        }
    }

    return result;
}

// Bilinear resample that scales to cover the target and drops what hangs over,
// which is what the backdrop does with the result anyway. Cropping here means
// we never build the parts that get thrown away.
static FloatImage resampleCover(const FloatImage& source, int width, int height)
{
    const float scale = qMax(float(width) / source.width, float(height) / source.height);
    const float left = (source.width - width / scale) / 2.0f;
    const float top = (source.height - height / scale) / 2.0f;

    FloatImage result(width, height);
    for (int y = 0; y < height; y++) {
        const float sy = qBound(0.0f, top + (y + 0.5f) / scale - 0.5f, source.height - 1.0f);
        const int y0 = int(sy);
        const int y1 = qMin(y0 + 1, source.height - 1);
        const float fy = sy - y0;

        for (int x = 0; x < width; x++) {
            const float sx = qBound(0.0f, left + (x + 0.5f) / scale - 0.5f, source.width - 1.0f);
            const int x0 = int(sx);
            const int x1 = qMin(x0 + 1, source.width - 1);
            const float fx = sx - x0;

            const float* upperLeft = source.pixel(x0, y0);
            const float* upperRight = source.pixel(x1, y0);
            const float* lowerLeft = source.pixel(x0, y1);
            const float* lowerRight = source.pixel(x1, y1);
            float* out = result.pixel(x, y);

            for (int c = 0; c < 4; c++) {
                const float upper = upperLeft[c] + (upperRight[c] - upperLeft[c]) * fx;
                const float lower = lowerLeft[c] + (lowerRight[c] - lowerLeft[c]) * fx;
                out[c] = upper + (lower - upper) * fy;
            }
        }
    }

    return result;
}

// The one place the pipeline rounds to 8 bits, so the one place that dithers
static QImage toImage(const FloatImage& source, const Backdrop& backdrop)
{
    QImage result(source.width, source.height, QImage::Format_ARGB32);

    for (int y = 0; y < source.height; y++) {
        QRgb* line = reinterpret_cast<QRgb*>(result.scanLine(y));

        const float position = source.height > 1 ? float(y) / (source.height - 1) : 0.0f;
        float window[4], dim[4];
        if (backdrop.valid) {
            for (int c = 0; c < 4; c++) {
                window[c] = backdrop.windowTop[c] + (backdrop.windowBottom[c] - backdrop.windowTop[c]) * position;
                dim[c] = backdrop.dimTop[c] + (backdrop.dimBottom[c] - backdrop.dimTop[c]) * position;
            }
        }

        for (int x = 0; x < source.width; x++) {
            const float* in = source.pixel(x, y);

            float value[4];
            for (int c = 0; c < 4; c++) {
                value[c] = in[c];
            }

            if (backdrop.valid) {
                const float dimAlpha = dim[0] / 255.0f;
                for (int c = 1; c < 4; c++) {
                    // The art faded back onto the window gradient, then darkened
                    const float overWindow = window[c] * (1.0f - backdrop.opacity) + in[c - 1] * backdrop.opacity;
                    value[c] = overWindow * (1.0f - dimAlpha) + dim[c] * dimAlpha;
                }
                value[0] = 255.0f;
            }
            else {
                // Without the rest of the background, hand back just the art.
                // Alpha is left undithered either way: box art is opaque, and
                // dithering a transparent edge would only make it speckle.
                value[0] = in[3];
                value[1] = in[0];
                value[2] = in[1];
                value[3] = in[2];
            }

            line[x] = qRgba(Dither::quantize(value[1], x, y, k_DitherAmplitude),
                            Dither::quantize(value[2], x, y, k_DitherAmplitude),
                            Dither::quantize(value[3], x, y, k_DitherAmplitude),
                            qBound(0, int(value[0] + 0.5f), 255));
        }
    }

    return result;
}

BlurredImageProvider::BlurredImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading)
{
}

QImage BlurredImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    // The art URL comes first. Base64url never produces a dot, so the rest of
    // the background can be appended after one.
    const QStringList fields = id.split(QLatin1Char('.'));

    QUrl url(QString::fromUtf8(QByteArray::fromBase64(fields.at(0).toLatin1(),
                                                      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));

    Backdrop backdrop;
    if (fields.count() == 6) {
        bool parsed[5];
        const uint opacity = fields.at(1).toUInt(&parsed[0], 16);
        const uint windowTop = fields.at(2).toUInt(&parsed[1], 16);
        const uint windowBottom = fields.at(3).toUInt(&parsed[2], 16);
        const uint dimTop = fields.at(4).toUInt(&parsed[3], 16);
        const uint dimBottom = fields.at(5).toUInt(&parsed[4], 16);

        backdrop.valid = parsed[0] && parsed[1] && parsed[2] && parsed[3] && parsed[4];
        if (backdrop.valid) {
            backdrop.opacity = qBound(0u, opacity, 255u) / 255.0f;
            unpackColor(windowTop, backdrop.windowTop);
            unpackColor(windowBottom, backdrop.windowBottom);
            unpackColor(dimTop, backdrop.dimTop);
            unpackColor(dimBottom, backdrop.dimBottom);
        }
    }

    // Only local images are supported, which includes all cached box art
    QString path;
    if (url.scheme() == QLatin1String("qrc")) {
        path = QLatin1Char(':') + url.path();
    }
    else if (url.isLocalFile()) {
        path = url.toLocalFile();
    }
    else {
        return QImage();
    }

    QImageReader reader(path);
    QSize sourceSize = reader.size();
    if (sourceSize.isValid() && sourceSize.width() > k_ReadWidth) {
        // Many formats can decode directly at a smaller size
        reader.setScaledSize(QSize(k_ReadWidth, qMax(1, sourceSize.height() * k_ReadWidth / sourceSize.width())));
    }

    QImage decoded = reader.read();
    if (decoded.isNull()) {
        return QImage();
    }

    if (decoded.width() > k_ReadWidth) {
        decoded = decoded.scaledToWidth(k_ReadWidth, Qt::SmoothTransformation);
    }

    FloatImage image = toFloat(decoded);
    blur(image, 3);
    image = resampleCover(image, image.width * k_BlurScale, image.height * k_BlurScale);
    blur(image, k_BlurScale * 2);

    // Fill the size the backdrop asked for, within reason. Without a request,
    // fall back to the blurred size and let the scene graph stretch it.
    QSize target = requestedSize;
    if (target.width() > 0 && target.height() > 0) {
        if (qMax(target.width(), target.height()) > k_MaxOutputEdge) {
            target.scale(k_MaxOutputEdge, k_MaxOutputEdge, Qt::KeepAspectRatio);
        }
        image = resampleCover(image, target.width(), target.height());
    }

    QImage result = toImage(image, backdrop);
    if (size != nullptr) {
        *size = result.size();
    }
    return result;
}
