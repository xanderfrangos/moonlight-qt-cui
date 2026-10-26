#pragma once

#include <QQuickImageProvider>

// Provides copies of local images with rounded corners, as
// image://rounded/<id>. The id is the source URL, base64url encoded so it
// survives being embedded in the image URL, followed by the width and height
// the image is drawn at and the corner radius at that size, each after a dot.
//
// The image keeps its original size so callers can still recognize it by its
// dimensions. Art of a different shape than the drawn size is rounded on its
// center crop, to be shown with Image.PreserveAspectCrop.
//
// Like BlurredImageProvider, this runs on the CPU so it works without shader
// effects on every Qt version we support.
class RoundedImageProvider : public QQuickImageProvider
{
public:
    RoundedImageProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};
