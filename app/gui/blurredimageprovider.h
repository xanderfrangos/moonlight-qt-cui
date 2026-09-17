#pragma once

#include <QQuickImageProvider>

// Provides heavily blurred copies of local images for TV mode backdrops, as
// image://blurred/<id>. The id is the source URL, base64url encoded so it
// survives being embedded in the image URL.
//
// The blur is done on the CPU because it has to work without shader effects
// on every Qt version we support. The result is small but smooth enough to
// be scaled up to fill the window without visible pixels.
class BlurredImageProvider : public QQuickImageProvider
{
public:
    BlurredImageProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};
