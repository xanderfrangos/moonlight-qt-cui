pragma Singleton
import QtQuick 2.9

// Colors, sizes, and animation timings for the TV mode look. Desktop mode
// doesn't use these, so it keeps the stock Material appearance.
QtObject {
    // Surfaces, from the window background up to raised cards and dialogs
    readonly property color background: "#0E1016"
    readonly property color backgroundTop: "#161A26"
    readonly property color backgroundBottom: "#0A0C10"
    readonly property color surface: "#171A22"
    readonly property color surfaceRaised: "#1F2330"

    readonly property color textSecondary: "#9AA0AE"

    readonly property int cardRadius: 12
    readonly property int dialogRadius: 16
    readonly property int focusRingRadius: 8

    readonly property int spacingSmall: 8
    readonly property int spacingMedium: 16
    readonly property int spacingLarge: 32

    // Short enough to keep up with held-direction repeats on the gamepad
    readonly property int animationFast: 120
    readonly property int animationNormal: 200
}
