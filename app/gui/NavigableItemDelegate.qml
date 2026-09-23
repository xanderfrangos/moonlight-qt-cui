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

    // What the TV mode hint bar says the select button does on this card
    property string tvHintSelect: ""

    // Opacity for dimming and hiding, which delegates may lower further
    property real cardOpacity: 1.0

    // Animated from 0 to 1 when the card first appears
    property real appearProgress: 1.0

    // Set by delegates while this card's own popup (like its context menu)
    // is open. Focus moves into the popup, but the card stays selected.
    property bool popupOpen: false

    // The card is selected when it has focus in the grid or its popup is
    // open. The TV mode visuals follow this rather than highlighted, so the
    // card doesn't shrink and move its context menu when the menu opens.
    readonly property bool tvSelected: highlighted || (popupOpen && grid.currentItem === this)

    readonly property bool gridInUse: grid.activeFocus ||
                                      (grid.currentItem !== null && grid.currentItem.popupOpen === true)
    property real focusDim: tvCard && gridInUse && !tvSelected ? 0.7 : 1.0
    Behavior on focusDim { NumberAnimation { duration: TvTheme.animationNormal } }

    highlighted: grid.activeFocus && grid.currentItem === this

    // Draw the focused card above its neighbors so it can grow over them
    z: tvSelected ? 1 : 0
    scale: tvCard && tvSelected ? 1.08 : 1.0
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
