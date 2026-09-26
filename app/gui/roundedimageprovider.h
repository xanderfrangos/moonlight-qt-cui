#pragma once

#include <QQuickImageProvider>

// Provides copies of local images with rounded corners, as
// image://rounded/<id>. The id is the source URL, base64url encoded so it
// survives being embedded in the image URL, followed by the width and height
// the image is drawn at and the corner radius at that size, each after a dot.
//
// The image keeps its original size so callers can still recognize it by its
// dimensions. The corners are scaled to match, so they come out round even
// when the image is stretched to a different aspect ratio.
//
// Like BlurredImageProvider, this runs on the CPU so it works without shader
// effects on every Qt version we support.
class RoundedImageProvider : public QQuickImageProvider
{
public:
    RoundedImageProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};
