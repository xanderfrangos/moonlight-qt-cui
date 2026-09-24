pragma Singleton
import QtQuick 2.9

// Colors, sizes, and animation timings for the TV mode look. Desktop mode
// doesn't use these, so it keeps the stock Material appearance.
//
// Sizes are in logical pixels for a 1080p window, which is what a TV shows at
// its usual GUI scale. Use these rather than picking sizes for each page, so
// every screen reads the same from across the room.
QtObject {
    // Surfaces, from the window background up to raised cards and dialogs
    readonly property color background: "#0B0D12"
    readonly property color backgroundTop: "#11141C"
    readonly property color backgroundBottom: "#0A0C10"
    readonly property color surface: "#151923"
    readonly property color surfaceRaised: "#1E2331"
    readonly property color stroke: "#2B3142"

    readonly property color textPrimary: "#EEF1F6"
    readonly property color textSecondary: "#A7AEBD"
    readonly property color textTertiary: "#858DA0"

    // The one accent, for focus and primary actions. Text drawn on it uses
    // accentText, since white doesn't contrast with it.
    readonly property color accent: "#8AA4FF"
    readonly property color accentText: "#0B0D12"
    readonly property color focusGlow: "#478AA4FF"

    // Only ever used to mean status
    readonly property color statusOnline: "#52C48A"
    readonly property color statusAttention: "#F2B84B"
    readonly property color statusError: "#F0707A"
    readonly property color statusInactive: "#858DA0"

    // Type scale, as pixel sizes. Nothing on a TV screen is smaller than
    // fontCaption.
    readonly property int fontDisplay: 54
    readonly property int fontTitle: 39
    readonly property int fontBody: 28
    readonly property int fontLabel: 24
    readonly property int fontCaption: 21

    // Spacing scale
    readonly property int spacingSmall: 8
    readonly property int spacingMedium: 16
    readonly property int spacingMediumLarge: 24
    readonly property int spacingLarge: 32
    readonly property int spacingXLarge: 48

    // Content stays this far in from the window edges, as a fraction of the
    // window size, so TVs that overscan don't crop it
    readonly property real safeAreaHorizontal: 0.0375
    readonly property real safeAreaVertical: 0.04

    // Focus: an accent ring with a soft glow outside it, and focused cards
    // lift a little
    readonly property int focusRingWidth: 3
    readonly property int focusGlowWidth: 6
    readonly property real focusScale: 1.06

    // Pages extend this far past the safe area, and keep their content this
    // far in from their own edges. The content lines up with the top and
    // bottom bars, and focused items have room to grow and glow without
    // being clipped at the page edge.
    readonly property int focusBleed: 24

    // Box art and small cards
    readonly property int cardRadius: 12
    // Larger tiles, like the computer cards
    readonly property int tileRadius: 24
    readonly property int dialogRadius: 29
    readonly property int focusRingRadius: 8

    // Dialogs: a fixed content width so they don't change size with their
    // text, full width buttons stacked with the primary one on top, and a
    // dark scrim over the page behind them. Dialogs are a size down from the
    // page type scale, so they don't overwhelm the page they're over.
    readonly property int dialogPadding: 43
    readonly property int dialogSpacing: 17
    readonly property int dialogContentWidth: 538
    readonly property int dialogIconSize: 67
    readonly property int dialogTitleFont: 31
    readonly property int dialogBodyFont: 22
    readonly property int dialogButtonHeight: 67
    readonly property int dialogButtonRadius: 19
    readonly property color scrim: "#B8050609"

    // Top bar buttons and status chips are pills this tall
    readonly property int pillHeight: 66
    readonly property int pillIconSize: 30
    readonly property int pillPadding: 24

    // Packs a color as the eight hex digits of ARGB that the image
    // providers parse out of their URLs
    function argbHex(color) {
        function channel(value) {
            var hex = Math.round(value * 255).toString(16)
            return hex.length < 2 ? "0" + hex : hex
        }

        return channel(color.a) + channel(color.r) + channel(color.g) + channel(color.b)
    }

    // Short enough to keep up with held-direction repeats on the gamepad
    readonly property int animationFast: 120
    readonly property int animationNormal: 200
}
