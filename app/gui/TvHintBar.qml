import QtQuick 2.9
import QtQuick.Controls 2.2

import StreamingPreferences 1.0
import InputModeTracker 1.0
import TvTheme 1.0

// Shows what the gamepad buttons do on the current screen in TV mode.
// It fades out while the mouse is in use, but keeps its space so the
// page doesn't jump around.
Item {
    // Whether the page shows hints at all (segues don't)
    property bool active: true
    // Focus is in a dialog, menu, or other popup
    property bool inPopup: false
    // Focus is on a grid card, which has an options menu
    property bool inGrid: false
    property bool canGoBack: false
    property bool canOpenSettings: true
    // The control with focus, which decides what the select button says
    property Item focusItem: null
    // How far to stay in from the window's right and bottom edges
    property int safeX: 0
    property int safeY: 0

    // Face button positions, matching ControllerButtonStyle::FacePosition
    readonly property int south: 0
    readonly property int east: 1
    readonly property int west: 2
    readonly property int north: 3

    // Face buttons can be swapped in settings. SdlGamepadKeyNavigation swaps
    // A with B and X with Y, so the hints show the button that is pressed.
    readonly property bool swapped: StreamingPreferences.swapFaceButtons

    // A slider being adjusted takes the select and back buttons for itself
    readonly property bool editingSlider: focusItem !== null && focusItem instanceof NavigableSlider && focusItem.editing

    // What pressing select does to the focused control. Controls can say for
    // themselves by providing tvHintSelect.
    readonly property string selectText: {
        // A grid passes select on to its current card
        var item = focusItem instanceof GridView ? focusItem.currentItem : focusItem
        if (item === null) {
            return qsTr("Select")
        }
        if (item.tvHintSelect !== undefined && item.tvHintSelect !== "") {
            return item.tvHintSelect
        }
        if (focusItem instanceof NavigableSlider) {
            return focusItem.editing ? qsTr("Done") : qsTr("Adjust")
        }
        if (focusItem instanceof ComboBox) {
            return qsTr("Change")
        }
        if (focusItem instanceof CheckBox || focusItem instanceof Switch) {
            return focusItem.checked ? qsTr("Turn off") : qsTr("Turn on")
        }
        return qsTr("Select")
    }

    implicitHeight: 72 + safeY

    Row {
        anchors.right: parent.right
        anchors.rightMargin: safeX
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -safeY / 2
        spacing: TvTheme.spacingXLarge

        opacity: active && InputModeTracker.gamepadActive ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal } }

        TvButtonHint {
            position: swapped ? east : south
            text: selectText
        }

        TvButtonHint {
            visible: inGrid && !inPopup
            position: swapped ? north : west
            text: qsTr("Options")
        }

        TvButtonHint {
            visible: canOpenSettings && !inPopup && !editingSlider
            position: swapped ? west : north
            text: qsTr("Settings")
        }

        TvButtonHint {
            visible: !editingSlider
            position: swapped ? south : east
            text: inPopup ? qsTr("Close") : canGoBack ? qsTr("Back") : qsTr("Exit")
        }
    }
}
