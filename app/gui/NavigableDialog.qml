import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Controls.Material 2.2
import QtQuick.Layouts 1.2

import SystemProperties 1.0
import TvTheme 1.0

Dialog {
    id: navigableDialog
    modal: true
    anchors.centerIn: Overlay.overlay

    // What had focus when the dialog opened, so we can return to it
    property Item focusItemBeforeOpen: null
    property bool focusItemHadVisualFocus: false

    // TV mode: labels that say what the buttons do, in place of Yes, No, OK
    // and Cancel
    property string acceptText: ""
    property string rejectText: ""

    // TV mode: accepting can't be undone, like quitting or deleting. The
    // button that backs out goes first, so it has focus when the dialog
    // opens, and the accept button is drawn in the error color.
    property bool destructive: false

    // Whether the OK or Yes button can be pressed, such as while the text
    // entered is valid. Use this rather than standardButton(), since TV mode
    // draws its own buttons.
    property bool acceptEnabled: true

    // TV mode stacks the buttons full width under the content, primary
    // action first
    readonly property var tvButtons: {
        var accept = null
        var reject = null
        if (standardButtons & Dialog.Yes) {
            accept = { "role": "accept", "text": acceptText || qsTr("Yes") }
        }
        else if (standardButtons & Dialog.Ok) {
            accept = { "role": "accept", "text": acceptText || qsTr("OK") }
        }
        if (standardButtons & Dialog.No) {
            reject = { "role": "reject", "text": rejectText || qsTr("No") }
        }
        else if (standardButtons & Dialog.Cancel) {
            reject = { "role": "reject", "text": rejectText || qsTr("Cancel") }
        }

        var buttons = []
        if (destructive) {
            if (reject) buttons.push(reject)
            if (accept) buttons.push(accept)
        }
        else {
            if (accept) buttons.push(accept)
            if (reject) buttons.push(reject)
        }
        if (standardButtons & Dialog.Help) {
            buttons.push({ "role": "help", "text": qsTr("Help") })
        }
        return buttons
    }

    // What the TV mode Help button does
    signal tvHelpRequested()

    // Focuses the first button: the primary one, or the safe one when
    // accepting can't be undone
    function focusFirstButton() {
        if (SystemProperties.tvMode) {
            if (footer && footer.firstButton) {
                footer.firstButton.forceActiveFocus(Qt.TabFocus)
            }
        }
        else if (footer && footer.count > 0) {
            footer.itemAt(0).forceActiveFocus(Qt.TabFocus)
        }
    }

    onAcceptEnabledChanged: {
        // Desktop mode uses the style's button box
        if (!SystemProperties.tvMode && standardButton) {
            var button = standardButton(Dialog.Ok) || standardButton(Dialog.Yes)
            if (button) {
                button.enabled = acceptEnabled
            }
        }
    }

    Component.onCompleted: {
        // TV mode replaces the style's header and button box with its own.
        // Replacing them here, after every declared property is set, also
        // covers dialogs that declare a footer of their own.
        if (SystemProperties.tvMode) {
            header = tvHeaderComponent.createObject(navigableDialog.contentItem)
            footer = tvFooterComponent.createObject(navigableDialog.contentItem)
        }
    }

    // TV mode: the dialog's title, if it has one, as a headline
    Component {
        id: tvHeaderComponent

        Label {
            visible: navigableDialog.title !== ""
            text: navigableDialog.title
            color: TvTheme.textPrimary
            font.pixelSize: TvTheme.dialogTitleFont
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
            leftPadding: TvTheme.dialogPadding
            rightPadding: TvTheme.dialogPadding
            topPadding: TvTheme.dialogPadding
        }
    }

    // TV mode: full width buttons, stacked
    Component {
        id: tvFooterComponent

        Item {
            readonly property Item firstButton: buttonRepeater.count > 0 ? buttonRepeater.itemAt(0) : null

            visible: buttonRepeater.count > 0
            implicitWidth: buttonColumn.implicitWidth + 2 * TvTheme.dialogPadding
            implicitHeight: buttonColumn.implicitHeight + TvTheme.dialogPadding

            ColumnLayout {
                id: buttonColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: TvTheme.dialogPadding
                anchors.rightMargin: TvTheme.dialogPadding
                // Wide enough that the focused button's ring and glow stay clear of
                // its neighbors
                spacing: TvTheme.dialogSpacing + 1

                Repeater {
                    id: buttonRepeater
                    model: navigableDialog.tvButtons

                    TvDialogButton {
                        Layout.fillWidth: true
                        text: modelData.text
                        destructive: navigableDialog.destructive && modelData.role === "accept"
                        enabled: modelData.role !== "accept" || navigableDialog.acceptEnabled
                        onClicked: {
                            if (modelData.role === "accept") {
                                navigableDialog.accept()
                            }
                            else if (modelData.role === "reject") {
                                navigableDialog.reject()
                            }
                            else {
                                navigableDialog.tvHelpRequested()
                            }
                        }
                    }
                }
            }
        }
    }

    // TV mode: larger text, more room, and rounder corners. Bindings are used
    // so desktop mode keeps the style's own values.
    Binding {
        target: navigableDialog
        property: "font.pixelSize"
        value: TvTheme.dialogBodyFont
        when: SystemProperties.tvMode
    }

    Binding {
        target: navigableDialog
        property: "padding"
        value: TvTheme.dialogPadding
        when: SystemProperties.tvMode
    }

    // The Material style sets a smaller top padding of its own. Under a
    // headline, the content only needs to be spaced from it.
    Binding {
        target: navigableDialog
        property: "topPadding"
        value: navigableDialog.title !== "" ? TvTheme.dialogSpacing : TvTheme.dialogPadding
        when: SystemProperties.tvMode
    }

    // The buttons are spaced from the content like the content's own parts
    Binding {
        target: navigableDialog
        property: "spacing"
        value: TvTheme.dialogSpacing
        when: SystemProperties.tvMode
    }

    Binding {
        target: navigableDialog
        property: "bottomPadding"
        value: TvTheme.dialogSpacing
        when: SystemProperties.tvMode
    }

    // A fixed width, so dialogs don't change size with their text
    Binding {
        target: navigableDialog
        property: "contentWidth"
        value: TvTheme.dialogContentWidth
        when: SystemProperties.tvMode
    }

    Binding {
        target: navigableDialog.background
        property: "radius"
        value: TvTheme.dialogRadius
        when: SystemProperties.tvMode
    }

    // Lift the dialog off the dark scrim
    Binding {
        target: navigableDialog.background
        property: "color"
        value: TvTheme.surfaceRaised
        when: SystemProperties.tvMode
    }

    // TV mode: a dark scrim, since the style's light one is glaring on a dark
    // TV screen. Otherwise this matches the Material style's own.
    Overlay.modal: Rectangle {
        color: SystemProperties.tvMode ? TvTheme.scrim : navigableDialog.Material.backgroundDimColor
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
