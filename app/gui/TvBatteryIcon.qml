import QtQuick 2.9

import TvTheme 1.0

// Three bars in an outline, like a phone's battery icon. Low levels are drawn
// in the error color.
Rectangle {
    id: icon

    // An SDL_JoystickPowerLevel: -1 unknown, 0 empty, 1 low, 2 medium,
    // 3 full, 4 wired. Only 0 to 3 are drawn.
    property int level: -1

    readonly property bool known: level >= 0 && level <= 3
    readonly property color levelColor: level <= 1 ? TvTheme.statusError : TvTheme.textSecondary

    visible: known
    width: 30
    height: 16
    radius: 4
    color: "transparent"
    border.width: 2
    border.color: levelColor

    Row {
        anchors.centerIn: parent
        spacing: 2

        Repeater {
            model: 3

            Rectangle {
                width: 6
                height: 8
                radius: 1
                color: index < icon.level ? icon.levelColor : TvTheme.stroke
            }
        }
    }
}
