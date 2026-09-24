import QtQuick 2.9
import QtQuick.Controls 2.2

import TvTheme 1.0

// A full width TV mode dialog button. The focused one is filled with the
// accent, so it's always clear what pressing A will do. This follows focus
// itself rather than the focus ring, which only appears for focus gained
// through keyboard or gamepad navigation.
Button {
    id: button

    // Drawn in the error color, for actions like quitting or deleting
    property bool destructive: false

    readonly property bool filled: activeFocus

    // The focus ring follows the button's rounded corners, a pixel out from
    // it like the top bar's
    readonly property int focusRingRadius: TvTheme.dialogButtonRadius

    // The Material style's insets would shrink the button inside the area
    // the focus ring surrounds
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0

    implicitHeight: TvTheme.dialogButtonHeight
    font.pixelSize: TvTheme.dialogBodyFont
    font.weight: Font.DemiBold

    contentItem: Label {
        text: button.text
        font: button.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        color: button.filled ? TvTheme.accentText :
               button.destructive ? TvTheme.statusError : TvTheme.textPrimary
    }

    background: Rectangle {
        radius: TvTheme.dialogButtonRadius
        color: button.filled ? TvTheme.accent :
               button.down || button.hovered ? TvTheme.stroke : "transparent"
        border.width: button.filled ? 0 : 2
        border.color: TvTheme.stroke
        Behavior on color { ColorAnimation { duration: TvTheme.animationFast } }
    }

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    // The buttons are stacked, so up and down move between them
    Keys.onUpPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
    Keys.onDownPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
}
