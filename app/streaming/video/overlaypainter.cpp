#include "overlaypainter.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
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
