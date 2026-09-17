import QtQuick 2.9
import QtQuick.Controls 2.2

import SystemProperties 1.0
import TvTheme 1.0

// A section of the settings page. In TV mode it is drawn as a filled card
// instead of an outlined frame, with more room around its contents.
// Bindings are used so desktop mode keeps the style's own values.
GroupBox {
    id: groupBox

    Binding {
        target: groupBox
        property: "padding"
        value: 20
        when: SystemProperties.tvMode
    }

    Binding {
        target: groupBox.background
        property: "color"
        value: TvTheme.surface
        when: SystemProperties.tvMode
    }

    Binding {
        target: groupBox.background
        property: "radius"
        value: TvTheme.cardRadius
        when: SystemProperties.tvMode
    }

    Binding {
        target: groupBox.background
        property: "border.width"
        value: 0
        when: SystemProperties.tvMode
    }
}
