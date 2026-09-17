import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Controls.Material 2.2

import SystemProperties 1.0
import TvTheme 1.0

Dialog {
    id: navigableDialog
    modal: true
    anchors.centerIn: Overlay.overlay

    // What had focus when the dialog opened, so we can return to it
    property Item focusItemBeforeOpen: null
    property bool focusItemHadVisualFocus: false

    // TV mode: larger text, more room, and rounder corners. Bindings are used
    // so desktop mode keeps the style's own values.
    Binding {
        target: navigableDialog
        property: "font.pointSize"
        value: 15
        when: SystemProperties.tvMode
    }

    Binding {
        target: navigableDialog
        property: "padding"
        value: 32
        when: SystemProperties.tvMode
    }

    Binding {
        target: navigableDialog.background
        property: "radius"
        value: TvTheme.dialogRadius
        when: SystemProperties.tvMode
    }

    // Lift the dialog off the dark scrim. None of our dialogs have a title,
    // whose header would still use the style's color.
    Binding {
        target: navigableDialog.background
        property: "color"
        value: TvTheme.surfaceRaised
        when: SystemProperties.tvMode
    }

    // The button box draws its own background, with the style's color and
    // corner radius
    Binding {
        target: navigableDialog.footer ? navigableDialog.footer.background : null
        property: "color"
        value: TvTheme.surfaceRaised
        when: SystemProperties.tvMode && navigableDialog.footer !== null
    }

    Binding {
        target: navigableDialog.footer ? navigableDialog.footer.background : null
        property: "radius"
        value: TvTheme.dialogRadius
        when: SystemProperties.tvMode && navigableDialog.footer !== null
    }

    // TV mode: a dark scrim, since the style's light one is glaring on a dark
    // TV screen. Otherwise this matches the Material style's own.
    Overlay.modal: Rectangle {
        color: SystemProperties.tvMode ? "#B3000000" : navigableDialog.Material.backgroundDimColor
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }

    onAboutToShow: {
        focusItemBeforeOpen = window.activeFocusItem
        focusItemHadVisualFocus = focusItemBeforeOpen !== null && focusItemBeforeOpen.visualFocus === true
    }

    onClosed: {
        // We must force focus back to the last item. If we don't,
        // gamepad and keyboard navigation will break after a
        // dialog appears.
        restoreFocus(focusItemBeforeOpen, focusItemHadVisualFocus)
        focusItemBeforeOpen = null
    }
}
