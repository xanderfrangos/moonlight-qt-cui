import QtQuick 2.9
import QtQuick.Controls 2.2

GridView {
    property int minMargin: 10
    property real availableWidth: (parent.width - 2 * minMargin)
    property int itemsPerRow: availableWidth / cellWidth
    readonly property bool rowsFilled: itemsPerRow < count && availableWidth >= cellWidth
    property real horizontalMargin: rowsFilled ? (availableWidth % cellWidth) / 2 : minMargin

    // Extra space before the first column. The margins leave room for it.
    property real leftInset: 0

    function updateMargins() {
        leftMargin = horizontalMargin + leftInset
        rightMargin = horizontalMargin
    }

    onHorizontalMarginChanged: {
        updateMargins()
    }

    onLeftInsetChanged: {
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
}
