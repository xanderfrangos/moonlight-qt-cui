import QtQuick 2.9
import QtQuick.Controls 2.2

import TvTheme 1.0

// A controller in the TV mode top bar: a gamepad icon, a short label (the
// player number), and the battery level when the controller reports one
Rectangle {
    id: chip

    property string text
    // An SDL_JoystickPowerLevel: -1 unknown, 0 empty, 1 low, 2 medium,
    // 3 full, 4 wired. Only 0 to 3 are drawn.
    property int battery: -1
    property bool dimmed: false


    // Sizes the top bar uses to plan its layout before the chips exist
    readonly property int horizontalPadding: 18
    readonly property int contentSpacing: 12
    readonly property int iconSize: 30
    readonly property int batteryWidth: 30

    implicitHeight: 54
    implicitWidth: row.implicitWidth + horizontalPadding * 2
    radius: height / 2
    color: TvTheme.surface
    opacity: dimmed ? 0.5 : 1.0

    Row {
        id: row
        anchors.centerIn: parent
        spacing: chip.contentSpacing

        Image {
            anchors.verticalCenter: parent.verticalCenter
            source: "qrc:/res/ic_videogame_asset_white_48px.svg"
            sourceSize.width: chip.iconSize
            sourceSize.height: chip.iconSize
            opacity: 0.7
        }

        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: chip.text
            color: TvTheme.textPrimary
            font.pixelSize: TvTheme.fontLabel
            font.weight: Font.DemiBold
        }

        TvBatteryIcon {
            anchors.verticalCenter: parent.verticalCenter
            width: chip.batteryWidth
            level: chip.battery
        }
    }
}
