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

    // TV mode: the width of each card. When set, the columns are spread so
    // the first card starts at minMargin and a full row's last card ends
    // minMargin from the right edge, rather than centering the grid. Cards
    // are at least tvMinSpacing apart. Views use tvCellWidth as cellWidth.
    property real tvItemWidth: 0
    property real tvMinSpacing: 0
    readonly property bool tvSpread: SystemProperties.tvMode && tvItemWidth > 0
    readonly property int tvColumns: Math.max(1, Math.floor((width - 2 * minMargin + tvMinSpacing) /
                                                            (tvItemWidth + tvMinSpacing)))
    readonly property real tvCellWidth: tvColumns > 1 ? (width - 2 * minMargin - tvItemWidth) / (tvColumns - 1)
                                                      : tvItemWidth + tvMinSpacing

    function updateMargins() {
        if (tvSpread) {
            // Each card sits at the left of its cell, so the last cell runs
            // past the last card. Let it run into the right margin, less a
            // pixel so rounding can't cost a column.
            leftMargin = minMargin
            rightMargin = minMargin - (cellWidth - tvItemWidth) - 1
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
