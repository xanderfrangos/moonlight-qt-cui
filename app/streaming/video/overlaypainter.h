#pragma once

#include <QColor>
#include <QSize>
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

// A button prompt along the bottom of the gamepad menu: the button's label
// drawn in a circle, followed by what the button does
struct ButtonHint {
    QString glyph;
    QColor color;
    QString text;
};

// A row of the gamepad menu
struct MenuItem {
    QString label;
    // Shown at the end of the row, such as the current value of a setting
    QString value;
    // Opens another page of the menu
    bool submenu = false;
    // Marks the choice in effect in a list of choices
    bool checked = false;
};

// What a page of the gamepad menu shows
struct Menu {
    // A short line in small capitals at the top
    QString title;
    // Under the title in a larger face, such as what is being streamed. It
    // may be empty.
    QString subtitle;
    QList<MenuItem> items;
    int selectedIndex = 0;
    // When there are more items than maxVisibleItems, the rows shown start
    // at firstVisibleIndex, with arrows for the rest
    int firstVisibleIndex = 0;
    int maxVisibleItems = 1000;
    // Labels at the start of each row with values at the end, rather than
    // centered labels
    bool alignRows = false;
    // Wrapped under the items, such as what a change will do. It may be empty.
    QString footnote;
    QList<ButtonHint> hints;
};

// Draws a page of the in-stream gamepad menu as a Material card. The layout
// scales with the viewport height so it reads the same at 720p and 4K. The
// card is as wide as the widest of all its items, so it keeps its size as it
// scrolls. Returns an ARGB8888 surface owned by the caller, or nullptr on
// failure.
SDL_Surface* paintGamepadMenu(const Menu& menu, int viewportHeight);

// Draws the stats history graphs as a Material card. Points run oldest to
// newest and are plotted against the right edge, so a history that hasn't
// filled the window yet still shows the newest sample in the same place.
// maxPoints is the number of samples a full window holds. The config picks
// which graphs appear and how they look. showStreamInfo adds a summary of the
// stream above the graphs; its frame rate also sets the frametime graphs'
// target line. A footer reports what the card itself costs to paint and
// upload. The card is drawn at the given scale, shrunk as needed to fit
// maxSize where that is set. Returns an ARGB8888 surface owned by the caller,
// or nullptr if there is nothing to draw or on failure.
SDL_Surface* paintStatsGraphs(const std::vector<StatsGraphPoint>& points,
                              int maxPoints,
                              const StatsGraphConfig& config,
                              const StatsGraphStreamInfo& streamInfo,
                              bool showStreamInfo,
                              const StatsGraphOverlayCost& cost,
                              qreal scale,
                              QSize maxSize);

// Draws a solid color for overlays anchored with OverlayAnchorFill. The surface
// is deliberately tiny, since renderers stretch it over the whole viewport.
SDL_Surface* paintFill(QColor color);

}

}
