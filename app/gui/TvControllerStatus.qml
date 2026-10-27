import QtQuick 2.9
import QtQuick.Controls 2.2

import SdlGamepadKeyNavigation 1.0
import TvTheme 1.0

// The count and lowest battery shown inside the Controllers button.
Row {
    id: status

    property bool focused: false

    readonly property var controllers: SdlGamepadKeyNavigation.controllerStatus
    readonly property int count: controllers.length

    // The lowest battery level any controller reports, or -1 if none do
    readonly property int lowestBattery: {
        var lowest = -1
        for (var i = 0; i < controllers.length; i++) {
            var battery = controllers[i].battery
            if (battery >= 0 && battery <= 3 && (lowest < 0 || battery < lowest)) {
                lowest = battery
            }
        }
        return lowest
    }

    // Controllers that aren't passed to the host are dimmed on the
    // Controllers page, so dim the status when none of them are.
    readonly property bool anyEnabled: {
        for (var i = 0; i < controllers.length; i++) {
            if (controllers[i].enabled) {
                return true
            }
        }
        return false
    }

    // The toolbar measures this independently of its current layout.
    readonly property real statusWidth: Math.ceil(countLabel.implicitWidth) +
                                        (batteryIcon.known ? spacing + batteryIcon.width : 0)

    width: statusWidth
    spacing: 12
    opacity: anyEnabled ? 1.0 : 0.5

    Label {
        id: countLabel
        anchors.verticalCenter: parent.verticalCenter
        text: status.count
        color: status.focused ? TvTheme.accentText : TvTheme.textPrimary
        font.pixelSize: TvTheme.fontLabel
        font.weight: Font.DemiBold
    }

    TvBatteryIcon {
        id: batteryIcon
        anchors.verticalCenter: parent.verticalCenter
        level: status.lowestBattery
        normalColor: status.focused ? TvTheme.accentText : TvTheme.textSecondary
        lowColor: status.focused ? TvTheme.accentText : TvTheme.statusError
    }
}
