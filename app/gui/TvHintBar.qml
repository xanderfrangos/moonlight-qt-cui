import QtQuick 2.9

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

    // Face buttons can be swapped in settings. SdlGamepadKeyNavigation swaps
    // A with B and X with Y, so the hints show the button that is pressed.
    readonly property bool swapped: StreamingPreferences.swapFaceButtons

    implicitHeight: 64

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 32
        anchors.verticalCenter: parent.verticalCenter
        spacing: 36

        opacity: active && InputModeTracker.gamepadActive ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal } }

        TvButtonHint {
            button: swapped ? "B" : "A"
            text: qsTr("Select")
        }

        TvButtonHint {
            visible: inGrid && !inPopup
            button: swapped ? "Y" : "X"
            text: qsTr("Options")
        }

        TvButtonHint {
            visible: canOpenSettings && !inPopup
            button: swapped ? "X" : "Y"
            text: qsTr("Settings")
        }

        TvButtonHint {
            button: swapped ? "A" : "B"
            text: inPopup ? qsTr("Close") : canGoBack ? qsTr("Back") : qsTr("Exit")
        }
    }
}
