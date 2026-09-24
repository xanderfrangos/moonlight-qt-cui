import QtQuick 2.9
import QtQuick.Controls 2.2

import SdlGamepadKeyNavigation 1.0
import TvTheme 1.0

// The controllers in the TV mode top bar: a chip for each one with its player
// number and battery, or when the bar is short of room, a single chip with how
// many there are and the lowest battery among them
Row {
    id: status

    property bool condensed: false

    // A single controller keeps its own chip, since summarizing it would
    // save no room
    readonly property bool summarizing: condensed && count > 1

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

    // How wide the chips will be, worked out from the sizes rather than from
    // the laid out chips, so the top bar can choose whether to condense
    // without depending on the result
    readonly property real chipWidth: sizes.horizontalPadding * 2 + sizes.iconSize +
                                      sizes.contentSpacing * 2 + labelMetrics.width + sizes.batteryWidth
    readonly property real fullWidth: count > 0 ? count * chipWidth + (count - 1) * spacing : 0
    readonly property real condensedWidth: count > 1 ? chipWidth + summaryMetrics.width - labelMetrics.width : fullWidth

    spacing: 10

    TvControllerChip {
        id: sizes
        visible: false
    }

    TextMetrics {
        id: labelMetrics
        font.pixelSize: TvTheme.fontLabel
        font.weight: Font.DemiBold
        font.styleName: TvTheme.fontStyleName(TvTheme.bodyFontFamily, font.weight)
        text: "8"
    }

    TextMetrics {
        id: summaryMetrics
        font.pixelSize: TvTheme.fontLabel
        font.weight: Font.DemiBold
        font.styleName: TvTheme.fontStyleName(TvTheme.bodyFontFamily, font.weight)
        text: "×" + status.count
    }

    Repeater {
        model: status.summarizing ? [] : status.controllers

        TvControllerChip {
            // Controllers that aren't passed to the host have no player number
            text: modelData.enabled ? modelData.playerNumber : "–"
            battery: modelData.battery
            dimmed: !modelData.enabled
        }
    }

    TvControllerChip {
        visible: status.summarizing
        text: "×" + status.count
        battery: status.lowestBattery
    }
}
