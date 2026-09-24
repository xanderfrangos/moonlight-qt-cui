import QtQuick 2.9
import QtQuick.Controls 2.2

import SystemProperties 1.0

GridView {
    property int minMargin: 10
    property real availableWidth: (parent.width - 2 * minMargin)
    property int itemsPerRow: availableWidth / cellWidth
    readonly property bool rowsFilled: itemsPerRow < count && availableWidth >= cellWidth
    property real horizontalMargin: rowsFilled ? (availableWidth % cellWidth) / 2 : minMargin

    // Extra space before the first column, and after the last. The margins
    // leave room for them.
    property real leftInset: 0
    property real rightInset: 0

    // TV mode: the card width. Column capacity and scale use the grid width,
    // not the number of cards currently present, so short rows use the same
    // card size and spacing as full rows.
    property real tvItemWidth: 0
    property real tvMinSpacing: 0
    property real tvGridSpacing: tvMinSpacing
    readonly property bool tvSpread: SystemProperties.tvMode && tvItemWidth > 0
    readonly property real tvAvailableWidth: width - 2 * minMargin
    readonly property int tvColumns: Math.max(1, Math.floor((tvAvailableWidth + tvMinSpacing) /
                                                            (tvItemWidth + tvMinSpacing)))
    readonly property real tvScale: tvSpread
                                    ? Math.max(0.1, tvAvailableWidth /
                                               (tvColumns * tvItemWidth + (tvColumns - 1) * tvGridSpacing))
                                    : 1.0
    readonly property real tvScaledItemWidth: tvItemWidth * tvScale
    readonly property real tvCellWidth: tvColumns > 1 ? (tvAvailableWidth - tvScaledItemWidth) / (tvColumns - 1)
                                                      : tvScaledItemWidth + tvGridSpacing * tvScale

    function updateMargins() {
        if (tvSpread) {
            // Each card sits at the left of its cell, so a full row's last
            // card ends at minMargin from the right edge. The extra pixel
            // avoids rounding that could cost a column.
            // Cards scale from their center, so offset their layout cells to
            // keep the first and last scaled cards aligned with the margins.
            leftMargin = minMargin + (tvScaledItemWidth - tvItemWidth) / 2
            rightMargin = leftMargin - (cellWidth - tvItemWidth) - 1
        }
        else {
            leftMargin = horizontalMargin + leftInset
            rightMargin = horizontalMargin + rightInset
        }

        // Changing a margin doesn't move the content, so the grid would stay
        // at its old horizontal position until it was scrolled. This grid only
        // scrolls vertically, so the content always starts at the left margin.
        // Desktop mode keeps its original layout.
        if (SystemProperties.tvMode) {
            contentX = -leftMargin
        }
    }

    onHorizontalMarginChanged: {
        updateMargins()
    }

    onLeftInsetChanged: {
        updateMargins()
    }

    onRightInsetChanged: {
        updateMargins()
    }

    onCellWidthChanged: {
        updateMargins()
    }

    onTvScaleChanged: {
        updateMargins()
    }

    onWidthChanged: {
        updateMargins()
    }

    Component.onCompleted: {
        updateMargins()
    }

    // Becomes true shortly after the grid first gets items. TV mode cards
    // fade in one after another until then.
    property bool tvCardsSettled: false

    onCountChanged: {
        if (count > 0 && !tvCardsSettled && !settleTimer.running) {
            settleTimer.start()
        }
    }

    Timer {
        id: settleTimer
        interval: 1000
        onTriggered: tvCardsSettled = true
    }

    boundsBehavior: Flickable.OvershootBounds

    // The TV mode toolbar and hint bar are transparent, so cards must not
    // scroll underneath them
    clip: SystemProperties.tvMode
}
