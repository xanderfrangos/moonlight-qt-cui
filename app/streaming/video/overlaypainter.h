#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

#include <vector>

#include "SDL_compat.h"
#include "statsgraphs.h"

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

// Draws the stats history graphs as a Material card. Points run oldest to
// newest and are plotted against the right edge, so a history that hasn't
// filled the window yet still shows the newest sample in the same place.
// maxPoints is the number of samples a full window holds. The config picks
// which graphs appear and how tall they are, and the whole card is drawn at
// the given scale. A non-null streamInfo adds a summary of the stream above
// the graphs. Returns an ARGB8888 surface owned by the caller, or nullptr if
// there is nothing to draw or on failure.
SDL_Surface* paintStatsGraphs(const std::vector<StatsGraphPoint>& points,
                              int maxPoints,
                              int windowSeconds,
                              const StatsGraphConfig& config,
                              qreal scale,
                              const StatsGraphStreamInfo* streamInfo);

// Draws a solid color for overlays anchored with OverlayAnchorFill. The surface
// is deliberately tiny, since renderers stretch it over the whole viewport.
SDL_Surface* paintFill(QColor color);

}

}
