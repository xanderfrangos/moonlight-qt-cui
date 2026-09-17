import QtQuick 2.0
import QtQuick.Controls 2.2

import SystemProperties 1.0
import TvTheme 1.0

ItemDelegate {
    id: card

    property GridView grid

    // Set by grids that draw their own TV mode card visuals. In TV mode the
    // focused card grows, the others dim, and cards fade in on first load
    // instead of using the Material list highlight.
    property bool tvCardStyle: false
    readonly property bool tvCard: SystemProperties.tvMode && tvCardStyle

    // Opacity for dimming and hiding, which delegates may lower further
    property real cardOpacity: 1.0

    // Animated from 0 to 1 when the card first appears
    property real appearProgress: 1.0

    property real focusDim: tvCard && grid.activeFocus && !highlighted ? 0.7 : 1.0
    Behavior on focusDim { NumberAnimation { duration: TvTheme.animationNormal } }

    highlighted: grid.activeFocus && grid.currentItem === this

    // Draw the focused card above its neighbors so it can grow over them
    z: highlighted ? 1 : 0
    scale: tvCard && highlighted ? 1.08 : 1.0
    Behavior on scale {
        enabled: tvCard
        NumberAnimation { duration: TvTheme.animationNormal; easing.type: Easing.OutCubic }
    }

    opacity: cardOpacity * focusDim * appearProgress
    transform: Translate { y: (1.0 - appearProgress) * 16 }

    SequentialAnimation {
        id: appearAnimation
        PauseAnimation { id: appearDelay }
        NumberAnimation {
            target: card
            property: "appearProgress"
            to: 1.0
            duration: TvTheme.animationNormal * 2
            easing.type: Easing.OutCubic
        }
    }

    Component.onCompleted: {
        if (tvCard) {
            // The card's own Material highlight would draw a square behind it
            background.opacity = 0

            // Stagger cards that appear while the grid is first loading. Cards
            // created later by scrolling show up immediately.
            if (grid.tvCardsSettled === false) {
                appearProgress = 0
                appearDelay.duration = Math.min(index, 12) * 35
                appearAnimation.start()
            }
        }
    }

    Keys.onLeftPressed: {
        grid.moveCurrentIndexLeft()
    }
    Keys.onRightPressed: {
        grid.moveCurrentIndexRight()
    }
    Keys.onDownPressed: {
        grid.moveCurrentIndexDown()
    }
    Keys.onUpPressed: {
        grid.moveCurrentIndexUp()

        // If we've reached the top of the grid, move focus to the toolbar
        if (grid.currentItem === this) {
            nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        }
    }
    Keys.onReturnPressed: {
        clicked()
    }
    Keys.onEnterPressed: {
        clicked()
    }
}
