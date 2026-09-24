#include "overlaypainter.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QRectF>
#include <QtMath>

#include <Limelight.h>

#include <cmath>

#include "backend/systemproperties.h"
#include "settings/streamingpreferences.h"

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

// Converts a premultiplied canvas straight into a new surface in a single pass,
// rather than through an intermediate non-premultiplied image.
SDL_Surface* canvasToSurface(const QImage& canvas)
{
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, canvas.width(), canvas.height(),
                                                          32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to allocate overlay surface: %s",
                     SDL_GetError());
        return nullptr;
    }

    // The same memory layout as imageToSurface() relies on. Qt converts to
    // the non-premultiplied format as it copies.
    QImage target((uchar*)surface->pixels, surface->w, surface->h, surface->pitch,
                  QImage::Format_ARGB32);
    QPainter painter(&target);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(0, 0, canvas);
    painter.end();

    return surface;
}

// A soft shadow that lifts the card off the video without needing a blur pass
void drawCardShadow(QPainter& painter, const QRectF& cardRect, qreal radius, qreal spread,
                    qreal strength = 1.0)
{
    painter.setPen(Qt::NoPen);

    for (qreal i = spread; i >= 1; i--) {
        const qreal falloff = 1.0 - (i / spread);
        painter.setBrush(QColor(0, 0, 0, qRound(60.0 * falloff * falloff * strength)));
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
                                       const QList<ButtonHint>& hints,
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
    const qreal hintSpacing = 28 * scale;
    const qreal glyphGap = 10 * scale;
    const qreal shadowSpread = 24 * scale;

    QFont titleFont = menuFont(15 * scale, QFont::DemiBold);
    titleFont.setCapitalization(QFont::AllUppercase);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * scale);
    QFont itemFont = menuFont(21 * scale, QFont::Medium);
    QFont hintFont = menuFont(14 * scale, QFont::Normal);
    QFont glyphFont = menuFont(13 * scale, QFont::Bold);

    QFontMetricsF titleMetrics(titleFont);
    QFontMetricsF itemMetrics(itemFont);
    QFontMetricsF hintMetrics(hintFont);

    // Each hint's glyph sits in a circle a little taller than the hint text
    const qreal glyphDiameter = qRound(hintMetrics.height() * 1.5);
    const qreal hintRowHeight = qMax(glyphDiameter, hintMetrics.height());
    qreal hintsWidth = 0;
    for (int i = 0; i < hints.size(); i++) {
        hintsWidth += glyphDiameter + glyphGap + hintMetrics.horizontalAdvance(hints.at(i).text);
        if (i > 0) {
            hintsWidth += hintSpacing;
        }
    }

    // The selection ring is drawn outside the item rect, so the items need to sit
    // far enough inside the card to leave room for it.
    const qreal selectionMargin = 6 * scale;

    qreal contentWidth = qMax(titleMetrics.horizontalAdvance(title), hintsWidth);
    for (const QString& item : items) {
        contentWidth = qMax(contentWidth, itemMetrics.horizontalAdvance(item) + (itemPaddingX * 2));
    }

    const qreal cardWidth = qMax(contentWidth + (cardPadding * 2) + (selectionMargin * 2),
                                 420 * scale);

    qreal cardHeight = (cardPadding * 2) + selectionMargin +
                       titleMetrics.height() + titleGap +
                       (itemHeight * items.size()) + (itemSpacing * (items.size() - 1));
    if (!hints.isEmpty()) {
        cardHeight += hintGap + hintRowHeight;
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

    if (!hints.isEmpty()) {
        y += hintGap - itemSpacing;

        qreal x = contentLeft + selectionMargin;
        for (const ButtonHint& hint : hints) {
            const QRectF glyphRect(x, y + (hintRowHeight - glyphDiameter) / 2, glyphDiameter, glyphDiameter);
            const qreal borderWidth = qMax(1.0, 2 * scale);

            painter.setPen(QPen(hint.color, borderWidth));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(glyphRect.adjusted(borderWidth / 2, borderWidth / 2,
                                                   -borderWidth / 2, -borderWidth / 2));

            painter.setFont(glyphFont);
            painter.drawText(glyphRect, Qt::AlignCenter, hint.glyph);
            x += glyphDiameter + glyphGap;

            const qreal textWidth = hintMetrics.horizontalAdvance(hint.text);
            painter.setFont(hintFont);
            painter.setPen(k_TitleColor);
            painter.drawText(QRectF(x, y, textWidth + 1, hintRowHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, hint.text);
            x += textWidth + hintSpacing;
        }
    }

    painter.end();

    return imageToSurface(image.convertToFormat(QImage::Format_ARGB32));
}

namespace {

// The graph card is authored at a fixed pixel size to match the debug text
// overlay it sits beside, which uses a fixed font size rather than scaling with
// the viewport. The user can scale it from there, or have it follow the
// viewport like the gamepad menu does.
const QColor k_GraphGridColor(0xFF, 0xFF, 0xFF, 0x1F);
const QColor k_GraphValueColor(0xFF, 0xFF, 0xFF);
const QColor k_GraphTargetColor(0xFF, 0xFF, 0xFF, 0x70);

// The graph card's colors and corners. TV mode matches TvTheme.qml, and the
// desktop matches the Material dark style of the rest of the desktop app.
struct GraphPalette {
    QColor surface;
    QColor surfaceEdge;
    QColor text;
    QColor secondaryText;
    QColor chip;
    QColor plot;
    qreal cardRadius;
    qreal plotRadius;
};

const GraphPalette& graphPalette()
{
    static const GraphPalette k_TvPalette = {
        k_SurfaceColor, k_SurfaceEdgeColor, k_ItemColor, k_TitleColor,
        QColor(0xFF, 0xFF, 0xFF, 0x1A), QColor(0x00, 0x00, 0x00, 0x66),
        12, 4
    };
    // The desktop app's background, with Material's white text at full,
    // 70% and hint strengths
    static const GraphPalette k_DesktopPalette = {
        QColor(0x30, 0x30, 0x30, 0xF2), QColor(0xFF, 0xFF, 0xFF, 0x14),
        QColor(0xFF, 0xFF, 0xFF), QColor(0xFF, 0xFF, 0xFF, 0xB3),
        QColor(0xFF, 0xFF, 0xFF, 0x1F), QColor(0x00, 0x00, 0x00, 0x4D),
        4, 2
    };
    return SystemProperties::isTvMode() ? k_TvPalette : k_DesktopPalette;
}

// Opacity the card's colors are authored at, which the user's setting scales
constexpr qreal k_DefaultCardOpacity = 0.95;

struct GraphSpec {
    // A StreamingPreferences::PerformanceGraph, which also names the graph
    int id;
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
    // Frametime graphs also print the frame rate they correspond to, and mark
    // the stream's target frametime.
    bool withFrameRate = false;
    // A second series drawn as a line over the first, with its current value
    // named alongside the main one. Null when a graph plots only one series.
    float StatsGraphPoint::* secondaryField = nullptr;
    const char* secondaryName = nullptr;
    QColor secondaryColor;
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

// target is a value to mark with a reference line, or 0 for none. The plot's
// backdrop follows the card's opacity, relative to the default.
void drawGraph(QPainter& painter, const QRectF& plotRect, const GraphSpec& spec,
               const std::vector<StatsGraphPoint>& points, int maxPoints,
               qreal target, qreal opacity, qreal frametimeMin, qreal frametimeMax)
{
    const GraphPalette& palette = graphPalette();
    QColor plotColor = palette.plot;
    plotColor.setAlphaF(qMin(1.0, plotColor.alphaF() * opacity));
    painter.setPen(Qt::NoPen);
    painter.setBrush(plotColor);
    painter.drawRoundedRect(plotRect, palette.plotRadius, palette.plotRadius);

    // Scale against the spread, not just the mean, so a spike that only shows
    // up in the band is never clipped off the plot.
    qreal scale = qMax(spec.minScale, target);
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
        if (spec.secondaryField) {
            scale = qMax(scale, (qreal)(points[i].*spec.secondaryField));
        }
    }
    // All visible frametime graphs share one close view of their combined
    // spread. Leave at least 1 ms, or 10% of that spread, at both edges.
    qreal baseline = 0;
    if (spec.withFrameRate && !points.empty()) {
        const qreal padding = qMax((qreal)1, (frametimeMax - frametimeMin) * 0.1);
        baseline = qMax((qreal)0, frametimeMin - padding);
        scale = frametimeMax + padding;
    }
    else {
        scale = niceCeil(scale);
    }
    const qreal plotRange = scale - baseline;

    // A midpoint gridline is enough to read the shape against
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(k_GraphGridColor, 1));
    const qreal midY = plotRect.center().y();
    painter.drawLine(QPointF(plotRect.left() + 1, midY), QPointF(plotRect.right() - 1, midY));

    // Draw the target only when it fits the data-focused frametime view.
    // An off-scale target must not flatten the observed variation.
    if (target > baseline && target < scale) {
        const qreal targetY = plotRect.bottom() -
                ((target - baseline) * plotRect.height() / plotRange);
        painter.setPen(QPen(k_GraphTargetColor, 1, Qt::DashLine));
        painter.drawLine(QPointF(plotRect.left() + 1, targetY),
                         QPointF(plotRect.right() - 1, targetY));
    }

    if (!points.empty() && maxPoints > 1) {
        const qreal stepX = plotRect.width() / (maxPoints - 1);
        const qreal valueToY = plotRect.height() / plotRange;

        // Anchor the newest sample to the right edge so a history that hasn't
        // filled the window grows leftward instead of stretching.
        auto pointAt = [&](size_t i, float StatsGraphPoint::* member) {
            const qreal x = plotRect.right() - ((points.size() - 1 - i) * stepX);
            const qreal y = plotRect.bottom() -
                    qBound((qreal)0,
                           ((qreal)(points[i].*member) - baseline) * valueToY,
                           plotRect.height());
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

        if (spec.secondaryField) {
            QPolygonF secondaryLine;
            secondaryLine.reserve((int)points.size());
            for (size_t i = 0; i < points.size(); i++) {
                secondaryLine.append(pointAt(i, spec.secondaryField));
            }
            painter.setPen(QPen(spec.secondaryColor, 1.5));
            painter.drawPolyline(secondaryLine);
        }

        painter.setClipping(false);
    }

    // The window's range, so min and max are readable as numbers and not only
    // as the extent of the band.
    painter.setPen(palette.secondaryText);
    painter.drawText(plotRect.adjusted(6, 2, -6, 0), Qt::AlignLeft | Qt::AlignTop,
                     points.empty() ? QStringLiteral("--")
                                    : QStringLiteral("%1 - %2")
                                        .arg(windowMin, 0, 'f', spec.decimals)
                                        .arg(windowMax, 0, 'f', spec.decimals));
}

// The codec family, without the bit depth and chroma the other chips show
const char* codecName(int videoFormat)
{
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        return "H.264";
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        return "HEVC";
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        return "AV1";
    }
    return "Unknown codec";
}

// Everything the text overlay's first line says about the stream, split into
// chips. SDR and 4:2:0 are shown rather than left out, so each fact is always
// in the same place and a missing chip never has to be interpreted.
QStringList streamInfoChips(const StatsGraphStreamInfo& info)
{
    QStringList chips;

    if (info.width > 0 && info.height > 0) {
        chips.append(QStringLiteral("%1×%2").arg(info.width).arg(info.height));
    }
    if (info.frameRate > 0) {
        chips.append(QStringLiteral("%1 FPS").arg(info.frameRate));
    }
    // Nothing is shown when presentation isn't synchronized at all
    if (info.syncMode == StatsGraphSyncMode::Vrr) {
        chips.append(QStringLiteral("VRR"));
    }
    else if (info.syncMode == StatsGraphSyncMode::VSync) {
        chips.append(QStringLiteral("V-Sync"));
    }
    if (info.videoFormat != 0) {
        chips.append(QString::fromUtf8(codecName(info.videoFormat)));
        if (info.videoFormat & VIDEO_FORMAT_MASK_10BIT) {
            chips.append(info.outputBitsPerComponent == 8 ? QStringLiteral("10-bit -> 8-bit")
                                                          : QStringLiteral("10-bit"));
        }
        else {
            chips.append(QStringLiteral("8-bit"));
        }
        chips.append(info.hdr ? QStringLiteral("HDR") : QStringLiteral("SDR"));
        chips.append((info.videoFormat & VIDEO_FORMAT_MASK_YUV444) ? QStringLiteral("4:4:4")
                                                                   : QStringLiteral("4:2:0"));
    }
    if (info.renderer != nullptr) {
        chips.append(info.backendRenderer != nullptr
                             ? QStringLiteral("%1 + %2").arg(QString::fromUtf8(info.renderer),
                                                             QString::fromUtf8(info.backendRenderer))
                             : QString::fromUtf8(info.renderer));
    }

    return chips;
}

}

SDL_Surface* Painter::paintStatsGraphs(const std::vector<StatsGraphPoint>& points,
                                       int maxPoints,
                                       const StatsGraphConfig& config,
                                       const StatsGraphStreamInfo& streamInfo,
                                       bool showStreamInfo,
                                       qreal scale,
                                       QSize maxSize)
{
    // Graphs are dealt into the two columns in turn, so this order reads
    // across each row. The default graphs pair the network path in on the
    // left with the client pipeline on the right. Most opt-in graphs come
    // last, so turning them on never shifts a default graph across.
    static const GraphSpec k_Graphs[] = {
        { StreamingPreferences::PG_INCOMING_FRAMETIME, &StatsGraphPoint::incomingFrametimeMs,
          &StatsGraphPoint::incomingFrametimeMinMs, &StatsGraphPoint::incomingFrametimeMaxMs,
          QColor(0x26, 0xA6, 0x9A), " ms", 1, 20, true },
        { StreamingPreferences::PG_RENDERING_FRAMETIME, &StatsGraphPoint::renderingFrametimeMs,
          &StatsGraphPoint::renderingFrametimeMinMs, &StatsGraphPoint::renderingFrametimeMaxMs,
          QColor(0x4C, 0xAF, 0x50), " ms", 1, 20, true },
        // Everything on the wire, with the video payload inside it drawn over
        // the top, so the gap between the two is the FEC and packet overhead.
        { StreamingPreferences::PG_BANDWIDTH, &StatsGraphPoint::networkMbps,
          nullptr, nullptr,
          QColor(0xEC, 0x40, 0x7A), " Mbps", 1, 5, false,
          &StatsGraphPoint::videoMbps, "video", QColor(0xF8, 0xBB, 0xD0) },
        // Opt-in. Placed to land directly under the rendering frametime it sits
        // beside in the pipeline, which shifts the graphs after it along by one.
        { StreamingPreferences::PG_DECODING_FRAMETIME, &StatsGraphPoint::decodingFrametimeMs,
          &StatsGraphPoint::decodingFrametimeMinMs, &StatsGraphPoint::decodingFrametimeMaxMs,
          QColor(0x5C, 0x6B, 0xC0), " ms", 1, 20, true },
        { StreamingPreferences::PG_HOST_PROCESSING_LATENCY, &StatsGraphPoint::hostProcessingLatencyMs,
          &StatsGraphPoint::hostProcessingLatencyMinMs, &StatsGraphPoint::hostProcessingLatencyMaxMs,
          QColor(0x42, 0xA5, 0xF5), " ms", 1, 10 },
        { StreamingPreferences::PG_NETWORK_LATENCY, &StatsGraphPoint::networkLatencyMs,
          nullptr, nullptr,
          QColor(0xAB, 0x47, 0xBC), " ms", 0, 20 },
        { StreamingPreferences::PG_REASSEMBLY, &StatsGraphPoint::reassemblyMs,
          &StatsGraphPoint::reassemblyMinMs, &StatsGraphPoint::reassemblyMaxMs,
          QColor(0x26, 0xC6, 0xDA), " ms", 1, 5 },
        { StreamingPreferences::PG_NETWORK_JITTER, &StatsGraphPoint::networkJitterMs,
          nullptr, nullptr,
          QColor(0x7E, 0x57, 0xC2), " ms", 1, 5 },
        { StreamingPreferences::PG_QUEUE_DEPTH, &StatsGraphPoint::queueDepth,
          nullptr, nullptr,
          QColor(0x9C, 0xCC, 0x65), "", 0, 3 },
        { StreamingPreferences::PG_NETWORK_DROPS, &StatsGraphPoint::networkDroppedFrames,
          nullptr, nullptr,
          QColor(0xEF, 0x53, 0x50), "", 0, 4 },
        { StreamingPreferences::PG_JITTER_DROPS, &StatsGraphPoint::jitterDroppedFrames,
          nullptr, nullptr,
          QColor(0xFF, 0xA7, 0x26), "", 0, 4 },

        // Opt-in, in the order a frame passes through them
        { StreamingPreferences::PG_DECODING_TIME, &StatsGraphPoint::decodingTimeMs,
          &StatsGraphPoint::decodingTimeMinMs, &StatsGraphPoint::decodingTimeMaxMs,
          QColor(0x8D, 0x6E, 0x63), " ms", 1, 5 },
        { StreamingPreferences::PG_RENDERING_TIME, &StatsGraphPoint::renderingTimeMs,
          &StatsGraphPoint::renderingTimeMinMs, &StatsGraphPoint::renderingTimeMaxMs,
          QColor(0xD4, 0xE1, 0x57), " ms", 1, 10 },
    };

    // Dealing one graph to each column in turn keeps the columns within one
    // graph of each other, whichever graphs are hidden. A lone graph gets the
    // card to itself.
    std::vector<std::vector<const GraphSpec*>> columns;
    int visibleCount = 0;
    for (const GraphSpec& spec : k_Graphs) {
        if (config.visibleGraphs & (1u << spec.id)) {
            if (columns.size() <= (size_t)(visibleCount % 2)) {
                columns.emplace_back();
            }
            columns[visibleCount % 2].push_back(&spec);
            visibleCount++;
        }
    }

    const QStringList chips = showStreamInfo ? streamInfoChips(streamInfo) : QStringList();
    if (columns.empty() && chips.isEmpty()) {
        return nullptr;
    }

    // Use the same vertical axis for every visible frametime graph. Keep
    // each graph's own windowMin/windowMax for its printed range.
    qreal frametimeMin = 0, frametimeMax = 0;
    bool haveFrametime = false;
    for (const auto& column : columns) {
        for (const GraphSpec* spec : column) {
            if (!spec->withFrameRate) {
                continue;
            }
            for (const StatsGraphPoint& point : points) {
                const qreal low = point.*spec->minField;
                const qreal high = point.*spec->maxField;
                if (!haveFrametime) {
                    frametimeMin = low;
                    frametimeMax = high;
                    haveFrametime = true;
                }
                else {
                    frametimeMin = qMin(frametimeMin, low);
                    frametimeMax = qMax(frametimeMax, high);
                }
            }
        }
    }

    const int graphColumns = (int)columns.size();
    int graphRows = 0;
    for (const auto& column : columns) {
        graphRows = qMax(graphRows, (int)column.size());
    }

    const GraphPalette& palette = graphPalette();

    // Laid out at 100% and drawn through a scaled painter, so text and lines
    // are rasterized at the final size rather than stretched.
    const qreal cardRadius = palette.cardRadius;
    const qreal cardPadding = 14;
    const qreal columnWidth = 264;
    const qreal columnGap = 16;
    const qreal contentWidth = qMax(1, graphColumns) * columnWidth +
                               qMax(0, graphColumns - 1) * columnGap;
    const qreal cardWidth = (cardPadding * 2) + contentWidth;
    const qreal labelHeight = 18;
    const qreal labelGap = 3;
    const qreal plotHeight = qMax(config.plotHeight, 16);
    const qreal graphGap = 10;
    const qreal shadowSpread = 14;
    const qreal chipPaddingX = 7;
    const qreal chipHeight = 20;
    const qreal chipGap = 6;

    QFont headerFont = menuFont(13, QFont::DemiBold);
    headerFont.setCapitalization(QFont::AllUppercase);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    QFont labelFont = menuFont(13, QFont::Normal);
    QFont valueFont = menuFont(14, QFont::DemiBold);
    QFont scaleFont = menuFont(11, QFont::Normal);
    QFont chipFont = menuFont(12, QFont::DemiBold);

    QFontMetricsF headerMetrics(headerFont);
    QFontMetricsF chipMetrics(chipFont);

    // Chips flow left to right and wrap within the width the graphs set
    std::vector<QRectF> chipRects;
    qreal chipsHeight = 0;
    {
        qreal x = 0, y = 0;
        for (const QString& chip : chips) {
            const qreal width = qMin(chipMetrics.horizontalAdvance(chip) + (chipPaddingX * 2),
                                     contentWidth);
            if (x > 0 && x + width > contentWidth) {
                x = 0;
                y += chipHeight + chipGap;
            }
            chipRects.emplace_back(x, y, width, chipHeight);
            x += width + chipGap;
        }
        if (!chipRects.empty()) {
            chipsHeight = chipRects.back().bottom();
        }
    }

    const qreal graphHeight = labelHeight + labelGap + plotHeight;
    qreal cardHeight = cardPadding * 2;
    if (!chipRects.empty()) {
        cardHeight += chipsHeight;
        if (graphRows > 0) {
            cardHeight += graphGap;
        }
    }
    if (graphRows > 0) {
        cardHeight += headerMetrics.height() + graphGap +
                      (graphHeight * graphRows) + (graphGap * (graphRows - 1));
    }

    // Shrink to fit the space available rather than running off the screen,
    // down to a size that is still legible.
    const qreal naturalWidth = cardWidth + (shadowSpread * 2);
    const qreal naturalHeight = cardHeight + (shadowSpread * 2);
    if (maxSize.width() > 0) {
        scale = qMin(scale, maxSize.width() / naturalWidth);
    }
    if (maxSize.height() > 0) {
        scale = qMin(scale, maxSize.height() / naturalHeight);
    }
    scale = qMax(scale, 0.5);

    const qreal opacity = qBound(0, config.backgroundOpacity, 100) / 100.0;
    const qreal opacityFactor = opacity / k_DefaultCardOpacity;

    // Reused across repaints. This card republishes for as long as it is on
    // screen, and reallocating a megabyte of pixels every time churns the
    // allocator to no purpose. Only the stats graph sampling thread paints
    // here, so a thread-local canvas needs no additional synchronization.
    static thread_local QImage image;
    const QSize cardSize(qCeil(naturalWidth * scale), qCeil(naturalHeight * scale));
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
    painter.scale(scale, scale);

    const QRectF cardRect(shadowSpread, shadowSpread, cardWidth, cardHeight);

    drawCardShadow(painter, cardRect, cardRadius, shadowSpread, qMin(1.0, opacityFactor));

    QColor surfaceColor = palette.surface;
    surfaceColor.setAlphaF(opacity);
    painter.setPen(Qt::NoPen);
    painter.setBrush(surfaceColor);
    painter.drawRoundedRect(cardRect, cardRadius, cardRadius);

    QColor edgeColor = palette.surfaceEdge;
    edgeColor.setAlphaF(qMin(1.0, edgeColor.alphaF() * opacityFactor));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(edgeColor, 1));
    painter.drawRoundedRect(cardRect.adjusted(0.5, 0.5, -0.5, -0.5), cardRadius, cardRadius);

    const qreal contentLeft = cardRect.left() + cardPadding;
    const qreal contentRight = cardRect.right() - cardPadding;
    qreal y = cardRect.top() + cardPadding;

    // Frametime graphs mark the frametime the stream is meant to arrive at
    const qreal targetFrametimeMs = streamInfo.frameRate > 0 ? 1000.0 / streamInfo.frameRate : 0;

    if (!chipRects.empty()) {
        painter.setFont(chipFont);
        for (int i = 0; i < chips.size(); i++) {
            const QRectF chipRect = chipRects[i].translated(contentLeft, y);
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette.chip);
            painter.drawRoundedRect(chipRect, chipHeight / 2, chipHeight / 2);

            painter.setBrush(Qt::NoBrush);
            painter.setPen(palette.text);
            painter.drawText(chipRect.adjusted(chipPaddingX, 0, -chipPaddingX, 0),
                             Qt::AlignCenter,
                             chipMetrics.elidedText(chips[i], Qt::ElideRight,
                                                    chipRect.width() - (chipPaddingX * 2)));
        }
        y += chipsHeight;
        if (graphRows > 0) {
            y += graphGap;
        }
    }

    if (graphRows > 0) {
        painter.setFont(headerFont);
        painter.setPen(palette.secondaryText);
        painter.drawText(QRectF(contentLeft, y, contentRight - contentLeft, headerMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Last %1 seconds").arg(config.windowSeconds));
        y += headerMetrics.height() + graphGap;
    }

    const qreal gridTop = y;
    for (int column = 0; column < graphColumns; column++) {
        for (int row = 0; row < (int)columns[column].size(); row++) {
            const GraphSpec& spec = *columns[column][row];
            const qreal cellLeft = contentLeft + (column * (columnWidth + columnGap));
            const qreal cellTop = gridTop + (row * (graphHeight + graphGap));
            const QRectF labelRect(cellLeft, cellTop, columnWidth, labelHeight);

            painter.setFont(labelFont);
            painter.setPen(palette.text);
            painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                             StreamingPreferences::performanceGraphName(spec.id));

            QRectF valueRect = labelRect;
            if (!points.empty() && spec.withFrameRate) {
                // The equivalent frame rate is secondary to the frametime it comes
                // from, so it sits to its right in the smaller, dimmer label type.
                const float frametimeMs = points.back().*spec.field;
                const QString rateText = QStringLiteral("  (%1 FPS)")
                        .arg(frametimeMs > 0 ? qRound(1000.0 / frametimeMs) : 0);

                painter.setFont(scaleFont);
                painter.setPen(palette.secondaryText);
                painter.drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter, rateText);
                valueRect.setRight(valueRect.right() -
                                   QFontMetricsF(scaleFont).horizontalAdvance(rateText));
            }
            else if (!points.empty() && spec.secondaryField) {
                // Named in its own line colour, which doubles as the legend
                const QString secondaryText = QStringLiteral("  (%1 %2)")
                        .arg(points.back().*spec.secondaryField, 0, 'f', spec.decimals)
                        .arg(QString::fromUtf8(spec.secondaryName));

                painter.setFont(scaleFont);
                painter.setPen(spec.secondaryColor);
                painter.drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter, secondaryText);
                valueRect.setRight(valueRect.right() -
                                   QFontMetricsF(scaleFont).horizontalAdvance(secondaryText));
            }

            painter.setFont(valueFont);
            painter.setPen(points.empty() ? palette.secondaryText : k_GraphValueColor);
            painter.drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter,
                             points.empty() ? QStringLiteral("--")
                                            : QStringLiteral("%1%2")
                                                .arg(points.back().*spec.field, 0, 'f', spec.decimals)
                                                .arg(QString::fromUtf8(spec.unit)));

            painter.setFont(scaleFont);
            drawGraph(painter,
                      QRectF(cellLeft, cellTop + labelHeight + labelGap,
                             columnWidth, plotHeight),
                      spec, points, maxPoints,
                      spec.withFrameRate ? targetFrametimeMs : 0,
                      opacityFactor, frametimeMin, frametimeMax);
        }
    }

    painter.end();

    return canvasToSurface(image);
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
