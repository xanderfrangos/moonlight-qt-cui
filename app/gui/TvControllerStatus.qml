import QtQuick 2.9
import QtQuick.Controls 2.2

import SdlGamepadKeyNavigation 1.0
import TvTheme 1.0

// The controllers in the TV mode top bar: one chip with how many there are and
// the lowest battery among them. The Controllers page lists each one.
TvControllerChip {
    id: status

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
    // Controllers page, so dim the chip when none of them are
    readonly property bool anyEnabled: {
        for (var i = 0; i < controllers.length; i++) {
            if (controllers[i].enabled) {
                return true
            }
        }
        return false
    }

    // How wide the chip will be, worked out from the sizes rather than from
    // the laid out chip, so the top bar can plan its layout without depending
    // on the result
    readonly property real chipWidth: horizontalPadding * 2 + iconSize +
                                      contentSpacing * 2 + labelMetrics.width + batteryWidth

    text: count
    battery: lowestBattery
    dimmed: !anyEnabled

    TextMetrics {
        id: labelMetrics
        font.pixelSize: TvTheme.fontLabel
        font.weight: Font.DemiBold
        text: status.text
    }
}
