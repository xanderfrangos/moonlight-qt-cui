#include "overlaypainter.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QRectF>

#include <cmath>

using namespace Overlay;

namespace {

// Matches the app's Material dark palette. See TvTheme.qml and the Material
// defaults that main.cpp installs.
const QColor k_SurfaceColor(0x1F, 0x23, 0x30, 0xF2);
const QColor k_SurfaceEdgeColor(0xFF, 0xFF, 0xFF, 0x1A);
const QColor k_TitleColor(0x9A, 0xA0, 0xAE);
const QColor k_ItemColor(0xE8, 0xEA, 0xF0);
const QColor k_SelectedItemColor(0xFF, 0xFF, 0xFF);

// Material Purple, which main.cpp uses as the accent unless the user overrides it
const QColor k_DefaultAccentColor(0x9C, 0x27, 0xB0);

QColor accentColor()
{
    // Follow the accent the user picked for the rest of the app when we can parse
    // it. Material's named colors line up with SVG color names closely enough.
    QColor accent(qEnvironmentVariable("QT_QUICK_CONTROLS_MATERIAL_ACCENT"));
    return accent.isValid() ? accent : k_DefaultAccentColor;
}

QFont menuFont(qreal pixelSize, QFont::Weight weight)
{
    QFont font = QGuiApplication::font();
    font.setPixelSize(qMax(1, qRound(pixelSize)));
    font.setWeight(weight);
    return font;
}

// Renderers require ARGB8888, which has the same byte layout as QImage's
// non-premultiplied ARGB32 on every platform we support.
SDL_Surface* imageToSurface(const QImage& image)
{
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, image.width(), image.height(),
                                                          32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to allocate overlay surface: %s",
                     SDL_GetError());
        return nullptr;
    }

    for (int y = 0; y < image.height(); y++) {
        SDL_memcpy((uint8_t*)surface->pixels + (y * surface->pitch),
                   image.constScanLine(y),
                   image.width() * 4);
    }

    return surface;
}

// A soft shadow that lifts the card off the video without needing a blur pass
void drawCardShadow(QPainter& painter, const QRectF& cardRect, qreal radius, qreal spread)
{
    painter.setPen(Qt::NoPen);

    for (qreal i = spread; i >= 1; i--) {
        const qreal falloff = 1.0 - (i / spread);
        painter.setBrush(QColor(0, 0, 0, qRound(60.0 * falloff * falloff)));
        painter.drawRoundedRect(cardRect.adjusted(-i, -i * 0.6, i, i * 1.4),
                                radius + i, radius + i);
    }
}

// The same focus treatment the QML views use: an accent ring with a soft glow
// around it, over a tinted fill.
void drawSelection(QPainter& painter, const QRectF& itemRect, qreal radius, qreal scale)
{
    QColor accent = accentColor();

    QColor fill = accent;
    fill.setAlpha(0x38);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(itemRect, radius, radius);

    QColor glow = accent;
    glow.setAlphaF(0.35f);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(glow, 4 * scale));
    painter.drawRoundedRect(itemRect.adjusted(-4 * scale, -4 * scale, 4 * scale, 4 * scale),
                            radius + 4 * scale, radius + 4 * scale);

    painter.setPen(QPen(accent, 3 * scale));
    painter.drawRoundedRect(itemRect, radius, radius);
}

}

