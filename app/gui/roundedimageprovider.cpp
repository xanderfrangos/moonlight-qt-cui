#include "roundedimageprovider.h"

#include <QImageReader>
#include <QPainter>
#include <QStringList>
#include <QUrl>

RoundedImageProvider::RoundedImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage RoundedImageProvider::requestImage(const QString& id, QSize* size, const QSize& /* requestedSize */)
{
    // Base64url never produces a dot, so the drawn size and radius follow one
    const QStringList fields = id.split(QLatin1Char('.'));
    if (fields.count() != 4) {
        return QImage();
    }

    QUrl url(QString::fromUtf8(QByteArray::fromBase64(fields.at(0).toLatin1(),
                                                      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));

    bool parsed[3];
    const qreal drawnWidth = fields.at(1).toDouble(&parsed[0]);
    const qreal drawnHeight = fields.at(2).toDouble(&parsed[1]);
    const qreal radius = fields.at(3).toDouble(&parsed[2]);
    if (!parsed[0] || !parsed[1] || !parsed[2] || drawnWidth <= 0 || drawnHeight <= 0) {
        return QImage();
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

    QImage source = QImageReader(path).read();
    if (source.isNull()) {
        return QImage();
    }

    QImage result(source.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    // The image is shown cropped to the drawn shape from its center, so only
    // that part gets rounded. The rest stays transparent and is never shown.
    const qreal scale = qMin(source.width() / drawnWidth, source.height() / drawnHeight);
    const qreal cropWidth = drawnWidth * scale;
    const qreal cropHeight = drawnHeight * scale;
    const QRectF crop((source.width() - cropWidth) / 2, (source.height() - cropHeight) / 2,
                      cropWidth, cropHeight);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(source));
    // The radius is given at the drawn size
    painter.drawRoundedRect(crop, radius * scale, radius * scale);
    painter.end();

    if (size != nullptr) {
        *size = result.size();
    }
    return result;
}
