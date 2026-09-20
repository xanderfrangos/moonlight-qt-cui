#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

#include "SDL_compat.h"

namespace Overlay {

// Draws overlays with QPainter so the in-stream UI matches the Material look of
// the rest of the app. QML isn't an option here, because the Qt event loop isn't
// running while the SDL stream window owns the main thread.
namespace Painter {

// Draws the in-stream gamepad menu as a Material card. The layout scales with
// the viewport height so it reads the same at 720p and 4K. Returns an ARGB8888
// surface owned by the caller, or nullptr on failure.
SDL_Surface* paintGamepadMenu(const QString& title,
                              const QStringList& items,
                              int selectedIndex,
                              const QString& hint,
                              int viewportHeight);

// Draws a solid color for overlays anchored with OverlayAnchorFill. The surface
// is deliberately tiny, since renderers stretch it over the whole viewport.
SDL_Surface* paintFill(QColor color);

}

}
