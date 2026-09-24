import QtQuick 2.0
import QtQuick.Controls 2.2

import SystemProperties 1.0
import TvTheme 1.0

Menu {
    id: navigableMenu

    property var initiator

    // For menus opened with a keyboard or gamepad. TV mode centers the menu
    // on the card that opened it instead of putting it at its top left.
    function openCentered() {
        if (SystemProperties.tvMode) {
            // The card may be scaled but the menu isn't, so the center is found
            // in window coordinates and only the menu's corner is mapped back.
            // The position is bound since the menu's size isn't final until it
            // opens, and popup() replaces the bindings when a mouse opens it.
            var center = initiator.mapToItem(null, initiator.width / 2, initiator.height / 2)
            x = Qt.binding(function() { return Math.round(parent.mapFromItem(null, center.x - width / 2, 0).x) })
            y = Qt.binding(function() { return Math.round(parent.mapFromItem(null, 0, center.y - height / 2).y) })
        }
        open()
    }

    // TV mode: larger items and rounder corners. Bindings are used so desktop
    // mode keeps the style's own values.
    Binding {
        target: navigableMenu
        property: "font.pointSize"
        value: 15
        when: SystemProperties.tvMode
    }

    Binding {
        target: navigableMenu.background
        property: "radius"
        value: TvTheme.cardRadius
        when: SystemProperties.tvMode
    }

    onOpened: {
        // If the initiating object currently has keyboard focus,
        // give focus to the first visible and enabled menu item
        if (initiator.focus) {
            for (var i = 0; i < count; i++) {
                var item = itemAt(i)
                if (item.visible && item.enabled) {
                    item.forceActiveFocus(Qt.TabFocusReason)
                    break
                }
            }
        }
    }
}
