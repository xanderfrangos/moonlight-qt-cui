import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

import SystemProperties 1.0
import TvTheme 1.0

NavigableDialog {
    id: dialog

    property alias text: dialogLabel.dialogText
    property alias showSpinner: dialogSpinner.visible
    property alias imageSrc: dialogImage.source

    property string helpText
    property string helpUrl : "https://github.com/moonlight-stream/moonlight-docs/wiki/Troubleshooting"
    property string helpTextSeparator : " "

    onOpened: {
        // Force keyboard focus on the label so keyboard navigation works
        if (dialogButtonBox.count > 0) {
            dialogButtonBox.itemAt(dialogButtonBox.count - 1).forceActiveFocus(Qt.TabFocus)
        }
    }

    RowLayout {
        spacing: 10

        BusyIndicator {
            id: dialogSpinner
            visible: false
            running: visible
        }

        Image {
            id: dialogImage
            source: (standardButtons & Dialog.Yes) ?
                        "qrc:/res/baseline-help_outline-24px.svg" :
                        "qrc:/res/baseline-error_outline-24px.svg"
            sourceSize {
                // The icon should be square so use the height as the width too
                width: SystemProperties.tvMode ? 64 : 50
                height: SystemProperties.tvMode ? 64 : 50
            }
            visible: !showSpinner
        }

        Label {
            property string dialogText

            id: dialogLabel
            text: dialogText + ((helpText && (standardButtons & Dialog.Help)) ? (helpTextSeparator + helpText) : "")
            wrapMode: Text.Wrap
            elide: Label.ElideRight

            // Cap the width so the dialog doesn't grow horizontally forever. This
            // will cause word wrap to kick in.
            Layout.maximumWidth: SystemProperties.tvMode ? 640 : 400
            Layout.maximumHeight: SystemProperties.tvMode ? 640 : 400
        }
    }

    footer: DialogButtonBox {
        id: dialogButtonBox
        standardButtons: dialog.standardButtons

        delegate: Button {
            id: dialogButton

            // In TV mode the focused button is filled with the accent color, so
            // it's always clear what pressing A will do. This follows focus
            // itself rather than the focus ring, which only appears for focus
            // gained through keyboard or gamepad navigation.
            flat: !(SystemProperties.tvMode && activeFocus)
            highlighted: SystemProperties.tvMode && activeFocus
            property bool focusRingFlush: SystemProperties.tvMode

            // Material's six-pixel top and bottom insets leave a strip between
            // the fill and focus outline. Remove them but keep the button's
            // original height so the dialog layout does not shift.
            topInset: SystemProperties.tvMode ? 0 : 6
            bottomInset: SystemProperties.tvMode ? 0 : 6
            implicitHeight: Math.max(implicitBackgroundHeight + (SystemProperties.tvMode ? 12 : topInset + bottomInset),
                                     implicitContentHeight + topPadding + bottomPadding)
            Binding { target: dialogButton.background; property: "radius"; value: TvTheme.focusRingRadius; when: SystemProperties.tvMode }

            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
            Keys.onRightPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
            Keys.onLeftPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        }

        onHelpRequested: {
            Qt.openUrlExternally(helpUrl)
            close()
        }
    }
}
