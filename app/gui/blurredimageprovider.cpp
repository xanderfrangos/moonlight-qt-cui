#include "blurredimageprovider.h"

#include <QImageReader>
#include <QUrl>

// The source is decoded at this width and blurred before it is scaled up,
// which removes detail cheaply
static const int k_ReadWidth = 72;

// The returned image is this many times larger than the decoded one. Blurring
// again at this size hides the steps left by scaling up.
static const int k_OutputScale = 4;

// One horizontal or vertical pass of a box blur with the given radius.
// Pixels past the edges repeat the nearest edge pixel.
static void boxBlurPass(QImage& image, int radius, bool horizontal)
{
    const int width = image.width();
    const int height = image.height();
    const int length = horizontal ? width : height;
    const int lines = horizontal ? height : width;
    const int window = radius * 2 + 1;

    // ARGB32 lines are exactly 4 bytes per pixel, so pixels can be indexed directly
    QRgb* pixels = static_cast<QRgb*>(static_cast<void*>(image.bits()));
    const int step = horizontal ? 1 : width;

    QVector<QRgb> line(length);

    for (int l = 0; l < lines; l++) {
        const int first = horizontal ? l * width : l;

        for (int i = 0; i < length; i++) {
            line[i] = pixels[first + i * step];
        }

        int sumA = 0, sumR = 0, sumG = 0, sumB = 0;
        for (int i = -radius; i <= radius; i++) {
            QRgb p = line[qBound(0, i, length - 1)];
            sumA += qAlpha(p);
            sumR += qRed(p);
            sumG += qGreen(p);
            sumB += qBlue(p);
        }

        for (int i = 0; i < length; i++) {
            pixels[first + i * step] = qRgba((sumR + window / 2) / window, (sumG + window / 2) / window,
                                             (sumB + window / 2) / window, (sumA + window / 2) / window);

            QRgb removed = line[qMax(i - radius, 0)];
            QRgb added = line[qMin(i + radius + 1, length - 1)];
            sumA += qAlpha(added) - qAlpha(removed);
            sumR += qRed(added) - qRed(removed);
            sumG += qGreen(added) - qGreen(removed);
            sumB += qBlue(added) - qBlue(removed);
        }
    }
}

// Three box blur passes in each direction closely approximate a Gaussian blur
static void blur(QImage& image, int radius)
{
    for (int i = 0; i < 3; i++) {
        boxBlurPass(image, radius, true);
        boxBlurPass(image, radius, false);
    }
}

BlurredImageProvider::BlurredImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading)
{
}

QImage BlurredImageProvider::requestImage(const QString& id, QSize* size, const QSize&)
{
    QUrl url(QString::fromUtf8(QByteArray::fromBase64(id.toLatin1(),
                                                      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));

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

    QImage image = reader.read();
    if (image.isNull()) {
        return QImage();
    }

    if (image.width() > k_ReadWidth) {
        image = image.scaledToWidth(k_ReadWidth, Qt::SmoothTransformation);
    }
    image = image.convertToFormat(QImage::Format_ARGB32);

    blur(image, 3);
    image = image.scaled(image.size() * k_OutputScale, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                 .convertToFormat(QImage::Format_ARGB32);
    blur(image, k_OutputScale * 2);

    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}
