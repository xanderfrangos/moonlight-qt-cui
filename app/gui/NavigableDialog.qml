import QtQuick 2.0
import QtQuick.Controls 2.5

Dialog {
    modal: true
    anchors.centerIn: Overlay.overlay

    // What had focus when the dialog opened, so we can return to it
    property Item focusItemBeforeOpen: null
    property bool focusItemHadVisualFocus: false

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
