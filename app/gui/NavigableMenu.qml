import QtQuick 2.0
import QtQuick.Controls 2.2

import SystemProperties 1.0
import TvTheme 1.0

Menu {
    id: navigableMenu

    property var initiator

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
