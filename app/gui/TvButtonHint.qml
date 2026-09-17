import QtQuick 2.9
import QtQuick.Controls 2.2

import TvTheme 1.0

// A gamepad face button glyph followed by what the button does
Row {
    // The face button letter: A, B, X, or Y
    property string button
    property string text

    spacing: 10

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: 34
        height: 34
        radius: width / 2
        color: TvTheme.surfaceRaised
        border.width: 2
        border.color: glyphLabel.color

        Label {
            id: glyphLabel
            anchors.centerIn: parent
            text: button
            font.pointSize: 13
            font.bold: true
            color: {
                switch (button) {
                case "A": return "#6CC24A"
                case "B": return "#E5534B"
                case "X": return "#4C9AE8"
                case "Y": return "#F2C14E"
                }
                return "white"
            }
        }
    }

    Label {
        anchors.verticalCenter: parent.verticalCenter
        text: parent.text
        font.pointSize: 15
    }
}
