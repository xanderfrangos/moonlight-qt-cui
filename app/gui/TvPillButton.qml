import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.impl 2.12

import TvTheme 1.0

// A pill shaped button with an optional icon, matching the TV mode top bar.
// The focused one is filled with the accent. It leaves the arrow keys to the
// page, which decides where focus goes next.
Button {
    id: button

    property url iconSource
    // Lets one arrow icon point any way
    property real iconRotation: 0

    // The focus ring follows the pill, a pixel out from it like the top bar's
    readonly property bool focusRingPill: true

    readonly property bool filled: activeFocus
    readonly property color contentColor: !enabled ? TvTheme.textTertiary :
                                          filled ? TvTheme.accentText : TvTheme.textPrimary

    // The Material style's insets would shrink the pill inside the area the
    // focus ring surrounds, and its padding would push an icon off center
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    padding: 0
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0

    implicitHeight: 56
    implicitWidth: text !== "" ? contentRow.implicitWidth + 2 * TvTheme.spacingMediumLarge : implicitHeight
    font.pixelSize: TvTheme.fontLabel
    font.weight: Font.DemiBold

    contentItem: Item {
        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: 10

            IconImage {
                visible: button.iconSource != ""
                anchors.verticalCenter: parent.verticalCenter
                source: button.iconSource
                sourceSize.width: 26
                sourceSize.height: 26
                rotation: button.iconRotation
                color: button.contentColor
            }

            Label {
                visible: button.text !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: button.text
                font: button.font
                color: button.contentColor
            }
        }
    }

    background: Rectangle {
        radius: height / 2
        color: button.filled ? TvTheme.accent :
               button.down || button.hovered ? TvTheme.stroke : TvTheme.surfaceRaised
        opacity: button.enabled ? 1.0 : 0.5
        Behavior on color { ColorAnimation { duration: TvTheme.animationFast } }
    }

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()
}
