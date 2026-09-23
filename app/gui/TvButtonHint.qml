import QtQuick 2.9
import QtQuick.Controls 2.2

import SdlGamepadKeyNavigation 1.0
import TvTheme 1.0

// A gamepad face button glyph followed by what the button does. The glyph is
// drawn the way the controller in use labels that button: A/B/X/Y on Xbox,
// the shapes on PlayStation, and Nintendo's swapped letters.
Row {
    // The face button's position, as a ControllerButtonStyle::FacePosition:
    // 0 bottom, 1 right, 2 left, 3 top
    property int position: 0
    property string text

    readonly property color glyphColor: SdlGamepadKeyNavigation.faceButtonColor(SdlGamepadKeyNavigation.buttonStyle, position)

    spacing: 14

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: 44
        height: 44
        radius: width / 2
        color: TvTheme.surfaceRaised
        border.width: 3
        border.color: glyphColor

        Label {
            id: glyphLabel
            anchors.centerIn: parent
            text: SdlGamepadKeyNavigation.faceButtonGlyph(SdlGamepadKeyNavigation.buttonStyle, position)
            font.pixelSize: 20
            font.bold: true
            color: glyphColor
        }
    }

    Label {
        anchors.verticalCenter: parent.verticalCenter
        text: parent.text
        color: TvTheme.textPrimary
        font.pixelSize: TvTheme.fontLabel
    }
}