SDL_Surface* Painter::paintGamepadMenu(const QString& title,
                                       const QStringList& items,
                                       int selectedIndex,
                                       const QString& hint,
                                       int viewportHeight)
{
    if (items.isEmpty()) {
        return nullptr;
    }

    // Everything below is authored against 1080p and scaled from there
    const qreal scale = qBound(0.75, viewportHeight / 1080.0, 3.0);

    const qreal cardRadius = 16 * scale;
    const qreal itemRadius = 8 * scale;
    const qreal cardPadding = 28 * scale;
    const qreal itemPaddingX = 20 * scale;
    const qreal itemHeight = 52 * scale;
    const qreal itemSpacing = 6 * scale;
    const qreal titleGap = 20 * scale;
    const qreal hintGap = 22 * scale;
    const qreal shadowSpread = 24 * scale;

    QFont titleFont = menuFont(15 * scale, QFont::DemiBold);
    titleFont.setCapitalization(QFont::AllUppercase);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * scale);
    QFont itemFont = menuFont(21 * scale, QFont::Medium);
    QFont hintFont = menuFont(14 * scale, QFont::Normal);

    QFontMetricsF titleMetrics(titleFont);
    QFontMetricsF itemMetrics(itemFont);
    QFontMetricsF hintMetrics(hintFont);

    // The selection ring is drawn outside the item rect, so the items need to sit
    // far enough inside the card to leave room for it.
    const qreal selectionMargin = 6 * scale;

    qreal contentWidth = qMax(titleMetrics.horizontalAdvance(title),
                              hint.isEmpty() ? 0.0 : hintMetrics.horizontalAdvance(hint));
    for (const QString& item : items) {
        contentWidth = qMax(contentWidth, itemMetrics.horizontalAdvance(item) + (itemPaddingX * 2));
    }

    const qreal cardWidth = qMax(contentWidth + (cardPadding * 2) + (selectionMargin * 2),
                                 420 * scale);

    qreal cardHeight = (cardPadding * 2) + selectionMargin +
                       titleMetrics.height() + titleGap +
                       (itemHeight * items.size()) + (itemSpacing * (items.size() - 1));
    if (!hint.isEmpty()) {
        cardHeight += hintGap + hintMetrics.height();
    }

    QImage image(qRound(cardWidth + (shadowSpread * 2)),
                 qRound(cardHeight + (shadowSpread * 2)),
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return nullptr;
    }
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF cardRect(shadowSpread, shadowSpread, cardWidth, cardHeight);

    drawCardShadow(painter, cardRect, cardRadius, shadowSpread);

    painter.setPen(Qt::NoPen);
    painter.setBrush(k_SurfaceColor);
    painter.drawRoundedRect(cardRect, cardRadius, cardRadius);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(k_SurfaceEdgeColor, 1));
    painter.drawRoundedRect(cardRect.adjusted(0.5, 0.5, -0.5, -0.5), cardRadius, cardRadius);

    const qreal contentLeft = cardRect.left() + cardPadding;
    const qreal contentRight = cardRect.right() - cardPadding;
    qreal y = cardRect.top() + cardPadding;

    painter.setFont(titleFont);
    painter.setPen(k_TitleColor);
    painter.drawText(QRectF(contentLeft, y, contentRight - contentLeft, titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, title);
    y += titleMetrics.height() + titleGap;

    painter.setFont(itemFont);
    for (int i = 0; i < items.size(); i++) {
        const QRectF itemRect(contentLeft + selectionMargin, y,
                              contentRight - contentLeft - (selectionMargin * 2), itemHeight);

        if (i == selectedIndex) {
            drawSelection(painter, itemRect, itemRadius, scale);
        }

        painter.setPen(i == selectedIndex ? k_SelectedItemColor : k_ItemColor);
        painter.drawText(itemRect.adjusted(itemPaddingX, 0, -itemPaddingX, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, items.at(i));

        y += itemHeight + itemSpacing;
    }

    if (!hint.isEmpty()) {
        y += hintGap - itemSpacing;
        painter.setFont(hintFont);
        painter.setPen(k_TitleColor);
        painter.drawText(QRectF(contentLeft, y, contentRight - contentLeft, hintMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, hint);
    }

    painter.end();

    return imageToSurface(image.convertToFormat(QImage::Format_ARGB32));
}

namespace {

// The graph card is authored at a fixed pixel size to match the debug text
// overlay it sits beside, which uses a fixed font size rather than scaling with
// the viewport.
const QColor k_GraphPlotColor(0x00, 0x00, 0x00, 0x66);
const QColor k_GraphGridColor(0xFF, 0xFF, 0xFF, 0x1F);
const QColor k_GraphValueColor(0xFF, 0xFF, 0xFF);

struct GraphSpec {
    const char* title;
    float StatsGraphPoint::* field;
    // Per-interval spread, where the value is measured per frame. Null for
    // metrics sampled once an interval, which have no spread to show.
    float StatsGraphPoint::* minField;
    float StatsGraphPoint::* maxField;
    QColor color;
    const char* unit;
    int decimals;
    // Smallest full-scale value, so an idle graph doesn't amplify noise into
    // something that looks like a problem.
    qreal minScale;
    // Frametime graphs also print the frame rate they correspond to.
    bool withFrameRate = false;
};

// Rounds a full-scale value up to the next 1/2/5 x 10^n so the axis label reads
// cleanly and the plot doesn't rescale on every sample.
qreal niceCeil(qreal value)
{
    if (!(value > 0)) {
        return 1;
    }

    const qreal magnitude = std::pow(10.0, std::floor(std::log10(value)));
    const qreal normalized = value / magnitude;
    const qreal step = normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10;
    return step * magnitude;
}

void drawGraph(QPainter& painter, const QRectF& plotRect, const GraphSpec& spec,
               const std::vector<StatsGraphPoint>& points, int maxPoints)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(k_GraphPlotColor);
    painter.drawRoundedRect(plotRect, 4, 4);

    // Scale against the spread, not just the mean, so a spike that only shows
    // up in the band is never clipped off the top of the plot.
    qreal scale = spec.minScale;
    qreal windowMin = 0, windowMax = 0;
    for (size_t i = 0; i < points.size(); i++) {
        const qreal low = spec.minField ? (qreal)(points[i].*spec.minField)
                                        : (qreal)(points[i].*spec.field);
        const qreal high = spec.maxField ? (qreal)(points[i].*spec.maxField)
                                         : (qreal)(points[i].*spec.field);
        if (i == 0 || low < windowMin) {
            windowMin = low;
        }
        if (i == 0 || high > windowMax) {
            windowMax = high;
        }
        scale = qMax(scale, high);
    }
    scale = niceCeil(scale);

    // A midpoint gridline is enough to read the shape against
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(k_GraphGridColor, 1));
    const qreal midY = plotRect.center().y();
    painter.drawLine(QPointF(plotRect.left() + 1, midY), QPointF(plotRect.right() - 1, midY));

    if (!points.empty() && maxPoints > 1) {
        const qreal stepX = plotRect.width() / (maxPoints - 1);
        const qreal valueToY = plotRect.height() / scale;

        // Anchor the newest sample to the right edge so a history that hasn't
        // filled the window grows leftward instead of stretching.
        auto pointAt = [&](size_t i, float StatsGraphPoint::* member) {
            const qreal x = plotRect.right() - ((points.size() - 1 - i) * stepX);
            const qreal y = plotRect.bottom() -
                    qBound((qreal)0, (qreal)(points[i].*member) * valueToY, plotRect.height());
            return QPointF(x, y);
        };

        QPolygonF line;
        line.reserve((int)points.size());
        for (size_t i = 0; i < points.size(); i++) {
            line.append(pointAt(i, spec.field));
        }

        painter.setClipRect(plotRect);

        if (spec.minField && spec.maxField) {
            // The spread within each interval, drawn behind the mean: the
            // maximum left to right, then the minimum back again.
            QPolygonF band;
            band.reserve((int)points.size() * 2);
            for (size_t i = 0; i < points.size(); i++) {
                band.append(pointAt(i, spec.maxField));
            }
            for (size_t i = points.size(); i-- > 0;) {
                band.append(pointAt(i, spec.minField));
            }

            QColor bandFill = spec.color;
            bandFill.setAlpha(0x40);
            painter.setPen(Qt::NoPen);
            painter.setBrush(bandFill);
            painter.drawPolygon(band);
        }
        else {
            // Nothing to shade between, so fill down to the baseline instead.
            QColor fill = spec.color;
            fill.setAlpha(0x4D);
            QPolygonF area = line;
            area.append(QPointF(line.last().x(), plotRect.bottom()));
            area.append(QPointF(line.first().x(), plotRect.bottom()));

            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawPolygon(area);
        }

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(spec.color, 1.5));
        painter.drawPolyline(line);
        painter.setClipping(false);
    }

    // The window's range, so min and max are readable as numbers and not only
    // as the extent of the band.
    painter.setPen(k_TitleColor);
    painter.drawText(plotRect.adjusted(6, 2, -6, 0), Qt::AlignLeft | Qt::AlignTop,
                     points.empty() ? QStringLiteral("--")
                                    : QStringLiteral("%1 - %2")
                                        .arg(windowMin, 0, 'f', spec.decimals)
                                        .arg(windowMax, 0, 'f', spec.decimals));
}

}

