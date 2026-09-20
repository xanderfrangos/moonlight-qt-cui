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
    QColor color;
    const char* unit;
    int decimals;
    // Smallest full-scale value, so an idle graph doesn't amplify noise into
    // something that looks like a problem.
    qreal minScale;
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

    qreal scale = spec.minScale;
    for (const StatsGraphPoint& point : points) {
        scale = qMax(scale, (qreal)(point.*spec.field));
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

        QPolygonF line;
        line.reserve((int)points.size());
        for (size_t i = 0; i < points.size(); i++) {
            // Anchor the newest sample to the right edge so a history that
            // hasn't filled the window grows leftward instead of stretching.
            const qreal x = plotRect.right() - ((points.size() - 1 - i) * stepX);
            const qreal y = plotRect.bottom() -
                    qBound((qreal)0, (qreal)(points[i].*spec.field) * valueToY, plotRect.height());
            line.append(QPointF(x, y));
        }

        QColor fill = spec.color;
        fill.setAlpha(0x4D);
        QPolygonF area = line;
        area.append(QPointF(line.last().x(), plotRect.bottom()));
        area.append(QPointF(line.first().x(), plotRect.bottom()));

        painter.setClipRect(plotRect);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPolygon(area);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(spec.color, 1.5));
        painter.drawPolyline(line);
        painter.setClipping(false);
    }

    // Full scale, so the shape can be read as an actual magnitude
    painter.setPen(k_TitleColor);
    painter.drawText(plotRect.adjusted(6, 2, -6, 0), Qt::AlignLeft | Qt::AlignTop,
                     QStringLiteral("%1").arg(scale, 0, 'f', scale < 10 ? 1 : 0));
}

}

SDL_Surface* Painter::paintStatsGraphs(const std::vector<StatsGraphPoint>& points,
                                       int maxPoints,
                                       int windowSeconds)
{
    static const GraphSpec k_Graphs[] = {
        { "Rendering frame rate", &StatsGraphPoint::renderedFps,
          QColor(0x4C, 0xAF, 0x50), " FPS", 0, 30 },
        { "Host processing latency", &StatsGraphPoint::hostProcessingLatencyMs,
          QColor(0x42, 0xA5, 0xF5), " ms", 1, 10 },
        { "Dropped by network", &StatsGraphPoint::networkDroppedFrames,
          QColor(0xEF, 0x53, 0x50), "", 0, 4 },
        { "Dropped by network jitter", &StatsGraphPoint::jitterDroppedFrames,
          QColor(0xFF, 0xA7, 0x26), "", 0, 4 },
        { "Network latency", &StatsGraphPoint::networkLatencyMs,
          QColor(0xAB, 0x47, 0xBC), " ms", 0, 20 },
    };
    const int graphCount = (int)SDL_arraysize(k_Graphs);

    const qreal cardRadius = 12;
    const qreal cardPadding = 14;
    const qreal cardWidth = 360;
    const qreal labelHeight = 18;
    const qreal labelGap = 3;
    const qreal plotHeight = 52;
    const qreal graphGap = 12;
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
                             (graphHeight * graphCount) + (graphGap * (graphCount - 1));

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

    painter.setFont(headerFont);
    painter.setPen(k_TitleColor);
    painter.drawText(QRectF(contentLeft, y, contentRight - contentLeft, headerMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Last %1 seconds").arg(windowSeconds));
    y += headerMetrics.height() + graphGap;

    for (int i = 0; i < graphCount; i++) {
        const GraphSpec& spec = k_Graphs[i];
        const QRectF labelRect(contentLeft, y, contentRight - contentLeft, labelHeight);

        painter.setFont(labelFont);
        painter.setPen(k_ItemColor);
        painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                         QString::fromUtf8(spec.title));

        painter.setFont(valueFont);
        painter.setPen(points.empty() ? k_TitleColor : k_GraphValueColor);
        painter.drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter,
                         points.empty() ? QStringLiteral("--")
                                        : QStringLiteral("%1%2")
                                            .arg(points.back().*spec.field, 0, 'f', spec.decimals)
                                            .arg(QString::fromUtf8(spec.unit)));

        painter.setFont(scaleFont);
        drawGraph(painter,
                  QRectF(contentLeft, y + labelHeight + labelGap,
                         contentRight - contentLeft, plotHeight),
                  spec, points, maxPoints);

        y += graphHeight + graphGap;
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
