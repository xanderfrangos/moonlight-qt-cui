import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2

// A Slider that only changes its value from the keyboard or gamepad after the
// user explicitly enters editing mode with Space/Return (the A button). While
// not editing, arrow keys move focus instead of silently changing the value.
Slider {
    id: control

    property bool editing: false

    // Walk up the parent chain to find a view that implements spatial
    // navigation (e.g. SettingsView) and let it move focus.
    function forwardHorizontalNavigation(forward) {
        for (var item = parent; item; item = item.parent) {
            if (typeof item.navigateHorizontally === "function") {
                item.navigateHorizontally(control, forward)
                return
            }
        }
    }

    onActiveFocusChanged: {
        if (!activeFocus) {
            editing = false
        }
    }

    Keys.onSpacePressed: {
        editing = !editing
    }

    Keys.onReturnPressed: {
        editing = !editing
    }

    Keys.onEnterPressed: {
        editing = !editing
    }

    Keys.onEscapePressed: {
        if (editing) {
            editing = false
        }
        else {
            // Let the page handle Escape (go back)
            event.accepted = false
        }
    }

    // When editing, leave the arrow keys unaccepted so the Slider itself
    // adjusts the value (and emits moved()).
    Keys.onLeftPressed: {
        if (editing) {
            event.accepted = false
        }
        else {
            forwardHorizontalNavigation(false)
        }
    }

    Keys.onRightPressed: {
        if (editing) {
            event.accepted = false
        }
        else {
            forwardHorizontalNavigation(true)
        }
    }

    Keys.onUpPressed: {
        event.accepted = !editing
    }

    Keys.onDownPressed: {
        event.accepted = !editing
    }

    Rectangle {
        anchors.fill: parent
        z: -1
        visible: control.editing
        color: "transparent"
        radius: 4
        border.width: 2
        border.color: control.Material.accentColor
    }
}