SDL_Surface* Painter::paintStatsGraphs(const std::vector<StatsGraphPoint>& points,
                                       int maxPoints,
                                       int windowSeconds)
{
    // Filled column-major: the left column follows the network path in, the
    // right column follows the client pipeline through to display.
    static const GraphSpec k_Graphs[] = {
        { "Incoming frametime", &StatsGraphPoint::incomingFrametimeMs,
          &StatsGraphPoint::incomingFrametimeMinMs, &StatsGraphPoint::incomingFrametimeMaxMs,
          QColor(0x26, 0xA6, 0x9A), " ms", 1, 20, true },
        { "Bandwidth", &StatsGraphPoint::videoMbps,
          nullptr, nullptr,
          QColor(0xEC, 0x40, 0x7A), " Mbps", 1, 5 },
        { "Network latency", &StatsGraphPoint::networkLatencyMs,
          nullptr, nullptr,
          QColor(0xAB, 0x47, 0xBC), " ms", 0, 20 },
        { "Network jitter", &StatsGraphPoint::networkJitterMs,
          nullptr, nullptr,
          QColor(0x7E, 0x57, 0xC2), " ms", 1, 5 },
        { "Dropped by network", &StatsGraphPoint::networkDroppedFrames,
          nullptr, nullptr,
          QColor(0xEF, 0x53, 0x50), "", 0, 4 },

        { "Rendering frametime", &StatsGraphPoint::renderingFrametimeMs,
          &StatsGraphPoint::renderingFrametimeMinMs, &StatsGraphPoint::renderingFrametimeMaxMs,
          QColor(0x4C, 0xAF, 0x50), " ms", 1, 20, true },
        { "Host processing latency", &StatsGraphPoint::hostProcessingLatencyMs,
          &StatsGraphPoint::hostProcessingLatencyMinMs, &StatsGraphPoint::hostProcessingLatencyMaxMs,
          QColor(0x42, 0xA5, 0xF5), " ms", 1, 10 },
        { "Reassembly time", &StatsGraphPoint::reassemblyMs,
          &StatsGraphPoint::reassemblyMinMs, &StatsGraphPoint::reassemblyMaxMs,
          QColor(0x26, 0xC6, 0xDA), " ms", 1, 5 },
        { "Frame queue depth", &StatsGraphPoint::queueDepth,
          nullptr, nullptr,
          QColor(0x9C, 0xCC, 0x65), "", 0, 3 },
        { "Dropped by network jitter", &StatsGraphPoint::jitterDroppedFrames,
          nullptr, nullptr,
          QColor(0xFF, 0xA7, 0x26), "", 0, 4 },
    };
    const int graphCount = (int)SDL_arraysize(k_Graphs);
    const int graphColumns = 2;
    const int graphRows = (graphCount + graphColumns - 1) / graphColumns;

    const qreal cardRadius = 12;
    const qreal cardPadding = 14;
    const qreal columnWidth = 264;
    const qreal columnGap = 16;
    const qreal cardWidth = (cardPadding * 2) + (columnWidth * graphColumns) +
                            (columnGap * (graphColumns - 1));
    const qreal labelHeight = 18;
    const qreal labelGap = 3;
    const qreal plotHeight = 40;
    const qreal graphGap = 10;
    const qreal shadowSpread = 14;

    QFont headerFont = menuFont(13, QFont::DemiBold);
    headerFont.setCapitalization(QFont::AllUppercase);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    QFont labelFont = menuFont(13, QFont::Normal);
    QFont valueFont = menuFont(14, QFont::DemiBold);
    QFont scaleFont = menuFont(11, QFont::Normal);

    QFontMetricsF headerMetrics(headerFont);

    const qreal graphHeight = labelHeight + labelGap + plotHeight;
    const qreal cardHeight = (cardPadding * 2) + headerMetrics.height() + graphGap +
                             (graphHeight * graphRows) + (graphGap * (graphRows - 1));

    // Reused across repaints. This card republishes for as long as it is on
    // screen, and reallocating a megabyte of pixels every time churns the
    // allocator to no purpose. Only the stats graph sampling thread paints
    // here, so a thread-local canvas needs no additional synchronization.
    static thread_local QImage image;
    const QSize cardSize(qRound(cardWidth + (shadowSpread * 2)),
                         qRound(cardHeight + (shadowSpread * 2)));
    if (image.size() != cardSize) {
        image = QImage(cardSize, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) {
            return nullptr;
        }
    }
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF cardRect(shadowSpread, shadowSpread, cardWidth, cardHeight);

    drawCardShadow(painter, cardRect, cardRadius, shadowSpread);

    painter.setPen(Qt::NoPen);
    painter.setBrush(k_SurfaceColor);
    painter.drawRoundedRect(cardRect, cardRadius, cardRadius);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(k_SurfaceEdgeColor, 1));
    painter.drawRoundedRect(cardRect.adjusted(0.5, 0.5, -0.5, -0.5), cardRadius, cardRadius);

    const qreal contentLeft = cardRect.left() + cardPadding;
    const qreal contentRight = cardRect.right() - cardPadding;
    qreal y = cardRect.top() + cardPadding;

    painter.setFont(headerFont);
    painter.setPen(k_TitleColor);
    painter.drawText(QRectF(contentLeft, y, contentRight - contentLeft, headerMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Last %1 seconds").arg(windowSeconds));
    y += headerMetrics.height() + graphGap;

    const qreal gridTop = y;
    for (int i = 0; i < graphCount; i++) {
        const GraphSpec& spec = k_Graphs[i];
        const qreal cellLeft = contentLeft + ((i / graphRows) * (columnWidth + columnGap));
        const qreal cellTop = gridTop + ((i % graphRows) * (graphHeight + graphGap));
        const QRectF labelRect(cellLeft, cellTop, columnWidth, labelHeight);

        painter.setFont(labelFont);
        painter.setPen(k_ItemColor);
        painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                         QString::fromUtf8(spec.title));

        QRectF valueRect = labelRect;
        if (!points.empty() && spec.withFrameRate) {
            // The equivalent frame rate is secondary to the frametime it comes
            // from, so it sits to its right in the smaller, dimmer label type.
            const float frametimeMs = points.back().*spec.field;
            const QString rateText = QStringLiteral("  (%1 FPS)")
                    .arg(frametimeMs > 0 ? qRound(1000.0 / frametimeMs) : 0);

            painter.setFont(scaleFont);
            painter.setPen(k_TitleColor);
            painter.drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter, rateText);
            valueRect.setRight(valueRect.right() -
                               QFontMetricsF(scaleFont).horizontalAdvance(rateText));
        }

        painter.setFont(valueFont);
        painter.setPen(points.empty() ? k_TitleColor : k_GraphValueColor);
        painter.drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter,
                         points.empty() ? QStringLiteral("--")
                                        : QStringLiteral("%1%2")
                                            .arg(points.back().*spec.field, 0, 'f', spec.decimals)
                                            .arg(QString::fromUtf8(spec.unit)));

        painter.setFont(scaleFont);
        drawGraph(painter,
                  QRectF(cellLeft, cellTop + labelHeight + labelGap,
                         columnWidth, plotHeight),
                  spec, points, maxPoints);
    }

    painter.end();

    return imageToSurface(image.convertToFormat(QImage::Format_ARGB32));
}

SDL_Surface* Painter::paintFill(QColor color)
{
    // Renderers stretch this over the viewport, so it only has to be large enough
    // to avoid edge cases with degenerate texture sizes.
    QImage image(16, 16, QImage::Format_ARGB32);
    if (image.isNull()) {
        return nullptr;
    }

    image.fill(color);

    return imageToSurface(image);
}
