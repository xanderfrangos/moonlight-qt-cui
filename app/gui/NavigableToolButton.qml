import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2
import QtQuick.Layouts 1.3

import SystemProperties 1.0
import TvTheme 1.0

ToolButton {
    id: button

    property string iconSource

    // TV mode draws the button as a pill with this label beside the icon.
    // Buttons without one are round and show only the icon.
    property string tvLabel: ""

    // Set false when the top bar is too narrow for every label. The focused
    // button still shows its label, so it's clear what A will do.
    property bool tvShowLabel: true

    readonly property bool tvLabelVisible: tvLabel !== "" && (tvShowLabel || visualFocus)
    // In TV mode, both labelled and icon-only toolbar buttons are capsules.
    readonly property bool focusRingPill: SystemProperties.tvMode

    activeFocusOnTab: true

    icon.source: iconSource
    icon.width: SystemProperties.tvMode ? TvTheme.pillIconSize : background.width
    icon.height: SystemProperties.tvMode ? TvTheme.pillIconSize : background.height

    // This determines the size of the Material highlight. We increase it
    // from the default because we use larger than normal icons for TV readability.
    Layout.preferredHeight: SystemProperties.tvMode ? TvTheme.pillHeight : parent.height
    Layout.preferredWidth: SystemProperties.tvMode && !tvLabelVisible ? TvTheme.pillHeight : -1

    Binding {
        target: button
        property: "text"
        value: button.tvLabel
        when: SystemProperties.tvMode
    }

    // TV mode draws its own pill under the content instead of the Material ripple
    Binding {
        target: button.background
        property: "visible"
        value: false
        when: SystemProperties.tvMode
    }

    Rectangle {
        visible: SystemProperties.tvMode
        z: -1
        anchors.fill: parent
        radius: height / 2
        color: button.visualFocus ? TvTheme.accent :
               button.down || button.hovered ? TvTheme.stroke : TvTheme.surfaceRaised
        Behavior on color { ColorAnimation { duration: TvTheme.animationFast } }
    }

    Binding {
        target: button
        property: "display"
        value: button.tvLabelVisible ? AbstractButton.TextBesideIcon : AbstractButton.IconOnly
        when: SystemProperties.tvMode
    }

    Component.onCompleted: {
        if (SystemProperties.tvMode) {
            // The focused pill is filled with the accent, with the label and
            // icon drawn in accentText. Material colors both from its foreground.
            button.Material.foreground = Qt.binding(function() {
                return button.visualFocus ? TvTheme.accentText : TvTheme.textPrimary
            })

            font.pixelSize = TvTheme.fontLabel
            font.weight = Font.DemiBold
            spacing = 12
            leftPadding = rightPadding = tvLabel !== "" ? TvTheme.pillPadding : 0
            topPadding = bottomPadding = 0
        }
    }

    Keys.onReturnPressed: {
        clicked()
    }

    Keys.onEnterPressed: {
        clicked()
    }

    Keys.onRightPressed: {
        nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
    }

    Keys.onLeftPressed: {
        nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
    }
}
